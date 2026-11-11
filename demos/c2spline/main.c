/* c2spline — curves through control points, C2 everywhere, each piece from four points.
 *
 *   Draw -> Present          (the method runs in C: method.c)
 *
 * Cem Yuksel: "A Class of C2 Interpolating Splines" (ACM Transactions on Graphics 39(5),
 * 2020). The same closed set of control points with the paper's four interpolation
 * functions: top left Bézier, top right circular, bottom left elliptical, bottom right
 * hybrid. Red is where a circle is used, blue an ellipse, green a parabola. The purple
 * lines are the curvature. Scene 0 is a free shape whose points wander, scene 1 has its
 * points on a circle (one slides, one leaves), scene 2 is a star whose tips are pairs of
 * points that close up (section 5.5).
 *
 * --check measures eq. 1, the continuity of the first and second derivative and of the
 * curvature at the control points (and that a linear blend does not have it), the distance
 * and the between-the-points bounds of sections 4.1 and 4.3, self-intersections, circles,
 * the hybrid's switch, and that a control point moves four segments.
 */
#include "canvas.h"
#include "method.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PI      3.14159265358979323846
#define MOST    16   /* control points */
#define SAMPLES 40   /* lines drawn for one segment */

typedef struct App {
    Demo     demo;   /* first: see demo.h */
    Canvas   canvas;
    uint32_t type, scene;   /* type 4: all four */
    float    comb, clock;
    bool     still;
} App;

static void app_ui(App *a, struct nk_context *ctx) {
    static const char *const k_types[5] = {"Bezier", "Circular", "Elliptical", "Hybrid", "All four"};
    static const char *const k_scenes[3] = {"Free shape", "Points on a circle", "Star with paired tips"};
    if (nk_begin(ctx, "C2 interpolating splines", nk_rect(16, 16, 300, 190), NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_TITLE)) {
        demo_ui_choice(ctx, "Interpolation function", k_types, 5u, &a->type);
        demo_ui_choice(ctx, "Control points", k_scenes, 3u, &a->scene);
        demo_ui_float(ctx, "Curvature lines", &a->comb, 0.0f, 0.1f);
        demo_ui_bool(ctx, "Still", &a->still);
    }
    nk_end(ctx);
}

