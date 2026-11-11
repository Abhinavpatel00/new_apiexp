/* polyroots — a picture made of polynomial roots: thick curves by the closest-point test.
 *
 *   Draw -> Present          (the method runs in C: method.c)
 *
 * Cem Yuksel: "High-Performance Polynomial Root Finding for Graphics" (HPG 2022). Three thick
 * cubic curves seen along z. For every cell of the picture and every curve, the point of the
 * curve nearest to the cell's ray is a root of a polynomial of degree 5 in the curve's
 * parameter, and the cell is covered when that point is within the radius (the "closest point
 * approach" of the paper's section 6.4). The box at the lower right is that polynomial for
 * the white probe and the first curve: yellow where a monotonic piece changes sign and is
 * searched, grey where it is skipped, ticks at the derivative's zeros, dots at the roots.
 * Lines go from the probe to the curve at each root; the nearest is yellow.
 *
 * --check repeats the paper's test on random Bernstein polynomials of degree 3 .. 10 and 20
 * (how many have 0, 1, 2 or more roots in 0 .. 1 against table 1; every root against the
 * Bernstein form in long double), measures the error for two thresholds, how often plain
 * Newton iterations miss a root, known roots, and the nearest point against a search.
 */
#include "canvas.h"
#include "method.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PI     3.14159265358979323846
#define CURVES 3
#define STORED 10000

typedef struct App {
    Demo     demo;   /* first: see demo.h */
    Canvas   canvas;
    uint32_t cells;   /* rows of the picture */
    float    epsilon, radius, clock;
    bool     still, no_graph;
} App;

/* The polynomial (c(t) - p) . c'(t) of a planar cubic c with power coefficients a[k] (x, y
 * and, unused here, z), its roots in 0 .. 1, and the parameter of the nearest point of the
 * curve to p. Returns the squared distance. */
static double nearest(const double a[4][3], const double p[2], double epsilon, PrWork *work, double g[6], double roots[5], int *count, double *at) {
    double best = 1e30;
    int    k, j, i;
    *at = 0.0;
    memset(g, 0, 6 * sizeof(double));
    for (k = 0; k < 4; ++k)
        for (j = 0; j < 3; ++j) g[k + j] += (j + 1) * ((a[k][0] - (k == 0) * p[0]) * a[j + 1][0] + (a[k][1] - (k == 0) * p[1]) * a[j + 1][1]);
    *count = pr_roots(g, 5, 0.0, 1.0, epsilon, roots, work);
    for (i = 0; i < *count + 2; ++i) {
        const double t = i < *count ? roots[i] : i - *count;   /* the roots, then the two ends */
        const double x = ((a[3][0] * t + a[2][0]) * t + a[1][0]) * t + a[0][0] - p[0], y = ((a[3][1] * t + a[2][1]) * t + a[1][1]) * t + a[0][1] - p[1];
        if (x * x + y * y < best) { best = x * x + y * y; *at = t; }
    }
    return best;
}

static void app_ui(App *a, struct nk_context *ctx) {
    float cells = (float)a->cells;
    bool  graph = !a->no_graph;
    if (nk_begin(ctx, "Polynomial roots", nk_rect(16, 16, 300, 210), NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_TITLE)) {
        demo_ui_float(ctx, "Error threshold epsilon", &a->epsilon, 0.0f, 0.2f);
        demo_ui_float(ctx, "Radius of the curves", &a->radius, 0.02f, 0.2f);
        demo_ui_float(ctx, "Rows of the picture", &cells, 36.0f, 108.0f);
        demo_ui_bool(ctx, "Polynomial of the probe", &graph);
        demo_ui_bool(ctx, "Still", &a->still);
    }
    nk_end(ctx);
    a->cells = (uint32_t)cells; a->no_graph = !graph;
}

