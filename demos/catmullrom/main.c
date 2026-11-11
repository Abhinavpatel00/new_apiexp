/* catmullrom — why the centripetal Catmull-Rom curve is the one without loops.
 *
 *   Draw -> Present          (the method runs in C: method.c)
 *
 * Yuksel, Schaefer, Keyser: "On the Parameterization of Catmull-Rom Curves" (2009). One
 * closed control polygon and its cubic Catmull-Rom curve for three parameterizations, in the
 * colours of the paper's figure 7: green uniform (alpha 0), blue centripetal (alpha 1/2),
 * red chordal (alpha 1); grey the values between in steps of 0.1. Scene 0 is a shape with
 * long and short edges, scene 1 a star whose tips are two points that close up. --bounds
 * draws the box of eq. 6 that holds each centripetal piece.
 *
 * --check compares the pyramid with eq. 2, then measures over random control points what the
 * paper proves: theorem 1 and 3, cusps and loops, the distance bound of eq. 6, the overshoot
 * past the end points, and the angle of section 5.
 */
#include "canvas.h"
#include "method.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PI      3.14159265358979323846
#define MOST    16   /* control points */
#define SAMPLES 32   /* lines drawn for one piece */

typedef struct App {
    Demo     demo;   /* first: see demo.h */
    Canvas   canvas;
    uint32_t scene;
    float    alpha, clock;   /* alpha < 0: the three named ones */
    bool     no_fan, bounds, still;
} App;

static void app_ui(App *a, struct nk_context *ctx) {
    static const char *const k_scenes[2] = {"Long and short edges", "Star with paired tips"};
    bool                     one = a->alpha >= 0.0f, fan = !a->no_fan;
    if (nk_begin(ctx, "Catmull-Rom parameterization", nk_rect(16, 16, 300, 230), NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_TITLE)) {
        demo_ui_choice(ctx, "Control points", k_scenes, 2u, &a->scene);
        demo_ui_bool(ctx, "One alpha only", &one);
        a->alpha = one ? fmaxf(a->alpha, 0.0f) : -1.0f;
        if (one) demo_ui_float(ctx, "alpha", &a->alpha, 0.0f, 1.0f);
        demo_ui_bool(ctx, "Values between, grey", &fan);
        demo_ui_bool(ctx, "Boxes of eq. 6 (centripetal)", &a->bounds);
        demo_ui_bool(ctx, "Still", &a->still);
    }
    nk_end(ctx);
    a->no_fan = !fan;
}