static void app_frame(App *a) {
    static const double  k_free[9][2] = {{-0.8, -0.3}, {-0.55, 0.45}, {-0.1, 0.1}, {0.3, 0.6}, {0.8, 0.4}, {0.55, -0.1}, {0.75, -0.55}, {0.0, -0.35}, {-0.4, -0.65}};
    static const double  k_angle[7] = {0.0, 0.9, 1.7, 2.9, 3.6, 4.6, 5.5};
    static const uint8_t k_colour[3][3] = {{120, 210, 150}, {235, 110, 90}, {100, 160, 245}};
    Demo                *d = &a->demo;
    Canvas              *c = &a->canvas;
    const uint32_t       count = a->scene == 0u ? 9u : a->scene == 1u ? 7u : 12u, first = a->type == 4u ? 0u : a->type, last = a->type == 4u ? 3u : a->type;
    const double         time = a->clock, gap = 0.012 + 0.3 * (0.5 - 0.5 * cos(1.2 * time));
    double               p[MOST][2];
    C2Fn                 fn[MOST];
    uint32_t             i, j, k, type;

    a->clock += a->still ? 0.0f : 1.0f / 60.0f;
    for (i = 0; i < count; ++i) {
        const double tip = 0.5 * PI * (i / 3u), side = i % 3u == 0u ? -gap : gap;
        const double radius = 0.6 * (1.0 + (i == 5u) * 0.45 * fmax(0.0, sin(0.8 * time))), angle = k_angle[i % 7u] + (i == 2u) * 0.5 * sin(1.3 * time);
        switch (a->scene) {
        case 0u: p[i][0] = k_free[i][0] + 0.09 * cos((0.7 + 0.13 * i) * time + 1.7 * i); p[i][1] = k_free[i][1] + 0.09 * sin((0.6 + 0.11 * i) * time + 2.3 * i); break;
        case 1u: p[i][0] = radius * cos(angle); p[i][1] = radius * sin(angle); break;
        /* four tips, each two points 2 gap apart across the tip's direction, and a point between tips */
        default:
            p[i][0] = i % 3u == 2u ? 0.3 * cos(tip + 0.25 * PI) : 0.78 * cos(tip) - side * sin(tip);
            p[i][1] = i % 3u == 2u ? 0.3 * sin(tip + 0.25 * PI) : 0.78 * sin(tip) + side * cos(tip);
            break;
        }
    }

    canvas_view_flat(d, c, 0.0f, 0.0f, 1.0f);
    for (type = first; type <= last; ++type) {
        const float scale = a->type == 4u ? 0.47f : 1.0f, cx = a->type == 4u ? (type % 2u ? 0.89f : -0.89f) : 0.0f, cy = a->type == 4u ? (type < 2u ? 0.5f : -0.5f) : 0.0f;
        for (i = 0; i < count; ++i) c2_function(&fn[i], (int)type, p[(i + count - 1u) % count], p[i], p[(i + 1u) % count]);
        for (i = 0; i < count; ++i) {
            const C2Fn *f = &fn[i], *g = &fn[(i + 1u) % count];
            const float here[3] = {cx + scale * (float)p[i][0], cy + scale * (float)p[i][1], 0.3f}, next[3] = {cx + scale * (float)p[(i + 1u) % count][0], cy + scale * (float)p[(i + 1u) % count][1], 0.0f};
            canvas_line(c, here, next, 0.004f, CANVAS_RGB(70, 76, 92));
            for (j = 0; j < SAMPLES; ++j) {
                const double theta = 0.5 * PI * j / SAMPLES, mid = theta + 0.25 * PI / SAMPLES, h = 1e-3, w = sin(mid) * sin(mid);
                double       from[2], to[2], before[2], at[2], after[2], dx, dy, ex, ey, speed, curvature;
                float        q0[3], q1[3], foot[3], tip[3];
                uint8_t      rgb[3];
                c2_segment(f, g, theta, from);
                c2_segment(f, g, theta + 0.5 * PI / SAMPLES, to);
                c2_segment(f, g, mid - h, before); c2_segment(f, g, mid, at); c2_segment(f, g, mid + h, after);
                dx = (after[0] - before[0]) / (2.0 * h); dy = (after[1] - before[1]) / (2.0 * h);
                ex = (after[0] - 2.0 * at[0] + before[0]) / (h * h); ey = (after[1] - 2.0 * at[1] + before[1]) / (h * h);
                speed = fmax(hypot(dx, dy), 1e-9);
                curvature = (dx * ey - dy * ex) / (speed * speed * speed);
                for (k = 0; k < 3u; ++k) rgb[k] = (uint8_t)((1.0 - w) * k_colour[f->kind][k] + w * k_colour[g->kind][k]);
                q0[0] = cx + scale * (float)from[0]; q0[1] = cy + scale * (float)from[1]; q0[2] = 0.2f;
                q1[0] = cx + scale * (float)to[0]; q1[1] = cy + scale * (float)to[1]; q1[2] = 0.2f;
                canvas_line(c, q0, q1, 0.012f * (a->type == 4u ? 0.8f : 1.0f), CANVAS_RGB(rgb[0], rgb[1], rgb[2]));
                /* a line across the curve, away from the centre of curvature, as long as the curvature (capped) */
                foot[0] = cx + scale * (float)at[0]; foot[1] = cy + scale * (float)at[1]; foot[2] = 0.1f;
                tip[0] = foot[0] + scale * (float)(fmin(a->comb * fabs(curvature), 0.25) * (curvature > 0.0 ? 1.0 : -1.0) * dy / speed);
                tip[1] = foot[1] - scale * (float)(fmin(a->comb * fabs(curvature), 0.25) * (curvature > 0.0 ? 1.0 : -1.0) * dx / speed);
                tip[2] = 0.1f;
                canvas_line(c, foot, tip, 0.004f, CANVAS_RGB(150, 95, 205));
            }
            canvas_dot(c, here, 0.022f * (a->type == 4u ? 0.8f : 1.0f), CANVAS_RGB(240, 240, 240));
        }
    }
    canvas_frame(d, c);
}