static void app_frame(App *a) {
    static const double  k_control[CURVES][4][3] = {{{-1.3, -0.5, 0.2}, {-0.2, 1.4, -0.6}, {0.1, -1.5, 0.7}, {1.2, 0.5, -0.2}},
                                                    {{-1.2, 0.6, -0.4}, {-0.5, -0.9, 0.9}, {0.7, 0.9, 0.5}, {1.3, -0.6, -0.5}},
                                                    {{-0.4, -0.8, -0.8}, {1.6, -0.2, 0.3}, {-1.6, 0.1, 0.4}, {0.4, 0.8, -0.7}}};
    static const uint8_t k_colour[CURVES][3] = {{235, 150, 80}, {100, 170, 240}, {130, 210, 140}};
    Demo                *d = &a->demo;
    Canvas              *c = &a->canvas;
    const double         time = a->clock, turn = 0.3 * time, half = (double)d->width / d->height, size = 2.0 / a->cells, r = a->radius;
    const double         probe[2] = {0.9 * sin(0.5 * time), 0.5 * sin(0.8 * time + 1.0)};
    const int            columns = (int)(2.0 * half / size) + 1;
    double               power[CURVES][4][3], box[CURVES][4], g[6], roots[5], at;
    PrWork               work = {0};
    int                  i, j, k, m, count;

    a->clock += a->still ? 0.0f : 1.0f / 60.0f;
    for (m = 0; m < CURVES; ++m) {
        double q[4][3];
        box[m][0] = box[m][1] = 1e30; box[m][2] = box[m][3] = -1e30;
        for (i = 0; i < 4; ++i) {
            /* turned about the y axis */
            q[i][0] = cos(turn) * k_control[m][i][0] + sin(turn) * k_control[m][i][2]; q[i][1] = k_control[m][i][1];
            q[i][2] = -sin(turn) * k_control[m][i][0] + cos(turn) * k_control[m][i][2];
            box[m][0] = fmin(box[m][0], q[i][0] - r); box[m][1] = fmin(box[m][1], q[i][1] - r); box[m][2] = fmax(box[m][2], q[i][0] + r); box[m][3] = fmax(box[m][3], q[i][1] + r);
        }
        for (k = 0; k < 3; ++k) {
            power[m][0][k] = q[0][k]; power[m][1][k] = 3.0 * (q[1][k] - q[0][k]);
            power[m][2][k] = 3.0 * (q[0][k] - 2.0 * q[1][k] + q[2][k]); power[m][3][k] = -q[0][k] + 3.0 * q[1][k] - 3.0 * q[2][k] + q[3][k];
        }
    }

    canvas_view_flat(d, c, 0.0f, 0.0f, 1.0f);
    for (j = 0; j < (int)a->cells; ++j)
        for (i = 0; i < columns; ++i) {
            const double p[2] = {-half + (i + 0.5) * size, -1.0 + (j + 0.5) * size};
            double       front = -1e30, shade = 0.0;
            int          which = -1;
            for (m = 0; m < CURVES; ++m) {
                double d2, t = 0.0, z, nx, ny, nz;
                if (p[0] < box[m][0] || p[0] > box[m][2] || p[1] < box[m][1] || p[1] > box[m][3]) continue;
                d2 = nearest(power[m], p, a->epsilon, &work, g, roots, &count, &t);
                if (d2 >= r * r) continue;
                /* the surface is taken as a ball of the curve's radius round the nearest point */
                nx = (p[0] - (((power[m][3][0] * t + power[m][2][0]) * t + power[m][1][0]) * t + power[m][0][0])) / r;
                ny = (p[1] - (((power[m][3][1] * t + power[m][2][1]) * t + power[m][1][1]) * t + power[m][0][1])) / r;
                nz = sqrt(fmax(1.0 - nx * nx - ny * ny, 0.0));
                z = ((power[m][3][2] * t + power[m][2][2]) * t + power[m][1][2]) * t + power[m][0][2] + r * nz;
                if (z > front) { front = z; which = m; shade = 0.25 + 0.75 * fmax(0.0, 0.45 * nx + 0.55 * ny + 0.7 * nz); }
            }
            if (which >= 0) {
                const float    x0 = (float)(p[0] - 0.5 * size), x1 = (float)(p[0] + 0.5 * size), y0 = (float)(p[1] - 0.5 * size), y1 = (float)(p[1] + 0.5 * size);
                const float    q0[3] = {x0, y0, 0.0f}, q1[3] = {x1, y0, 0.0f}, q2[3] = {x1, y1, 0.0f}, q3[3] = {x0, y1, 0.0f};
                const uint32_t rgba = CANVAS_RGB((uint8_t)(shade * k_colour[which][0]), (uint8_t)(shade * k_colour[which][1]), (uint8_t)(shade * k_colour[which][2]));
                canvas_triangle(c, q0, q1, q2, rgba); canvas_triangle(c, q0, q2, q3, rgba);
            }
        }

    if (!a->no_graph) {
        /* the probe, its polynomial for the first curve, and the points of that curve at the roots */
        const float x0 = (float)half - 1.02f, x1 = (float)half - 0.04f, y0 = -0.96f, y1 = -0.42f, mid = 0.5f * (y0 + y1);
        const float b0[3] = {x0, y0, 0.5f}, b1[3] = {x1, y0, 0.5f}, b2[3] = {x1, y1, 0.5f}, b3[3] = {x0, y1, 0.5f}, z0[3] = {x0, mid, 0.6f}, z1[3] = {x1, mid, 0.6f};
        const float from[3] = {(float)probe[0], (float)probe[1], 0.8f};
        double      slope[5], edge[6], largest = 1e-30;
        int         pieces;
        nearest(power[0], probe, 0.0, &work, g, roots, &count, &at);
        for (k = 1; k <= 5; ++k) slope[k - 1] = k * g[k];
        pieces = 1 + pr_roots(slope, 4, 0.0, 1.0, 0.0, edge + 1, &work);
        edge[0] = 0.0; edge[pieces] = 1.0;
        for (i = 0; i <= 96; ++i) largest = fmax(largest, fabs(((((g[5] * i / 96.0 + g[4]) * i / 96.0 + g[3]) * i / 96.0 + g[2]) * i / 96.0 + g[1]) * i / 96.0 + g[0]));
        canvas_triangle(c, b0, b1, b2, CANVAS_RGB(14, 16, 22)); canvas_triangle(c, b0, b2, b3, CANVAS_RGB(14, 16, 22));
        canvas_line(c, z0, z1, 0.004f, CANVAS_RGB(90, 96, 112));
        for (k = 0; k < pieces; ++k) {
            const double lo = edge[k], hi = edge[k + 1];
            const double flo = ((((g[5] * lo + g[4]) * lo + g[3]) * lo + g[2]) * lo + g[1]) * lo + g[0], fhi = ((((g[5] * hi + g[4]) * hi + g[3]) * hi + g[2]) * hi + g[1]) * hi + g[0];
            const float  tick0[3] = {x0 + (x1 - x0) * (float)hi, y0 + 0.02f, 0.6f}, tick1[3] = {tick0[0], y1 - 0.02f, 0.6f};
            if (k + 1 < pieces) canvas_line(c, tick0, tick1, 0.003f, CANVAS_RGB(70, 76, 92));
            for (i = 0; i < 24; ++i) {
                const double t0 = lo + (hi - lo) * i / 24.0, t1 = lo + (hi - lo) * (i + 1) / 24.0;
                const double v0 = ((((g[5] * t0 + g[4]) * t0 + g[3]) * t0 + g[2]) * t0 + g[1]) * t0 + g[0], v1 = ((((g[5] * t1 + g[4]) * t1 + g[3]) * t1 + g[2]) * t1 + g[1]) * t1 + g[0];
                const float  s0[3] = {x0 + (x1 - x0) * (float)t0, mid + 0.45f * (y1 - y0) * (float)(v0 / largest), 0.7f}, s1[3] = {x0 + (x1 - x0) * (float)t1, mid + 0.45f * (y1 - y0) * (float)(v1 / largest), 0.7f};
                canvas_line(c, s0, s1, 0.008f, (flo < 0.0) != (fhi < 0.0) ? CANVAS_RGB(240, 200, 90) : CANVAS_RGB(120, 124, 136));
            }
        }
        for (k = 0; k < count; ++k) {
            const double t = roots[k];
            const float  dot[3] = {x0 + (x1 - x0) * (float)t, mid, 0.8f};
            const float  to[3] = {(float)(((power[0][3][0] * t + power[0][2][0]) * t + power[0][1][0]) * t + power[0][0][0]), (float)(((power[0][3][1] * t + power[0][2][1]) * t + power[0][1][1]) * t + power[0][0][1]), 0.3f};
            canvas_dot(c, dot, 0.014f, CANVAS_RGB(240, 240, 240));
            canvas_line(c, from, to, 0.005f, CANVAS_RGB(200, 200, 210));
        }
        {
            const float to[3] = {(float)(((power[0][3][0] * at + power[0][2][0]) * at + power[0][1][0]) * at + power[0][0][0]), (float)(((power[0][3][1] * at + power[0][2][1]) * at + power[0][1][1]) * at + power[0][0][1]), 0.4f};
            canvas_line(c, from, to, 0.009f, CANVAS_RGB(240, 200, 90));
        }
        canvas_dot(c, from, 0.02f, CANVAS_RGB(255, 255, 255));
    }
    canvas_frame(d, c);
}

