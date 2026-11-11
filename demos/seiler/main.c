/* seiler — Seiler's interpolation: a cubic Bezier curve from three lerps.
 *
 *   Draw -> Present          (the method runs in C: method.c)
 *
 * Yuksel: "Seiler's Interpolation for Evaluating Polynomial Curves" (SIGGRAPH
 * Talks 2024). Scene 0 shows the construction of the talk's figure 1 on a
 * curve of degree 2 to 5; scene 1 a uniform Catmull-Rom spline stored as one
 * knot and one Seiler point a sample, the form a texture filter can evaluate.
 *
 * --check compares every form with the Bernstein polynomials for degrees 2 to
 * 5, and tests what the talk says about Catmull-Rom splines and about splits.
 */
#include "canvas.h"
#include "method.h"

#include <math.h>
#include <time.h>

enum { KNOTS = 9 };

typedef struct App {
    Demo     demo;   /* first: see demo.h */
    Canvas   canvas;
    uint32_t scene, degree, frame;
    float    at;      /* the parameter the construction is shown for; 0 = it moves */
} App;

static const SeilerPoint k_curve[SEILER_MAX + 1][SEILER_MAX + 1] = {
    {{0}}, {{0}},
    {{-1.5, -0.6}, {0.0, 1.3}, {1.5, -0.6}},
    {{-1.5, -0.6}, {-1.1, 0.5}, {0.9, 0.7}, {1.5, -0.6}},
    {{-1.5, -0.6}, {-1.3, 0.2}, {0.0, 1.0}, {1.2, 0.3}, {1.5, -0.6}},
    {{-1.5, -0.6}, {-1.4, 0.0}, {-0.6, 0.9}, {0.5, 0.2}, {1.3, 0.1}, {1.5, -0.6}}};
static const SeilerPoint k_knots[KNOTS] = {{-1.6, -0.3}, {-1.2, 0.5}, {-0.8, 0.1}, {-0.4, -0.5}, {0.0, 0.6},
                                           {0.4, 0.7}, {0.8, -0.2}, {1.2, 0.0}, {1.6, 0.4}};

static void line(Canvas *c, const SeilerPoint p, const SeilerPoint q, float z, float width, uint32_t rgba) {
    const float a[3] = {(float)p[0], (float)p[1], z}, b[3] = {(float)q[0], (float)q[1], z};
    canvas_line(c, a, b, width, rgba);
}

static void dot(Canvas *c, const SeilerPoint p, float radius, uint32_t rgba) {
    const float a[3] = {(float)p[0], (float)p[1], 0.5f};
    canvas_dot(c, a, radius, rgba);
}

static void app_ui(App *a, struct nk_context *ctx) {
    static const char *const k_scenes[2] = {"One curve", "Catmull-Rom spline"};
    if (nk_begin(ctx, "Seiler's interpolation", nk_rect(16, 16, 300, 150), NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_TITLE)) {
        int degree = (int)a->degree;
        demo_ui_choice(ctx, "Scene", k_scenes, 2u, &a->scene);
        nk_layout_row_dynamic(ctx, 22, 1);
        nk_property_int(ctx, "Degree", 2, &degree, SEILER_MAX, 1, 1.0f);
        a->degree = (uint32_t)degree;
        demo_ui_float(ctx, "t (0 moves)", &a->at, 0.0f, 1.0f);
    }
    nk_end(ctx);
}