static void app_frame(App *a) {
    static const double k_free[9][2] = {{-1.2, -0.25}, {-1.05, 0.55}, {-0.25, 0.3}, {-0.1, 0.42}, {0.9, 0.6}, {1.05, 0.45}, {1.2, -0.5}, {0.1, -0.3}, {-0.05, -0.62}};
    Demo               *d = &a->demo;
    Canvas             *c = &a->canvas;
    const uint32_t      count = a->scene == 0u ? 9u : 12u;
    const double        time = a->clock, gap = 0.015 + 0.3 * (0.5 - 0.5 * cos(1.2 * time));
    double              p[MOST][2];
    uint32_t            i, j, k, pass;

    a->clock += a->still ? 0.0f : 1.0f / 60.0f;
    for (i = 0; i < count; ++i) {
        const double tip = 0.5 * PI * (i / 3u), side = i % 3u == 0u ? -gap : gap;
        if (a->scene == 0u) {
            p[i][0] = k_free[i][0] + 0.06 * cos((0.7 + 0.13 * i) * time + 1.7 * i); p[i][1] = k_free[i][1] + 0.06 * sin((0.6 + 0.11 * i) * time + 2.3 * i);
        } else {
            /* four tips, each two points 2 gap apart across the tip's direction, and a point between tips */
            p[i][0] = i % 3u == 2u ? 0.3 * cos(tip + 0.25 * PI) : 0.8 * cos(tip) - side * sin(tip);
            p[i][1] = i % 3u == 2u ? 0.3 * sin(tip + 0.25 * PI) : 0.8 * sin(tip) + side * cos(tip);
        }
    }

    canvas_view_flat(d, c, 0.0f, 0.0f, 1.0f);
    for (i = 0; i < count; ++i) {
        double q[4][2], t[4];
        float  here[3] = {(float)p[i][0], (float)p[i][1], 0.5f}, next[3] = {(float)p[(i + 1u) % count][0], (float)p[(i + 1u) % count][1], 0.0f};
        for (k = 0; k < 4u; ++k) { q[k][0] = p[(i + count - 1u + k) % count][0]; q[k][1] = p[(i + count - 1u + k) % count][1]; }
        canvas_line(c, here, next, 0.004f, CANVAS_RGB(84, 90, 108));
        /* passes 0 .. 10: alpha 0, 0.1, .. 1, the three named ones in colour and in front; pass 11: the one of --alpha */
        for (pass = 0; pass < 12u; ++pass) {
            const bool   named = pass == 0u || pass == 5u || pass == 10u, shown = a->alpha >= 0.0f ? pass == 11u : pass < 11u && (named || !a->no_fan);
            const double alpha = pass == 11u ? a->alpha : 0.1 * pass;
            const float  depth = pass == 5u || pass == 11u ? 0.4f : named ? 0.3f : 0.1f;
            cr_knots(q, alpha, t);
            for (j = 0; j < (shown ? SAMPLES : 0u); ++j) {
                double from[2], to[2];
                float  f0[3], f1[3];
                cr_point(q, t, t[1] + (t[2] - t[1]) * j / SAMPLES, from);
                cr_point(q, t, t[1] + (t[2] - t[1]) * (j + 1u) / SAMPLES, to);
                f0[0] = (float)from[0]; f0[1] = (float)from[1]; f0[2] = depth; f1[0] = (float)to[0]; f1[1] = (float)to[1]; f1[2] = depth;
                canvas_line(c, f0, f1, named || pass == 11u ? 0.012f : 0.004f,
                            pass == 0u ? CANVAS_RGB(110, 200, 120) : pass == 10u ? CANVAS_RGB(230, 100, 90) : pass == 5u || pass == 11u ? CANVAS_RGB(100, 160, 245) : CANVAS_RGB(120, 124, 136));
            }
        }
        if (a->bounds) {
            /* eq. 6 with alpha 1/2, for both ends: the piece is within `reach` of its edge, between its two points */
            const double d1 = hypot(q[1][0] - q[0][0], q[1][1] - q[0][1]), d2 = hypot(q[2][0] - q[1][0], q[2][1] - q[1][1]), d3 = hypot(q[3][0] - q[2][0], q[3][1] - q[2][1]);
            const double reach = d2 * fmax(sqrt(d1 / d2) / (4.0 * (1.0 + sqrt(d1 / d2))), sqrt(d3 / d2) / (4.0 * (1.0 + sqrt(d3 / d2))));
            const double nx = -(q[2][1] - q[1][1]) / d2 * reach, ny = (q[2][0] - q[1][0]) / d2 * reach;
            const float  corner[4][3] = {{(float)(q[1][0] + nx), (float)(q[1][1] + ny), 0.05f}, {(float)(q[2][0] + nx), (float)(q[2][1] + ny), 0.05f}, {(float)(q[2][0] - nx), (float)(q[2][1] - ny), 0.05f}, {(float)(q[1][0] - nx), (float)(q[1][1] - ny), 0.05f}};
            for (k = 0; k < 4u; ++k) canvas_line(c, corner[k], corner[(k + 1u) % 4u], 0.004f, CANVAS_RGB(200, 170, 80));
        }
        canvas_dot(c, here, 0.02f, CANVAS_RGB(240, 240, 240));
    }
    canvas_frame(d, c);
}

static double uniform(uint64_t *state) {
    *state = *state * 6364136223846793005ull + 1442695040888963407ull;
    return (double)(*state >> 11) / 9007199254740992.0;
}

/* Four control points: the middle edge from (0, 0) to (1, 0), the outer edges of lengths
 * between e^-4 and e^4 (evenly in the logarithm) in any direction. */
static void quadruple(uint64_t *state, double p[4][2]) {
    const double d1 = exp(8.0 * uniform(state) - 4.0), d3 = exp(8.0 * uniform(state) - 4.0), a = 2.0 * PI * uniform(state), b = 2.0 * PI * uniform(state);
    p[1][0] = 0.0; p[1][1] = 0.0; p[2][0] = 1.0; p[2][1] = 0.0;
    p[0][0] = d1 * cos(a); p[0][1] = d1 * sin(a); p[3][0] = 1.0 + d3 * cos(b); p[3][1] = d3 * sin(b);
}