static double uniform(uint64_t *state) {
    *state = *state * 6364136223846793005ull + 1442695040888963407ull;
    return (double)(*state >> 11) / 9007199254740992.0;
}

/* A polynomial in Bernstein form by de Casteljau's steps, in long double. */
static long double bernstein(const double *b, int degree, long double x) {
    long double v[PR_MOST + 1] = {0};
    int         i, k;
    for (i = 0; i <= degree; ++i) v[i] = b[i];
    for (k = degree; k > 0; --k)
        for (i = 0; i < k; ++i) v[i] = (1.0L - x) * v[i] + x * v[i + 1];
    return v[0];
}

/* Bernstein coefficients evenly in -1 .. 1 (the paper's test polynomials) and their power form. */
static void random_polynomial(uint64_t *state, int degree, double *b, double *c) {
    double binom[PR_MOST + 1][PR_MOST + 1];
    int    i, k;
    for (i = 0; i <= degree; ++i)
        for (k = 0; k <= i; ++k) binom[i][k] = k == 0 || k == i ? 1.0 : binom[i - 1][k - 1] + binom[i - 1][k];
    for (i = 0; i <= degree; ++i) b[i] = 2.0 * uniform(state) - 1.0;
    for (k = 0; k <= degree; ++k) {
        c[k] = 0.0;
        for (i = 0; i <= k; ++i) c[k] += ((k - i) % 2 ? -1.0 : 1.0) * binom[degree][k] * binom[k][i] * b[i];
    }
}