static double uniform(uint64_t *state) {
    *state = *state * 6364136223846793005ull + 1442695040888963407ull;
    return (double)(*state >> 11) / 9007199254740992.0;
}

/* the closed curve's segment i at theta, from all its functions */
static void curve(const C2Fn *fn, int count, int i, double theta, double out[2]) { c2_segment(&fn[i], &fn[(i + 1) % count], theta, out); }

/* the same segment with weights that are straight lines in theta instead of cos^2 and sin^2 */
static void linear(const C2Fn *fn, int count, int i, double theta, double out[2]) {
    const double w = theta / (0.5 * PI);
    double       a[2], b[2];
    c2_at(&fn[i], theta + 0.5 * PI, a); c2_at(&fn[(i + 1) % count], theta, b);
    out[0] = (1.0 - w) * a[0] + w * b[0]; out[1] = (1.0 - w) * a[1] + w * b[1];
}

/* largest | |x| - 1 | over the closed curve through `count` points */
static double off_circle(int type, int count, double p[][2]) {
    C2Fn   fn[MOST];
    double worst = 0.0, x[2];
    int    i, j;
    for (i = 0; i < count; ++i) c2_function(&fn[i], type, p[(i + count - 1) % count], p[i], p[(i + 1) % count]);
    for (i = 0; i < count; ++i)
        for (j = 0; j <= 64; ++j) { curve(fn, count, i, 0.5 * PI * j / 64.0, x); worst = fmax(worst, fabs(hypot(x[0], x[1]) - 1.0)); }
    return worst;
}