static void app_check(App *a) {
    static const double k_alpha[6] = {0.0, 0.25, 0.5, 2.0 / 3.0, 0.75, 1.0};
    Demo               *d = &a->demo;
    uint64_t            state = 2009u;
    double              p[4][2], t[4], b[4][2], x[2];
    int                 n, i, j, k, which;
    char                label[200];

    /* 1: the pyramid, eq. 2, and the tangent where two pieces meet */
    {
        double ends = 0.0, same = 0.0, tangent = 0.0;
        for (n = 0; n < 3000; ++n) {
            const double alpha = k_alpha[n % 6];
            double       five[5][2], left[4][2], right[4][2], tl[4], tr[4];
            for (i = 0; i < 5; ++i) { five[i][0] = uniform(&state); five[i][1] = uniform(&state); }
            cr_knots(five, alpha, t); cr_bezier(five, alpha, left); cr_bezier(five + 1, alpha, right);
            cr_knots(five, alpha, tl); cr_knots(five + 1, alpha, tr);
            cr_point(five, t, t[1], x); ends = fmax(ends, hypot(x[0] - five[1][0], x[1] - five[1][1]));
            cr_point(five, t, t[2], x); ends = fmax(ends, hypot(x[0] - five[2][0], x[1] - five[2][1]));
            for (j = 0; j <= 16; ++j) {
                const double s = j / 16.0, w[4] = {(1 - s) * (1 - s) * (1 - s), 3 * s * (1 - s) * (1 - s), 3 * s * s * (1 - s), s * s * s};
                cr_point(five, t, t[1] + s * (t[2] - t[1]), x);
                same = fmax(same, hypot(x[0] - (w[0] * left[0][0] + w[1] * left[1][0] + w[2] * left[2][0] + w[3] * left[3][0]), x[1] - (w[0] * left[0][1] + w[1] * left[1][1] + w[2] * left[2][1] + w[3] * left[3][1])));
            }
            /* derivative in t at the shared point: 3 (B3 - B2) / interval before, 3 (B1 - B0) / interval after */
            for (k = 0; k < 2; ++k) x[k] = (left[3][k] - left[2][k]) / (tl[2] - tl[1]) - (right[1][k] - right[0][k]) / (tr[2] - tr[1]);
            tangent = fmax(tangent, hypot(x[0], x[1]) / hypot((left[3][0] - left[2][0]) / (tl[2] - tl[1]), (left[3][1] - left[2][1]) / (tl[2] - tl[1])));
        }
        demo_check(d, "3000 random pieces, six values of alpha: largest distance of the pyramid's ends from P1 and P2", ends, "< 1e-12", ends < 1e-12);
        demo_check(d, "  the pyramid (figure 3) against the Bezier form (eq. 2), 17 parameters", same, "< 1e-12", same < 1e-12);
        demo_check(d, "  jump of the derivative where two pieces meet, relative", tangent, "< 1e-9", tangent < 1e-9);
    }

    /* 2: what the paper proves, over 20000 random quadruples for each alpha */
    for (which = 0; which < 6; ++which) {
        const double alpha = k_alpha[which];
        double       lowest = 1e30, highest = -1e30, algebra3 = 0.0, algebra4 = 0.0, algebra5 = 0.0, over6 = 0.0, farthest = 0.0, past = 0.0, widest = 0.0, slowest = 1e30;
        int          backward = 0, loops = 0, outside = 0;
        state = 2009u;
        for (n = 0; n < 20000; ++n) {
            double q[65][2], d1, d3, r, cosine, sine, x1, h1, reach, low = 0.0, high = 1.0;
            int    crossed = 0;
            quadruple(&state, p);
            cr_bezier(p, alpha, b);
            d1 = hypot(p[0][0], p[0][1]); d3 = hypot(p[3][0] - 1.0, p[3][1]); r = d1;   /* d2 = 1 */
            cosine = p[0][0] / d1; sine = p[0][1] / d1;                                  /* theta: between P0 - P1 and P2 - P1 */
            x1 = b[1][0]; h1 = fabs(b[1][1]);                                            /* B1 along the edge and off it */
            lowest = fmin(lowest, x1); highest = fmax(highest, x1);
            algebra3 = fmax(algebra3, fabs(x1 - (pow(d1, 2.0 * alpha) - pow(d1, 1.0) * cosine) / (3.0 * pow(d1, alpha) * (pow(d1, alpha) + 1.0))));
            algebra4 = fmax(algebra4, fabs(x1 - (pow(r, alpha) - pow(r, 1.0 - alpha) * cosine) / (3.0 * (1.0 + pow(r, alpha)))));
            algebra5 = fmax(algebra5, fabs(h1 - pow(r, 1.0 - alpha) * fabs(sine) / (3.0 * (1.0 + pow(r, alpha)))));
            widest = fmax(widest, atan2(h1, 1.0 - x1));
            reach = fmax(pow(d1, 1.0 - alpha) / (4.0 * (1.0 + pow(d1, alpha))), pow(d3, 1.0 - alpha) / (4.0 * (1.0 + pow(d3, alpha))));
            for (j = 0; j <= 64; ++j) {
                const double s = j / 64.0, w[4] = {(1 - s) * (1 - s) * (1 - s), 3 * s * (1 - s) * (1 - s), 3 * s * s * (1 - s), s * s * s};
                const double speed = 3.0 * ((1 - s) * (1 - s) * (b[1][0] - b[0][0]) + 2.0 * s * (1 - s) * (b[2][0] - b[1][0]) + s * s * (b[3][0] - b[2][0]));
                q[j][0] = w[0] * b[0][0] + w[1] * b[1][0] + w[2] * b[2][0] + w[3] * b[3][0]; q[j][1] = w[0] * b[0][1] + w[1] * b[1][1] + w[2] * b[2][1] + w[3] * b[3][1];
                over6 = fmax(over6, fabs(q[j][1]) / reach);
                farthest = fmax(farthest, fabs(q[j][1]));
                low = fmin(low, q[j][0]); high = fmax(high, q[j][0]);
                past = fmax(past, q[j][0] < 0.0 ? hypot(q[j][0], q[j][1]) : q[j][0] > 1.0 ? hypot(q[j][0] - 1.0, q[j][1]) : 0.0);
                slowest = fmin(slowest, speed);
            }
            for (j = 0; j < 64; ++j)
                for (k = j + 2; k < 64; ++k) {
                    const double ux = q[j + 1][0] - q[j][0], uy = q[j + 1][1] - q[j][1], vx = q[k + 1][0] - q[k][0], vy = q[k + 1][1] - q[k][1];
                    const double s0 = ux * (q[k][1] - q[j][1]) - uy * (q[k][0] - q[j][0]), s1 = ux * (q[k + 1][1] - q[j][1]) - uy * (q[k + 1][0] - q[j][0]);
                    const double s2 = vx * (q[j][1] - q[k][1]) - vy * (q[j][0] - q[k][0]), s3 = vx * (q[j + 1][1] - q[k][1]) - vy * (q[j + 1][0] - q[k][0]);
                    crossed |= s0 * s1 < 0.0 && s2 * s3 < 0.0;
                }
            backward += b[1][0] < 0.0 || b[2][0] > 1.0;
            loops += crossed;
            outside += low < -1e-12 || high > 1.0 + 1e-12;
        }
        snprintf(label, sizeof label, "alpha %.3f, 20000 random quadruples, edge lengths e^-4 .. e^4 of the middle one: (B1 - B0).(B3 - B0) against eq. 3", alpha);
        demo_check(d, label, algebra3, "< 1e-9", algebra3 < 1e-9);
        demo_check(d, "  the same against eq. 4", algebra4, "< 1e-9", algebra4 < 1e-9);
        demo_check(d, "  B1's distance from the edge's line against eq. 5", algebra5, "< 1e-9", algebra5 < 1e-9);
        if (alpha == 0.5) {
            demo_check(d, "  pieces that start or end going backward along their edge (theorem 1)", backward, "= 0", backward == 0);
            demo_check(d, "  smallest and", lowest, ">= 0", lowest >= 0.0);
            demo_check(d, "  largest position of B1 along the edge (theorem 3)", highest, "< 1", highest < 1.0);
            demo_check(d, "  smallest derivative along the edge, 65 parameters a piece (theorem 2)", slowest, ">= 0", slowest >= 0.0);
            demo_check(d, "  pieces that cross themselves", loops, "= 0", loops == 0);
            demo_check(d, "  pieces that leave the strip between their two points", outside, "= 0", outside == 0);
        } else {
            demo_check(d, "  pieces that start or end going backward along their edge (theorem 1)", backward, "> 0", backward > 0);
            demo_check(d, "  smallest derivative along the edge", slowest, "< 0", slowest < 0.0);
            demo_check(d, "  pieces that cross themselves", loops, "> 0", loops > 0);
            demo_check(d, "  pieces that leave the strip between their two points", outside, "> 0", outside > 0);
        }
        demo_check(d, "  largest distance from the edge's line over the bound of eq. 6", over6, "<= 1", over6 <= 1.0 + 1e-12);
        if (alpha == 0.0 || alpha == 0.25) demo_check(d, "  largest distance from the edge's line, in edge lengths", farthest, "> 1/4 (no bound below alpha 1/2)", farthest > 0.25);
        if (alpha == 0.5 || alpha == 0.75 || alpha == 1.0) demo_check(d, "  largest distance from the edge's line, in edge lengths", farthest, "<= 1/4", farthest <= 0.25);
        if (which == 3) demo_check(d, "  largest distance from the edge's line, in edge lengths", farthest, "<= 1/8", farthest <= 0.125);
        if (alpha == 1.0) demo_check(d, "  farthest point past an end of the edge, from that end, in edge lengths", past, "<= 1/3 (eq. 8)", past <= 1.0 / 3.0);
        if (alpha == 0.5) demo_check(d, "  widest angle at B3 between B1 and B0 (gamma of section 5)", widest, "<= pi / 6 = 0.5236 (section 5)", widest <= PI / 6.0 + 1e-12);
    }

    /* 3: section 5's formula for the angle and its largest value */
    {
        double worst = 0.0, printed = 0.0, largest = 0.0;
        state = 2009u;
        for (n = 0; n < 20000; ++n) {
            double r, cosine, sine;
            quadruple(&state, p);
            cr_bezier(p, 0.5, b);
            r = hypot(p[0][0], p[0][1]); cosine = p[0][0] / r; sine = p[0][1] / r;
            /* tan(gamma) = (eq. 5) / (1 - eq. 4), which has + cos(theta); the paper's text prints - */
            worst = fmax(worst, fabs(sqrt(r) * fabs(sine) / (3.0 + 2.0 * sqrt(r) + sqrt(r) * cosine) - fabs(b[1][1]) / (1.0 - b[1][0])));
            printed = fmax(printed, fabs(sqrt(r) * fabs(sine) / (3.0 + 2.0 * sqrt(r) - sqrt(r) * cosine) - fabs(b[1][1]) / (1.0 - b[1][0])));
        }
        for (i = 0; i <= 400; ++i)
            for (j = 0; j <= 720; ++j) {
                const double root = exp(0.05 * i - 5.0), theta = PI * j / 720.0;
                largest = fmax(largest, atan(root * sin(theta) / (3.0 + 2.0 * root + root * cos(theta))));
            }
        demo_check(d, "centripetal: sqrt(r) |sin| / (3 + 2 sqrt(r) + sqrt(r) cos) against the tangent of the angle at B3 between B1 and B0", worst, "< 1e-9", worst < 1e-9);
        demo_check(d, "  the same with - sqrt(r) cos, as printed in section 5 (info)", printed, "any", true);
        demo_check(d, "  largest gamma over sqrt(r) in e^-5 .. e^15 and all theta", largest, "<= pi / 6, > 0.52", largest <= PI / 6.0 && largest > 0.52);
    }
}