static void app_check(App *a) {
    static const int    k_degree[9] = {3, 4, 5, 6, 7, 8, 9, 10, 20};
    static const double k_table[9][3] = {{27, 48, 24}, {22, 45, 33}, {18, 42, 41}, {15, 38, 47}, {12, 35, 53}, {10, 32, 58}, {9, 29, 62}, {8, 27, 66}, {2, 12, 86}};   /* the paper's table 1, per cent */
    static double       stored_b[STORED][PR_MOST + 1], stored_c[STORED][PR_MOST + 1];
    Demo               *d = &a->demo;
    uint64_t            state = 2022u;
    double              roots[PR_MOST], exact[PR_MOST];
    int                 which, n, i, j, k;
    char                label[200];

    /* 1: random polynomials in Bernstein form, roots in 0 .. 1 to full precision */
    for (which = 0; which < 9; ++which) {
        const int degree = k_degree[which], many = degree == 20 ? 3000 : STORED;
        PrWork    work = {0};
        double    share[3] = {0}, off = 0.0, worst = 0.0, sum = 0.0, seconds;
        long      missed = 0, unconfirmed = 0, total = 0;
        clock_t   start;
        for (n = 0; n < many; ++n) random_polynomial(&state, degree, stored_b[n], stored_c[n]);
        start = clock();
        for (n = 0; n < many; ++n) pr_roots(stored_c[n], degree, 0.0, 1.0, 0.0, roots, &work);
        seconds = (double)(clock() - start) / CLOCKS_PER_SEC;
        for (n = 0; n < many; ++n) {
            PrWork            unused = {0};
            const int         count = pr_roots(stored_c[n], degree, 0.0, 1.0, 0.0, roots, &unused);
            long double       before = bernstein(stored_b[n], degree, 0.0L);
            share[count > 2 ? 2 : count] += 100.0 / many;
            /* every sign change between 513 samples of the Bernstein form holds an odd number of the roots */
            for (j = 0; j < 512; ++j) {
                const long double after = bernstein(stored_b[n], degree, (j + 1) / 512.0L);
                int               inside = 0;
                for (i = 0; i < count; ++i) inside += roots[i] >= j / 512.0 && roots[i] <= (j + 1) / 512.0;
                missed += ((before < 0.0L) != (after < 0.0L)) && inside % 2 == 0;
                before = after;
            }
            /* every root is a sign change of the Bernstein form within 1e-7; where exactly, by bisection */
            for (i = 0; i < count; ++i) {
                long double lo = fmaxl(roots[i] - 1e-7L, 0.0L), hi = fminl(roots[i] + 1e-7L, 1.0L);
                const bool  down = bernstein(stored_b[n], degree, lo) < 0.0L;
                total += 1;
                if (down == (bernstein(stored_b[n], degree, hi) < 0.0L)) { unconfirmed += 1; continue; }
                for (k = 0; k < 70; ++k) {
                    const long double middle = 0.5L * (lo + hi);
                    if ((bernstein(stored_b[n], degree, middle) < 0.0L) == down) lo = middle; else hi = middle;
                }
                worst = fmax(worst, fabs(roots[i] - (double)lo)); sum += fabs(roots[i] - (double)lo);
            }
        }
        for (k = 0; k < 3; ++k) off = fmax(off, fabs(share[k] - k_table[which][k]));
        snprintf(label, sizeof label, "degree %d, %d random Bernstein polynomials, roots in 0 .. 1: per cent with no root", degree, many);
        demo_check(d, label, share[0], "table 1 (info)", true);
        demo_check(d, "  with one root", share[1], "table 1 (info)", true);
        demo_check(d, "  with two or more", share[2], "table 1 (info)", true);
        demo_check(d, "  largest difference from the paper's table 1, points", off, "< 1.5", off < 1.5);
        demo_check(d, "  sign changes between 513 samples of the Bernstein form without a root reported", missed, "= 0", missed == 0);
        if (degree <= 10) {
            demo_check(d, "  roots reported that are not a sign change of the Bernstein form within 1e-7", unconfirmed, "= 0", unconfirmed == 0);
            demo_check(d, "  largest distance of a root from the Bernstein form's (long double, bisection)", worst, "< 1e-9", worst < 1e-9);
        } else {
            demo_check(d, "  roots reported that are not a sign change of the Bernstein form within 1e-7 (info)", unconfirmed, "any", true);
            demo_check(d, "  largest distance of the others from the Bernstein form's (info)", worst, "any", true);
        }
        demo_check(d, "  mean distance (info)", sum / fmax((double)(total - unconfirmed), 1.0), "any", true);
        demo_check(d, "  pieces searched, per cent of all monotonic pieces (info)", 100.0 * work.searched / work.pieces, "any", true);
        demo_check(d, "  iterations a search (info)", (double)work.newton / work.searched, "any", true);
        demo_check(d, "  of them halvings of the interval, per cent (info)", 100.0 * work.bisection / work.newton, "any", true);
        demo_check(d, "  microseconds a polynomial on this machine (info)", 1e6 * seconds / many, "any", true);
    }

    /* 2: cubics with an error threshold; 3: plain Newton iterations on the same cubics */
    state = 2022u;
    for (n = 0; n < STORED; ++n) random_polynomial(&state, 3, stored_b[n], stored_c[n]);
    for (which = 0; which < 2; ++which) {
        const double epsilon = which ? 3.5e-4 : 1e-2;
        PrWork       full = {0}, work = {0};
        double       worst = 0.0, sum = 0.0;
        long         total = 0, differ = 0;
        for (n = 0; n < STORED; ++n) {
            const int count = pr_roots(stored_c[n], 3, 0.0, 1.0, 0.0, exact, &full), loose = pr_roots(stored_c[n], 3, 0.0, 1.0, epsilon, roots, &work);
            differ += count != loose;
            for (i = 0; i < (count == loose ? count : 0); ++i) { worst = fmax(worst, fabs(roots[i] - exact[i])); sum += fabs(roots[i] - exact[i]); total += 1; }
        }
        snprintf(label, sizeof label, "%d cubics, epsilon %g: largest error of a root", STORED, epsilon);
        demo_check(d, label, worst, "<= epsilon", worst <= epsilon);
        demo_check(d, "  mean error (info; the paper, in single precision: 5e-5 at 1e-2, 5e-8 at 3.5e-4)", sum / total, "any", true);
        demo_check(d, "  cubics with another number of roots than at full precision (info)", differ, "any", true);
        demo_check(d, "  iterations a search", (double)work.newton / work.searched, "< at full precision", (double)work.newton / work.searched < (double)full.newton / full.searched);
    }
    {
        /* from x = 0, at most 40 steps, each kept inside 0 .. 1; found: within 1e-3 of a root when the step falls under 3.5e-4 */
        long with[4] = {0}, failed[4] = {0};
        for (n = 0; n < STORED; ++n) {
            PrWork       unused = {0};
            const int    count = pr_roots(stored_c[n], 3, 0.0, 1.0, 0.0, exact, &unused);
            const double *c = stored_c[n];
            double       x = 0.0;
            bool         found = false;
            for (k = 0; k < 40 && !found; ++k) {
                const double f = ((c[3] * x + c[2]) * x + c[1]) * x + c[0], df = (3.0 * c[3] * x + 2.0 * c[2]) * x + c[1], next = fmin(fmax(x - f / df, 0.0), 1.0);
                for (i = 0; i < count; ++i) found |= fabs(next - x) <= 3.5e-4 && fabs(next - exact[i]) < 1e-3;
                x = next;
            }
            with[count] += 1; failed[count] += !found;
        }
        demo_check(d, "plain Newton iterations from 0, kept in 0 .. 1, 40 steps: per cent of the cubics with one root where it is not found (the paper: 33)", 100.0 * failed[1] / with[1], "within 5 of 33", fabs(100.0 * failed[1] / with[1] - 33.0) < 5.0);
        demo_check(d, "  of those with two roots where neither is found (the paper: 11)", 100.0 * failed[2] / with[2], "within 5 of 11", fabs(100.0 * failed[2] / with[2] - 11.0) < 5.0);
        demo_check(d, "  of all cubics, those without a root included (the paper: 18)", 100.0 * (failed[1] + failed[2] + failed[3]) / STORED, "within 5 of 18", fabs(100.0 * (failed[1] + failed[2] + failed[3]) / STORED - 18.0) < 5.0);
    }

    /* 4: roots that are known */
    {
        /* the Chebyshev polynomial T_10 on -1 .. 1, and (x - 0.1)(x - 0.2) .. (x - 0.9) on 0 .. 1 */
        static const double k_chebyshev[11] = {-1, 0, 50, 0, -400, 0, 1120, 0, -1280, 0, 512};
        double              product[10] = {1.0}, worst = 0.0;
        PrWork              work = {0};
        int                 count = pr_roots(k_chebyshev, 10, -1.0, 1.0, 0.0, roots, &work);
        for (i = 0; i < count; ++i) worst = fmax(worst, fabs(roots[i] - cos(PI * (2.0 * (10 - i) - 1.0) / 20.0)));
        demo_check(d, "Chebyshev polynomial of degree 10 on -1 .. 1: roots found", count, "= 10", count == 10);
        demo_check(d, "  largest distance from cos((2k - 1) pi / 20)", worst, "< 1e-13", worst < 1e-13);
        for (k = 1; k <= 9; ++k)
            for (i = k; i >= 0; --i) product[i] = (i > 0 ? product[i - 1] : 0.0) - 0.1 * k * (i < k ? product[i] : 0.0);
        count = pr_roots(product, 9, 0.0, 1.0, 0.0, roots, &work);
        for (worst = 0.0, i = 0; i < count; ++i) worst = fmax(worst, fabs(roots[i] - 0.1 * (i + 1)));
        demo_check(d, "(x - 0.1)(x - 0.2) .. (x - 0.9) on 0 .. 1: roots found", count, "= 9", count == 9);
        demo_check(d, "  largest distance from 0.1, 0.2, ..", worst, "< 1e-9", worst < 1e-9);
        {
            /* (x - 0.5)^2 (x + 1): the curve touches zero inside the interval */
            static const double k_touch[4] = {0.25, -0.75, 0.0, 1.0};
            count = pr_roots(k_touch, 3, 0.0, 1.0, 0.0, roots, &work);
            demo_check(d, "(x - 0.5)^2 (x + 1) on 0 .. 1, a root where the curve only touches zero: roots found (info; the paper: can be missed)", count, "any", true);
        }
    }

    /* 5: the picture's use: the nearest point of a cubic to a point, against 4097 samples refined by halving the step */
    {
        double worst = 0.0;
        PrWork work = {0};
        for (n = 0; n < 2000; ++n) {
            double power[4][3], p[2], g[6], at, best = 1e30, where = 0.0, step = 1.0 / 4096.0, found;
            int    count;
            for (k = 0; k < 4; ++k) { power[k][0] = (k ? 3.0 : 1.0) * (2.0 * uniform(&state) - 1.0); power[k][1] = (k ? 3.0 : 1.0) * (2.0 * uniform(&state) - 1.0); power[k][2] = 0.0; }
            p[0] = 2.0 * uniform(&state) - 1.0; p[1] = 2.0 * uniform(&state) - 1.0;
            found = nearest(power, p, 0.0, &work, g, roots, &count, &at);
            for (j = 0; j <= 4096; ++j) {
                const double t = j / 4096.0, x = ((power[3][0] * t + power[2][0]) * t + power[1][0]) * t + power[0][0] - p[0], y = ((power[3][1] * t + power[2][1]) * t + power[1][1]) * t + power[0][1] - p[1];
                if (x * x + y * y < best) { best = x * x + y * y; where = t; }
            }
            for (k = 0; k < 40; ++k, step *= 0.5)
                for (j = -1; j <= 1; j += 2) {
                    const double t = fmin(fmax(where + j * step, 0.0), 1.0), x = ((power[3][0] * t + power[2][0]) * t + power[1][0]) * t + power[0][0] - p[0], y = ((power[3][1] * t + power[2][1]) * t + power[1][1]) * t + power[0][1] - p[1];
                    if (x * x + y * y < best) { best = x * x + y * y; where = t; }
                }
            worst = fmax(worst, sqrt(found) - sqrt(best));
        }
        demo_check(d, "2000 random cubics and points: distance by the roots of the degree 5 polynomial minus the distance by search, largest", worst, "< 1e-9", worst < 1e-9);
    }
}

int main(int argc, char **argv) {
    static App app;
    static const DemoOption options[] = {
        {"--epsilon", .f32 = &app.epsilon, .lo = 0, .hi = 0.2f, .help = "error threshold of the root finder for the picture (0: full precision)"},
        {"--radius", .f32 = &app.radius, .lo = 0.02f, .hi = 0.2f, .help = "radius of the curves"},
        {"--cells", .u32 = &app.cells, .lo = 36, .hi = 108, .help = "rows of the picture"},
        {"--no-graph", .flag = &app.no_graph, .help = "without the probe and its polynomial"},
        {"--still", .flag = &app.still, .help = "the curves do not turn"},
        {0}};
    Demo *d = &app.demo;
    app.cells = 108u; app.radius = 0.07f;
    demo_defaults(d, "polyroots", "Polynomial roots");
    demo_open(d, options, argc, argv);
    canvas_open(d, &app.canvas, 160000u, CANVAS_RGB(24, 27, 36));
    if (d->headless && !d->frames) d->frames = 70u;
    while (demo_running(d)) {
        if (d->ui) app_ui(&app, d->ui);
        app_frame(&app);
    }
    if (d->check) app_check(&app);
    return demo_close(d);
}