static void app_frame(App *a) {
    Canvas        *c = &a->canvas;
    const int      degree = (int)a->degree;
    const double   t = a->at > 0.0f ? (double)a->at : 0.5 - 0.5 * cos((double)a->frame * 0.02);
    const uint32_t grey = CANVAS_RGB(120, 126, 140), ink = CANVAS_RGB(235, 235, 240), red = CANVAS_RGB(235, 95, 70),
                   blue = CANVAS_RGB(90, 160, 235), green = CANVAS_RGB(110, 200, 120);
    SeilerPoint    d[SEILER_MAX + 1], left[SEILER_MAX], right[SEILER_MAX], p, q, ends, inner;
    float          unit;
    int            i;

    if (a->scene == 0u) {
        const SeilerPoint *b = k_curve[degree];
        double             low[2] = {b[0][0], b[0][1]}, high[2] = {b[0][0], b[0][1]};
        seiler_differences(b, degree, d);
        seiler_points(d, degree, left, right);
        /* Seiler points lie far outside the control polygon: the view takes in both */
        for (i = 0; i < 2; ++i) {
            int n;
            for (n = 0; n <= degree; ++n) { low[i] = fmin(low[i], b[n][i]); high[i] = fmax(high[i], b[n][i]); }
            low[i]  = fmin(low[i], fmin(left[1][i], right[1][i]));
            high[i] = fmax(high[i], fmax(left[1][i], right[1][i]));
        }
        unit = (float)(0.56 * fmax(high[1] - low[1], (high[0] - low[0]) * (double)a->demo.height / (double)a->demo.width));
        canvas_view_flat(&a->demo, c, (float)(0.5 * (low[0] + high[0])), (float)(0.5 * (low[1] + high[1])), unit);
        unit /= 1.6f;   /* widths below are for a view 1.6 high */
        for (i = 0; i < degree; ++i) line(c, b[i], b[i + 1], 0.1f, unit * 0.012f, grey);
        for (i = 0; i <= degree; ++i) dot(c, b[i], unit * 0.025f, grey);
        /* the two polygons the lerps run along: end points, and the first Seiler points */
        line(c, b[0], b[degree], 0.2f, unit * 0.012f, blue);
        line(c, left[1], right[1], 0.2f, unit * 0.012f, red);
        line(c, b[0], left[1], 0.15f, unit * 0.010f, red);
        line(c, b[degree], right[1], 0.15f, unit * 0.010f, red);
        dot(c, left[1], unit * 0.03f, red);
        dot(c, right[1], unit * 0.03f, red);
        seiler_from_points(left, right, degree, 0.0, p);
        for (i = 1; i <= 128; ++i) {
            seiler_from_points(left, right, degree, (double)i / 128.0, q);
            line(c, p, q, 0.3f, unit * 0.022f, ink);
            p[0] = q[0]; p[1] = q[1];
        }
        /* at t: the point between the ends, the point between the Seiler points, and the curve
         * point (1 - t) t of the way from the first to the second (exact for degree 2 and 3;
         * higher degrees have more levels inside the second point) */
        for (i = 0; i < 2; ++i) {
            ends[i]  = b[0][i] + t * (b[degree][i] - b[0][i]);
            inner[i] = left[1][i] + t * (right[1][i] - left[1][i]);
        }
        seiler_from_points(left, right, degree, t, p);
        line(c, ends, inner, 0.4f, unit * 0.012f, green);
        dot(c, ends, unit * 0.028f, blue);
        dot(c, inner, unit * 0.028f, red);
        dot(c, p, unit * 0.04f, green);
    } else {
        SeilerPoint seiler[KNOTS];
        canvas_view_flat(&a->demo, c, 0.0f, 0.1f, 1.5f);
        seiler_spline_points(k_knots, KNOTS, seiler);
        for (i = 0; i + 1 < KNOTS; ++i) {
            line(c, k_knots[i], k_knots[i + 1], 0.2f, 0.012f, blue);
            line(c, seiler[i], seiler[i + 1], 0.2f, 0.012f, red);
        }
        seiler_spline(k_knots, seiler, KNOTS, 0.0, p);
        for (i = 1; i <= 320; ++i) {
            seiler_spline(k_knots, seiler, KNOTS, (double)i / 40.0, q);
            line(c, p, q, 0.3f, 0.022f, ink);
            p[0] = q[0]; p[1] = q[1];
        }
        for (i = 0; i < KNOTS; ++i) { dot(c, k_knots[i], 0.03f, blue); dot(c, seiler[i], 0.025f, red); }
        {
            const double x = t * (double)(KNOTS - 1), f = x - floor(fmin(x, (double)KNOTS - 1.001));
            const int    piece = (int)fmin(x, (double)KNOTS - 1.001);
            for (i = 0; i < 2; ++i) {
                ends[i]  = k_knots[piece][i] + f * (k_knots[piece + 1][i] - k_knots[piece][i]);
                inner[i] = seiler[piece][i] + f * (seiler[piece + 1][i] - seiler[piece][i]);
            }
            seiler_spline(k_knots, seiler, KNOTS, x, p);
            line(c, ends, inner, 0.4f, 0.012f, green);
            dot(c, ends, 0.028f, blue);
            dot(c, inner, 0.028f, red);
            dot(c, p, 0.04f, green);
        }
    }
    canvas_frame(&a->demo, c);
    a->frame++;
}

static double random01(uint32_t *state) {
    *state = *state * 1664525u + 1013904223u;
    return (double)(*state >> 8) / 16777216.0;
}

static double apart(const SeilerPoint a, const SeilerPoint b) { return fmax(fabs(a[0] - b[0]), fabs(a[1] - b[1])); }