static void app_check(App *a) {
    static const char *const k_names[4] = {"Bezier", "circular", "elliptical", "hybrid"};
    static const double      k_irregular[7] = {0.0, 0.9, 1.7, 2.9, 3.6, 4.6, 5.5};
    Demo                    *d = &a->demo;
    uint64_t                 state = 2020u;
    double                   p[MOST][2], x[2], y[2];
    C2Fn                     fn[MOST], other[MOST];
    int                      type, n, i, j, k, blend;
    char                     label[200];

    /* 1: eq. 1 and what section 4 asks of each function */
    for (type = 0; type < 4; ++type) {
        double worst = 0.0, skew = 0.0;
        int    outside = 0, late = 0;
        for (n = 0; n < 5000; ++n) {
            C2Fn f;
            for (i = 0; i < 3; ++i) { p[i][0] = uniform(&state); p[i][1] = uniform(&state); }
            c2_function(&f, type, p[0], p[1], p[2]);
            for (i = 0; i < 3; ++i) { c2_at(&f, 0.5 * PI * i, x); worst = fmax(worst, hypot(x[0] - p[i][0], x[1] - p[i][1])); }
            if (f.kind == C2_PARABOLA) {
                /* the curvature |B' x B''| / |B'|^3 of the parabola is largest at t_i */
                double kappa[3];
                for (i = 0; i < 3; ++i) {
                    const double t = f.t + 1e-4 * (i - 1);
                    const double dx = 2.0 * (1.0 - t) * (f.b[1][0] - f.b[0][0]) + 2.0 * t * (f.b[2][0] - f.b[1][0]), dy = 2.0 * (1.0 - t) * (f.b[1][1] - f.b[0][1]) + 2.0 * t * (f.b[2][1] - f.b[1][1]);
                    const double ex = 2.0 * (f.b[2][0] - 2.0 * f.b[1][0] + f.b[0][0]), ey = 2.0 * (f.b[2][1] - 2.0 * f.b[1][1] + f.b[0][1]);
                    kappa[i] = fabs(dx * ey - dy * ex) / pow(dx * dx + dy * dy, 1.5);
                }
                late += !(kappa[1] >= kappa[0] && kappa[1] >= kappa[2]);
            }
            if (f.kind == C2_ELLIPSE) {
                skew = fmax(skew, fabs(f.b[1][0] * f.b[2][0] + f.b[1][1] * f.b[2][1]) / (hypot(f.b[1][0], f.b[1][1]) * hypot(f.b[2][0], f.b[2][1])));
                /* the farther neighbour a quarter turn from p, the nearer one less than that, on the other side */
                outside += !(fmax(-f.from, f.to) > 0.5 * PI - 1e-12 && fmax(-f.from, f.to) < 0.5 * PI + 1e-12 && fmin(-f.from, f.to) > 0.0);
                outside += (f.to > -f.from) != (hypot(p[2][0] - p[1][0], p[2][1] - p[1][1]) >= hypot(p[0][0] - p[1][0], p[0][1] - p[1][1]));
            }
        }
        snprintf(label, sizeof label, "%s, 5000 random triples in the unit square: largest distance of F(0), F(pi/2), F(pi) from the points (eq. 1)", k_names[type]);
        demo_check(d, label, worst, "< 1e-9", worst < 1e-9);
        if (type == C2_BEZIER) demo_check(d, "  parabolas whose curvature is not largest at the middle point (eq. 7)", late, "= 0", late == 0);
        if (type == C2_ELLIPTICAL) {
            demo_check(d, "  ellipses: largest cosine of the angle between the axes that the two points sit on", skew, "< 1e-9", skew < 1e-9);
            demo_check(d, "  ellipses with the neighbours not placed as section 4.3 asks", outside, "= 0", outside == 0);
        }
    }

    /* 2: continuity at the control points. The points are around a centre, at uneven angles and
     * distances 0.4 .. 1: with points anywhere in a square, near-straight triples give circles
     * so large that differences cannot resolve the second derivative. Derivatives are one-sided
     * differences in theta (five and six values, fourth order), scaled
     * to the global parameter of eq. 8, 9: by 1 / t_i before the point and 1 / (1 - t_i) after. */
    for (blend = 0; blend < 2; ++blend)
        for (type = 0; type < 4; ++type) {
            double first = 0.0, second = 0.0, bend = 0.0;
            for (n = 0; n < 200; ++n) {
                for (i = 0; i < 8; ++i) {
                    const double angle = 2.0 * PI * (i + 0.6 * (uniform(&state) - 0.5)) / 8.0, radius = 0.4 + 0.6 * uniform(&state);
                    p[i][0] = radius * cos(angle); p[i][1] = radius * sin(angle);
                }
                for (i = 0; i < 8; ++i) c2_function(&fn[i], type, p[(i + 7) % 8], p[i], p[(i + 1) % 8]);
                for (i = 0; i < 8; ++i) {
                    const double h = 4e-3, before = fn[i].t, after = 1.0 - fn[i].t;
                    double       v[2][6][2], d1[2][2], d2[2][2], kappa[2], size1, size2;
                    for (j = 0; j < 6; ++j) {
                        (blend ? linear : curve)(fn, 8, (i + 7) % 8, 0.5 * PI - j * h, v[0][j]);
                        (blend ? linear : curve)(fn, 8, i, j * h, v[1][j]);
                    }
                    for (k = 0; k < 2; ++k) {
                        for (j = 0; j < 2; ++j) {
                            d1[j][k] = (j ? -1.0 : 1.0) * (25.0 * v[j][0][k] - 48.0 * v[j][1][k] + 36.0 * v[j][2][k] - 16.0 * v[j][3][k] + 3.0 * v[j][4][k]) / (12.0 * h);
                            d2[j][k] = (45.0 * v[j][0][k] - 154.0 * v[j][1][k] + 214.0 * v[j][2][k] - 156.0 * v[j][3][k] + 61.0 * v[j][4][k] - 10.0 * v[j][5][k]) / (12.0 * h * h);
                        }
                    }
                    for (j = 0; j < 2; ++j) kappa[j] = (d1[j][0] * d2[j][1] - d1[j][1] * d2[j][0]) / pow(d1[j][0] * d1[j][0] + d1[j][1] * d1[j][1], 1.5);
                    size1 = fmax(hypot(d1[0][0], d1[0][1]) / before, hypot(d1[1][0], d1[1][1]) / after);
                    size2 = fmax(size1, fmax(hypot(d2[0][0], d2[0][1]) / (before * before), hypot(d2[1][0], d2[1][1]) / (after * after)));
                    first = fmax(first, hypot(d1[0][0] / before - d1[1][0] / after, d1[0][1] / before - d1[1][1] / after) / size1);
                    second = fmax(second, hypot(d2[0][0] / (before * before) - d2[1][0] / (after * after), d2[0][1] / (before * before) - d2[1][1] / (after * after)) / size2);
                    bend = fmax(bend, fabs(kappa[0] - kappa[1]) / fmax(fmax(fabs(kappa[0]), fabs(kappa[1])), 1.0));
                }
            }
            if (!blend) {
                snprintf(label, sizeof label, "%s, 200 random closed curves of 8 points around a centre: largest jump of the first derivative at a control point, relative", k_names[type]);
                demo_check(d, label, first, "< 1e-4", first < 1e-4);
                demo_check(d, "  of the second derivative, relative to the larger of the two derivatives", second, "< 1e-3", second < 1e-3);
                demo_check(d, "  of the curvature", bend, "< 1e-3", bend < 1e-3);
            } else {
                snprintf(label, sizeof label, "%s with weights linear in theta instead of eq. 2: jump of the first derivative", k_names[type]);
                demo_check(d, label, first, "< 1e-4", first < 1e-4);
                demo_check(d, "  of the second derivative", second, "> 0.5", second > 0.5);
                demo_check(d, "  of the curvature", bend, "> 0.5", bend > 0.5);
            }
        }

    /* 3: how far a segment goes from its chord, whether it stays between its two points, and
     * whether it crosses itself; 1000 random closed curves of 8 points, 64 lines a segment */
    for (type = 0; type < 4; ++type) {
        double away = 0.0;
        int    beyond = 0, crossing = 0, inner = 0;
        for (n = 0; n < 1000; ++n) {
            for (i = 0; i < 8; ++i) { p[i][0] = uniform(&state); p[i][1] = uniform(&state); }
            for (i = 0; i < 8; ++i) c2_function(&fn[i], type, p[(i + 7) % 8], p[i], p[(i + 1) % 8]);
            for (i = 0; i < 8; ++i) {
                const double *from = p[i], *to = p[(i + 1) % 8];
                const double  length = hypot(to[0] - from[0], to[1] - from[1]), h = 1e-3;
                double        q[65][2], low = 0.0, high = 1.0, largest = 0.0;
                int           crossed = 0, where = 0;
                for (j = 0; j <= 64; ++j) {
                    const double along = ((q[j][0] = 0.0, curve(fn, 8, i, 0.5 * PI * j / 64.0, q[j]), q[j][0] - from[0]) * (to[0] - from[0]) + (q[j][1] - from[1]) * (to[1] - from[1])) / (length * length);
                    away = fmax(away, fabs((q[j][0] - from[0]) * (to[1] - from[1]) - (q[j][1] - from[1]) * (to[0] - from[0])) / (length * length));
                    low = fmin(low, along); high = fmax(high, along);
                }
                for (j = 0; j < 64; ++j)
                    for (k = j + 2; k < 64; ++k) {
                        const double ux = q[j + 1][0] - q[j][0], uy = q[j + 1][1] - q[j][1], vx = q[k + 1][0] - q[k][0], vy = q[k + 1][1] - q[k][1];
                        const double s0 = ux * (q[k][1] - q[j][1]) - uy * (q[k][0] - q[j][0]), s1 = ux * (q[k + 1][1] - q[j][1]) - uy * (q[k + 1][0] - q[j][0]);
                        const double s2 = vx * (q[j][1] - q[k][1]) - vy * (q[j][0] - q[k][0]), s3 = vx * (q[j + 1][1] - q[k][1]) - vy * (q[j + 1][0] - q[k][0]);
                        crossed |= s0 * s1 < 0.0 && s2 * s3 < 0.0;
                    }
                /* where along the segment the curvature is largest */
                for (j = 1; j < 64; ++j) {
                    double u[2], v[2], w[2], dx, dy, ex, ey, kappa;
                    curve(fn, 8, i, 0.5 * PI * j / 64.0 - h, u); curve(fn, 8, i, 0.5 * PI * j / 64.0, v); curve(fn, 8, i, 0.5 * PI * j / 64.0 + h, w);
                    dx = (w[0] - u[0]) / (2.0 * h); dy = (w[1] - u[1]) / (2.0 * h); ex = (w[0] - 2.0 * v[0] + u[0]) / (h * h); ey = (w[1] - 2.0 * v[1] + u[1]) / (h * h);
                    kappa = fabs(dx * ey - dy * ex) / pow(dx * dx + dy * dy, 1.5);
                    if (kappa > largest) { largest = kappa; where = j; }
                }
                beyond += low < -1e-9 || high > 1.0 + 1e-9;
                crossing += crossed;
                inner += where >= 16 && where <= 48;
            }
        }
        snprintf(label, sizeof label, "%s, 8000 random segments: largest distance from the chord over its length", k_names[type]);
        if (type == C2_BEZIER) demo_check(d, label, away, "<= 1/8 (theorem A.1)", away <= 0.125 + 1e-9);
        if (type == C2_CIRCULAR) demo_check(d, label, away, "> 1 (no bound)", away > 1.0);
        if (type >= C2_ELLIPTICAL) demo_check(d, label, away, "<= (sqrt 2 - 1) / 2 = 0.2071 (theorem B.1)", away <= 0.5 * (sqrt(2.0) - 1.0) + 1e-9);
        if (type == C2_CIRCULAR) {
            demo_check(d, "  segments that leave the strip between their two points", beyond, "> 0", beyond > 0);
            demo_check(d, "  segments that cross themselves", crossing, "> 0", crossing > 0);
        } else {
            demo_check(d, "  segments that leave the strip between their two points", beyond, "= 0", beyond == 0);
            demo_check(d, "  segments that cross themselves", crossing, "= 0", crossing == 0);
        }
        if (type == C2_BEZIER || type == C2_HYBRID) demo_check(d, "  segments whose largest curvature is in their middle half", inner, "= 0", inner == 0);
        else demo_check(d, "  segments whose largest curvature is in their middle half", inner, "> 0", inner > 0);
    }

    /* 4: control points on the unit circle (figure 8) */
    {
        double regular[2][4];
        for (i = 0; i < 7; ++i) { p[i][0] = cos(k_irregular[i]); p[i][1] = sin(k_irregular[i]); }
        demo_check(d, "7 points at uneven angles on a circle, circular: largest distance of the curve from the circle", off_circle(C2_CIRCULAR, 7, p), "< 1e-12", off_circle(C2_CIRCULAR, 7, p) < 1e-12);
        demo_check(d, "  hybrid (every gap is under a quarter turn)", off_circle(C2_HYBRID, 7, p), "< 1e-12", off_circle(C2_HYBRID, 7, p) < 1e-12);
        demo_check(d, "  elliptical", off_circle(C2_ELLIPTICAL, 7, p), "> 1e-3", off_circle(C2_ELLIPTICAL, 7, p) > 1e-3);
        demo_check(d, "  Bezier", off_circle(C2_BEZIER, 7, p), "> 1e-3", off_circle(C2_BEZIER, 7, p) > 1e-3);
        for (type = 0; type < 4; ++type)
            for (k = 0; k < 2; ++k) {
                for (i = 0; i < 4 + 4 * k; ++i) { p[i][0] = cos(2.0 * PI * i / (4 + 4 * k)); p[i][1] = sin(2.0 * PI * i / (4 + 4 * k)); }
                regular[k][type] = off_circle(type, 4 + 4 * k, p);
            }
        demo_check(d, "4 points evenly on a circle, Bezier", regular[0][C2_BEZIER], "> 1e-3", regular[0][C2_BEZIER] > 1e-3);
        demo_check(d, "  8 points, Bezier: closer to the circle (figure 8b)", regular[1][C2_BEZIER], "< with 4 points", regular[1][C2_BEZIER] < regular[0][C2_BEZIER]);
        demo_check(d, "4 points evenly on a circle, elliptical: a circle (figure 8e)", regular[0][C2_ELLIPTICAL], "< 1e-9", regular[0][C2_ELLIPTICAL] < 1e-9);
        demo_check(d, "  8 points, elliptical: not a circle (figure 8f)", regular[1][C2_ELLIPTICAL], "> 1e-3", regular[1][C2_ELLIPTICAL] > 1e-3);
        demo_check(d, "  8 points, hybrid", regular[1][C2_HYBRID], "< 1e-12", regular[1][C2_HYBRID] < 1e-12);
    }

    /* 5: where the hybrid switches (an arc of a quarter turn) the ellipse is the circle */
    {
        double worst = 0.0, step = 0.0;
        for (n = 0; n < 200; ++n) {
            const double turn = uniform(&state) * 2.0 * PI, small = 0.1 + 1.4 * uniform(&state), radius = 0.2 + uniform(&state);
            const int    flip = n % 2;
            C2Fn         circle, ellipse, under, over;
            double       q[3][2], r[3][2];
            for (i = 0; i < 3; ++i) {
                const double angle = turn + (i == 0 ? -(flip ? small : 0.5 * PI) : i == 2 ? (flip ? 0.5 * PI : small) : 0.0);
                const double nudge = turn + (i == 0 ? -(flip ? small : 0.5 * PI + 1e-7) : i == 2 ? (flip ? 0.5 * PI + 1e-7 : small) : 0.0);
                q[i][0] = radius * cos(angle); q[i][1] = radius * sin(angle);
                r[i][0] = radius * cos(nudge); r[i][1] = radius * sin(nudge);
            }
            c2_function(&circle, C2_CIRCULAR, q[0], q[1], q[2]); c2_function(&ellipse, C2_ELLIPTICAL, q[0], q[1], q[2]);
            c2_function(&under, C2_CIRCULAR, r[0], r[1], r[2]); c2_function(&over, C2_HYBRID, r[0], r[1], r[2]);
            for (j = 0; j <= 32; ++j) {
                c2_at(&circle, PI * j / 32.0, x); c2_at(&ellipse, PI * j / 32.0, y);
                worst = fmax(worst, hypot(x[0] - y[0], x[1] - y[1]) / radius);
                c2_at(&under, PI * j / 32.0, x); c2_at(&over, PI * j / 32.0, y);
                step = fmax(step, (over.kind == C2_ELLIPSE ? 1.0 : 1e9) * hypot(x[0] - y[0], x[1] - y[1]) / radius);
            }
        }
        demo_check(d, "200 triples on a circle with one arc a quarter turn, the other less: ellipse against circle, over the radius", worst, "< 1e-9", worst < 1e-9);
        demo_check(d, "  the arc 1e-7 longer, so that the hybrid takes the ellipse: against the circle", step, "< 1e-5", step < 1e-5);
    }

    /* 6: one control point moved */
    for (type = 0; type < 4; ++type) {
        int changed = 0, wrong = 0;
        for (i = 0; i < 12; ++i) { p[i][0] = cos(2.0 * PI * i / 12.0) * (0.6 + 0.4 * uniform(&state)); p[i][1] = sin(2.0 * PI * i / 12.0) * (0.6 + 0.4 * uniform(&state)); }
        for (i = 0; i < 12; ++i) c2_function(&fn[i], type, p[(i + 11) % 12], p[i], p[(i + 1) % 12]);
        p[5][0] += 0.13; p[5][1] -= 0.08;
        for (i = 0; i < 12; ++i) c2_function(&other[i], type, p[(i + 11) % 12], p[i], p[(i + 1) % 12]);
        for (i = 0; i < 12; ++i) {
            double moved = 0.0;
            for (j = 1; j < 16; ++j) { curve(fn, 12, i, 0.5 * PI * j / 16.0, x); curve(other, 12, i, 0.5 * PI * j / 16.0, y); moved = fmax(moved, hypot(x[0] - y[0], x[1] - y[1])); }
            changed += moved > 0.0;
            wrong += (moved > 0.0) != (i >= 3 && i <= 6);
        }
        snprintf(label, sizeof label, "%s, 12 points, point 5 moved: segments that change", k_names[type]);
        demo_check(d, label, changed, "= 4", changed == 4);
        demo_check(d, "  segments other than 3 .. 6 that change, or of those that do not", wrong, "= 0", wrong == 0);
    }

    /* 7: four points in a line, and the curve turned, scaled and shifted */
    for (type = 0; type < 4; ++type) {
        static const double k_line[7][2] = {{-0.9, -0.3}, {-0.5, -0.1}, {0.1, 0.2}, {0.7, 0.5}, {0.9, -0.6}, {0.0, -0.8}, {-0.8, -0.7}};
        const double        turn = 0.7, size = 1.9, cs = size * cos(turn), sn = size * sin(turn);
        double              bent = 0.0, apart = 0.0;
        for (i = 0; i < 7; ++i) {
            c2_function(&fn[i], type, k_line[(i + 6) % 7], k_line[i], k_line[(i + 1) % 7]);
            p[i][0] = cs * k_line[i][0] - sn * k_line[i][1] + 0.3; p[i][1] = sn * k_line[i][0] + cs * k_line[i][1] - 0.2;
        }
        for (i = 0; i < 7; ++i) c2_function(&other[i], type, p[(i + 6) % 7], p[i], p[(i + 1) % 7]);
        for (j = 0; j <= 32; ++j) {
            curve(fn, 7, 1, 0.5 * PI * j / 32.0, x);
            bent = fmax(bent, fabs((x[0] + 0.5) * 0.3 - (x[1] + 0.1) * 0.6));
            for (i = 0; i < 7; ++i) {
                curve(fn, 7, i, 0.5 * PI * j / 32.0, x); curve(other, 7, i, 0.5 * PI * j / 32.0, y);
                apart = fmax(apart, hypot(cs * x[0] - sn * x[1] + 0.3 - y[0], sn * x[0] + cs * x[1] - 0.2 - y[1]));
            }
        }
        snprintf(label, sizeof label, "%s: four points in a line, the segment between the middle two: largest distance from the line", k_names[type]);
        demo_check(d, label, bent, "< 1e-12", bent < 1e-12);
        demo_check(d, "  control points turned by 0.7, scaled by 1.9 and shifted: the new curve against the old one moved the same way", apart, "< 1e-9", apart < 1e-9);
    }
}

int main(int argc, char **argv) {
    static App app;
    static const DemoOption options[] = {
        {"--type", .u32 = &app.type, .lo = 0, .hi = 4, .help = "0 Bezier, 1 circular, 2 elliptical, 3 hybrid, 4 all four"},
        {"--scene", .u32 = &app.scene, .lo = 0, .hi = 2, .help = "0 free shape, 1 points on a circle, 2 a star whose tips are pairs of points"},
        {"--comb", .f32 = &app.comb, .lo = 0, .hi = 0.1f, .help = "length of the curvature lines for curvature 1"},
        {"--still", .flag = &app.still, .help = "the control points do not move"},
        {0}};
    Demo *d = &app.demo;
    app.type = 4u; app.comb = 0.015f;
    demo_defaults(d, "c2spline", "C2 interpolating splines");
    demo_open(d, options, argc, argv);
    canvas_open(d, &app.canvas, 60000u, CANVAS_RGB(24, 27, 36));
    if (d->headless && !d->frames) d->frames = 90u;
    while (demo_running(d)) {
        if (d->ui) app_ui(&app, d->ui);
        app_frame(&app);
    }
    if (d->check) app_check(&app);
    return demo_close(d);
}