int main(int argc, char **argv) {
    static App app;
    static const DemoOption options[] = {
        {"--scene", .u32 = &app.scene, .lo = 0, .hi = 1, .help = "0 long and short edges, 1 a star whose tips are pairs of points"},
        {"--alpha", .f32 = &app.alpha, .lo = 0, .hi = 1, .help = "draw this one parameterization only (0 uniform, 0.5 centripetal, 1 chordal)"},
        {"--no-fan", .flag = &app.no_fan, .help = "without the grey curves of alpha 0.1, 0.2, .."},
        {"--bounds", .flag = &app.bounds, .help = "the boxes of eq. 6 for the centripetal curve"},
        {"--still", .flag = &app.still, .help = "the control points do not move"},
        {0}};
    Demo *d = &app.demo;
    app.alpha = -1.0f;
    demo_defaults(d, "catmullrom", "Catmull-Rom parameterization");
    demo_open(d, options, argc, argv);
    canvas_open(d, &app.canvas, 120000u, CANVAS_RGB(24, 27, 36));
    if (d->headless && !d->frames) d->frames = 60u;
    while (demo_running(d)) {
        if (d->ui) app_ui(&app, d->ui);
        app_frame(&app);
    }
    if (d->check) app_check(&app);
    return demo_close(d);
}