static void app_check(App *a) {
    Demo    *d = &a->demo;
    uint32_t state = 7u;
    int      degree, trial, i, k;

    /* 1. every form against the Bernstein polynomials, control points in [-1, 1]^2 */
    for (degree = 2; degree <= SEILER_MAX; ++degree) {
        double worst[4] = {0};
        char   label[160];
        for (trial = 0; trial < 500; ++trial) {
            SeilerPoint b[SEILER_MAX + 1], diff[SEILER_MAX + 1], left[SEILER_MAX], right[SEILER_MAX], want, got;
            for (i = 0; i <= degree; ++i)
                for (k = 0; k < 2; ++k) b[i][k] = 2.0 * random01(&state) - 1.0;
            seiler_differences(b, degree, diff);
            seiler_points(diff, degree, left, right);
            for (i = 0; i <= 20; ++i) {
                const double t = (double)i / 20.0;
                seiler_bernstein(b, degree, t, want);
                seiler_from_differences(diff, degree, t, got); worst[0] = fmax(worst[0], apart(want, got));
                seiler_from_points(left, right, degree, t, got); worst[1] = fmax(worst[1], apart(want, got));
                seiler_from_offsets(diff, degree, t, got);     worst[2] = fmax(worst[2], apart(want, got));
                seiler_casteljau(b, degree, t, got);           worst[3] = fmax(worst[3], apart(want, got));
            }
        }
        snprintf(label, sizeof label, "degree %d, 500 curves x 21 parameters: difference terms - Bernstein", degree);
        demo_check(d, label, worst[0], "< 1e-13", worst[0] < 1e-13);
        demo_check(d, "  Seiler points, lerps only", worst[1], "< 1e-13", worst[1] < 1e-13);
        demo_check(d, "  offsets and one lerp", worst[2], "< 1e-13", worst[2] < 1e-13);
        demo_check(d, "  de Casteljau", worst[3], "< 1e-13", worst[3] < 1e-13);
    }

    /* 2. the cubic in the page's words: s1 = 3 b1 - b0 - b3, s2 = 3 b2 - b3 - b0; and the
     * polynomial form of the talk's eq. 2 */
    {
        double worst = 0.0, poly = 0.0;
        for (trial = 0; trial < 500; ++trial) {
            SeilerPoint b[4], diff[4], left[2], right[2], want, got;
            for (i = 0; i < 4; ++i)
                for (k = 0; k < 2; ++k) b[i][k] = 2.0 * random01(&state) - 1.0;
            seiler_differences(b, 3, diff);
            seiler_points(diff, 3, left, right);
            for (k = 0; k < 2; ++k) {
                worst = fmax(worst, fabs(left[1][k] - (3.0 * b[1][k] - b[0][k] - b[3][k])));
                worst = fmax(worst, fabs(right[1][k] - (3.0 * b[2][k] - b[3][k] - b[0][k])));
            }
            seiler_bernstein(b, 3, 0.37, want);
            seiler_polynomial(b, 0.37, got);
            poly = fmax(poly, apart(want, got));
        }
        demo_check(d, "cubic: Seiler points against 3 b1 - b0 - b3 and 3 b2 - b3 - b0", worst, "< 1e-14", worst < 1e-14);
        demo_check(d, "  polynomial form - Bernstein", poly, "< 1e-14", poly < 1e-14);
    }

    /* 3. uniform Catmull-Rom: the Seiler point at a knot is the same for the piece before and
     * the piece after, so 2 m + 2 points store m pieces. */
    {
        SeilerPoint knot[12], seiler[12];
        double      shared = 0.0, worst = 0.0;
        for (i = 0; i < 12; ++i)
            for (k = 0; k < 2; ++k) knot[i][k] = 2.0 * random01(&state) - 1.0;
        seiler_spline_points(knot, 12, seiler);
        for (i = 1; i + 2 < 12; ++i) {
            /* the piece from knot i to i + 1 as a Bezier curve, and the one before it */
            SeilerPoint b[4], before[4], diff[4], left[2], right[2], left0[2], right0[2], want, got;
            int         sample;
            for (k = 0; k < 2; ++k) {
                b[0][k] = knot[i][k];     b[1][k] = knot[i][k] + (knot[i + 1][k] - knot[i - 1][k]) / 6.0;
                b[3][k] = knot[i + 1][k]; b[2][k] = knot[i + 1][k] - (knot[i + 2][k] - knot[i][k]) / 6.0;
                before[3][k] = knot[i][k];     before[2][k] = knot[i][k] - (knot[i + 1][k] - knot[i - 1][k]) / 6.0;
                before[0][k] = knot[i - 1][k]; before[1][k] = knot[i - 1][k] + (knot[i][k] - knot[i > 1 ? i - 2 : 0][k]) / 6.0;
            }
            seiler_differences(b, 3, diff);      seiler_points(diff, 3, left, right);
            seiler_differences(before, 3, diff); seiler_points(diff, 3, left0, right0);
            shared = fmax(shared, apart(left[1], right0[1]));
            for (sample = 0; sample <= 10; ++sample) {
                seiler_bernstein(b, 3, (double)sample / 10.0, want);
                seiler_spline(knot, seiler, 12, (double)i + (double)sample / 10.0, got);
                worst = fmax(worst, apart(want, got));
            }
        }
        demo_check(d, "Catmull-Rom, uniform: first Seiler point of a piece - second of the piece before", shared, "< 1e-14", shared < 1e-14);
        demo_check(d, "  spline from one knot and one Seiler point a sample - its Bezier pieces", worst, "< 1e-14", worst < 1e-14);
    }

    /* 4. a cubic split at 1/2 by de Casteljau: the two halves join smoothly, yet their Seiler
     * points at the join differ, as the talk says. */
    {
        const SeilerPoint *b = k_curve[3];
        SeilerPoint        half[2][4], diff[4], left[2], right[2], other_left[2], other_right[2];
        for (k = 0; k < 2; ++k) {
            const double m01 = 0.5 * (b[0][k] + b[1][k]), m12 = 0.5 * (b[1][k] + b[2][k]), m23 = 0.5 * (b[2][k] + b[3][k]);
            const double m02 = 0.5 * (m01 + m12), m13 = 0.5 * (m12 + m23), mid = 0.5 * (m02 + m13);
            half[0][0][k] = b[0][k]; half[0][1][k] = m01; half[0][2][k] = m02; half[0][3][k] = mid;
            half[1][0][k] = mid;     half[1][1][k] = m13; half[1][2][k] = m23; half[1][3][k] = b[3][k];
        }
        seiler_differences(half[0], 3, diff); seiler_points(diff, 3, left, right);
        seiler_differences(half[1], 3, diff); seiler_points(diff, 3, other_left, other_right);
        demo_check(d, "cubic split in two: Seiler points on the two sides of the split are this far apart (not 0)",
                   apart(right[1], other_left[1]), "> 0.01", apart(right[1], other_left[1]) > 0.01);
    }

    /* 5. time for a cubic, each form, 4 million evaluations; reported. */
    {
        static const char *const k_names[5] = {"ns an evaluation (reported): Bernstein", "  de Casteljau, 6 lerps", "  polynomial",
                                               "  Seiler points, 3 lerps", "  Seiler offsets"};
        const SeilerPoint *b = k_curve[3];
        SeilerPoint        diff[4], left[2], right[2], got;
        int                form;
        seiler_differences(b, 3, diff);
        seiler_points(diff, 3, left, right);
        for (form = 0; form < 5; ++form) {
            const clock_t start = clock();
            double        sum = 0.0;
            for (i = 0; i < 4000000; ++i) {
                const double t = (double)(i & 1023) / 1023.0;
                if (form == 0) seiler_bernstein(b, 3, t, got);
                if (form == 1) seiler_casteljau(b, 3, t, got);
                if (form == 2) seiler_polynomial(b, t, got);
                if (form == 3) seiler_from_points(left, right, 3, t, got);
                if (form == 4) seiler_from_offsets(diff, 3, t, got);
                sum += got[0];
            }
            demo_check(d, k_names[form], (double)(clock() - start) / (double)CLOCKS_PER_SEC / 4e6 * 1e9, "", sum == sum);
        }
    }
}

int main(int argc, char **argv) {
    static App app;
    static const DemoOption options[] = {
        {"--scene", .u32 = &app.scene, .lo = 0, .hi = 1, .help = "0 one curve and its construction, 1 a Catmull-Rom spline from knots and Seiler points"},
        {"--degree", .u32 = &app.degree, .lo = 2, .hi = SEILER_MAX, .help = "degree of the curve of scene 0"},
        {"--at", .f32 = &app.at, .lo = 0, .hi = 1, .help = "show the construction at this parameter; 0 lets it move"},
        {0}};
    Demo *d = &app.demo;
    app.degree = 3u;
    demo_defaults(d, "seiler", "Seiler's interpolation");
    demo_open(d, options, argc, argv);
    canvas_open(d, &app.canvas, 60000u, CANVAS_RGB(26, 28, 36));
    while (demo_running(d)) {
        if (d->ui) app_ui(&app, d->ui);
        app_frame(&app);
    }
    if (d->check) app_check(&app);
    return demo_close(d);
}
