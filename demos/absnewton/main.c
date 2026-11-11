/* absnewton — absolute eigenvalue filtering for projected Newton.
 *
 *   Draw -> Present          (the method runs in C: method.c)
 *
 * Chen, Liu, Levin, Zheng, Jacobson: "Stabler Neo-Hookean Simulation: Absolute
 * Eigenvalue Filtering for Projected Newton" (SIGGRAPH 2024). Two copies of a
 * nearly incompressible bar whose end has been moved far away look for their
 * state of least energy by Newton's method. The red one clamps each element's
 * negative eigenvalues to zero, the green one takes their absolute values.
 *
 * --check tests the paper's example in two variables (eq. 7 to 11), the
 * eigenvalue routine, eq. 14 to 19, the derivatives, the filters, a small
 * pull against linear elasticity, and counts iterations of both filters.
 */
#include "canvas.h"
#include "method.h"
#include "softmesh.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct App {
    Demo      demo;   /* first: see demo.h */
    Canvas    canvas;
    DemoOrbit orbit;
    SoftMesh  mesh;
    Nh        world[2];        /* 0 clamps, 1 takes absolute values */
    uint32_t  scene, built, frame, iteration[2], pace, cells;
    double    decrement[2];
    float     poisson, tolerance;   /* tolerance: log10 of decrement / lambda at which an iteration stops */
    bool      restart, trace;
} App;

/* A bar of nx x ny x nz cubes with both end faces held and the far one moved to `amount` times
 * the bar's length, and turned a quarter if `turned`. With `everywhere` every node moves in
 * proportion, so each element starts with its volume changed by `amount`; without, everything
 * between stays where it was and the last layer of elements takes all of the change (section 7:
 * "the initial deformation is created by moving some selected vertices"). */
static void bar(Nh *s, SoftMesh *m, uint32_t nx, uint32_t ny, uint32_t nz, double cell, double poisson, double amount, bool turned, bool everywhere) {
    const double young = 1e8, length = cell * nx, cy = 0.5 * cell * ny, cz = 0.5 * cell * nz;
    uint32_t     i;
    free(m->rest); free(m->tet); free(m->face);
    softmesh_cells(m, nx, ny, nz, cell, NULL);
    nh_mesh(s, m->nodes, m->rest, m->tets, m->tet);
    s->mu      = young / (2.0 * (1.0 + poisson));
    s->lambda  = young * poisson / ((1.0 + poisson) * (1.0 - 2.0 * poisson));
    s->epsilon = 0.0;
    for (i = 0u; i < s->nodes; ++i) {
        double    *x = &s->x[3u * i];
        const bool far_end = x[0] > length - 1e-9;
        if (far_end || everywhere) x[0] *= amount;
        if (far_end && turned) { const double y = x[1] - cy, z = x[2] - cz; x[1] = cy - z; x[2] = cz + y; }
        s->held[3u * i] = s->held[3u * i + 1u] = s->held[3u * i + 2u] = far_end || x[0] < 1e-9;
    }
}

static void seed(App *a) {
    static const struct { double amount; bool turned, everywhere; } k_scene[4] = {{3.0, false, true}, {3.0, false, false}, {2.0, true, false}, {0.5, false, true}};
    const uint32_t side = (3u * a->cells + 5u) / 10u;
    uint32_t       w;
    for (w = 0u; w < 2u; ++w) {
        /* the second call builds the same mesh again, which both then share */
        bar(&a->world[w], &a->mesh, a->cells, side, side, 1.0 / a->cells, a->poisson, k_scene[a->scene].amount, k_scene[a->scene].turned, k_scene[a->scene].everywhere);
        a->world[w].filter = w ? NH_ABS : NH_CLAMP;
        a->iteration[w]    = 0u;
        a->decrement[w]    = INFINITY;
    }
    a->built = a->scene;
    a->frame = 0u;
}

static void app_ui(App *a, struct nk_context *ctx) {
    static const char *const k_scenes[4] = {"Stretched to 3 lengths", "End pulled to 3 lengths", "End pulled to 2 and turned", "Squeezed to half"};
    static const char *const k_names[2] = {"Clamped (red)", "Absolute (green)"};
    const float before = a->poisson;
    uint32_t    w;
    if (nk_begin(ctx, "Projected Newton", nk_rect(16, 16, 340, 250), NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_TITLE)) {
        if (demo_ui_choice(ctx, "Scene", k_scenes, 4u, &a->scene)) a->restart = true;
        demo_ui_float(ctx, "Poisson's ratio", &a->poisson, 0.3f, 0.499f);
        for (w = 0u; w < 2u; ++w) {
            demo_ui_text(ctx, "%s: %u iterations%s", k_names[w], a->iteration[w], a->decrement[w] < pow(10.0, a->tolerance) * a->world[w].lambda ? ", done" : "");
            demo_ui_text(ctx, "   energy %.4g J, step %.3g", a->world[w].energy, a->world[w].step);
        }
        nk_layout_row_dynamic(ctx, 24, 1);
        if (nk_button_label(ctx, "Restart")) a->restart = true;
    }
    nk_end(ctx);
    if (before != a->poisson) a->restart = true;
}

static void app_frame(App *a) {
    static const uint32_t k_colour[2] = {CANVAS_RGB(215, 95, 80), CANVAS_RGB(90, 190, 120)};
    Demo    *d = &a->demo;
    Canvas  *c = &a->canvas;
    uint32_t w, i, k;

    if (a->restart || a->built != a->scene) { seed(a); a->restart = false; }
    /* One Newton iteration of each every `pace` frames, until the paper's test says it is done
     * (section 7: decrement below 1e-5 lambda) or 200 have been taken. */
    for (w = 0u; w < 2u && a->frame % a->pace == a->pace - 1u; ++w) {
        if (a->decrement[w] < pow(10.0, a->tolerance) * a->world[w].lambda || a->iteration[w] >= 200u) continue;
        a->decrement[w] = nh_newton(&a->world[w]);
        a->iteration[w]++;
        if (a->trace) printf("%s %3u: energy %.9e, decrement / lambda %.3e, step %.3g, pivots replaced %u\n", w ? "absolute" : "clamped ", a->iteration[w], a->world[w].energy, a->decrement[w] / a->world[w].lambda, a->world[w].step, a->world[w].replaced);
    }
    a->frame++;

    canvas_view_orbit(d, c, &a->orbit);
    for (w = 0u; w < 2u; ++w) {
        Nh *s = &a->world[w];
        /* side by side: the clamped bar behind */
        for (i = 0u; i < s->nodes; ++i) s->trial[3u * i] = s->x[3u * i], s->trial[3u * i + 1u] = s->x[3u * i + 1u], s->trial[3u * i + 2u] = s->x[3u * i + 2u] + (w ? 0.45 : -0.75);
        softmesh_draw(&a->mesh, s->trial, c, k_colour[w], 0.002f);
        for (i = 0u; i < s->nodes; ++i) {
            const float p[3] = {(float)s->trial[3u * i], (float)s->trial[3u * i + 1u], (float)s->trial[3u * i + 2u]};
            if (s->held[3u * i]) canvas_dot(c, p, 0.012f, CANVAS_RGB(240, 210, 70));
        }
        /* progress: one tick an iteration under each bar */
        for (k = 0u; k < a->iteration[w]; ++k) {
            const float at = 0.07f * (float)k, z = w ? 0.6f : -0.6f, tick[2][3] = {{at, -0.3f, z}, {at, -0.18f, z}};
            canvas_line(c, tick[0], tick[1], 0.02f, k_colour[w]);
        }
    }
    canvas_frame(d, c);
}

static double noise(uint32_t *state) {
    *state = *state * 1664525u + 1013904223u;
    return (double)(*state >> 8) / 8388608.0 - 1.0;
}

/* Eq. 7: the sum of two squared distances to unit circles about (-1, 0) and (1, 0). */
static double toy(const double p[2], double g[2], double h[4]) {
    double f = 0.0;
    int    k;
    g[0] = g[1] = h[0] = h[1] = h[2] = h[3] = 0.0;
    for (k = 0; k < 2; ++k) {
        const double x = p[0] + (k ? -1.0 : 1.0), y = p[1], r = sqrt(x * x + y * y), nx = x / r, ny = y / r, bend = 2.0 * (1.0 - 1.0 / r);
        f    += (r - 1.0) * (r - 1.0);
        g[0] += 2.0 * (r - 1.0) * nx;
        g[1] += 2.0 * (r - 1.0) * ny;
        h[0] += 2.0 * nx * nx + bend * (1.0 - nx * nx);
        h[1] += 2.0 * nx * ny - bend * nx * ny;
        h[3] += 2.0 * ny * ny + bend * (1.0 - ny * ny);
    }
    h[2] = h[1];
    return f;
}

static void app_check(App *a) {
    Demo    *d = &a->demo;
    Nh      *s = &a->world[0];
    uint32_t rng = 11u, i, trial;
    int      r, c, k;

    /* 1. Section 4's example at the point (1 - 1e-6, 1e-8): eq. 8, 9 and 11, then figure 8. */
    {
        const double start[2] = {1.0 - 1e-6, 1e-8};
        double       g[2], h[4], copy[4], values[2], vectors[4], dir[2][2], f = toy(start, g, h);
        uint32_t     count[2] = {0u, 0u};
        memcpy(copy, h, sizeof copy);
        nh_eigen(copy, 2, values, vectors);
        demo_check(d, "two-variable example (eq. 7) at (1 - 1e-6, 1e-8): f - 2", f - 2.0, "|.| < 1e-5", fabs(f - 2.0) < 1e-5);
        demo_check(d, "  gradient - (4, -0.02) (eq. 8 prints 3.99, -0.02)", fabs(g[0] - 4.0) + fabs(g[1] + 0.02), "< 1e-3", fabs(g[0] - 4.0) + fabs(g[1] + 0.02) < 1e-3);
        demo_check(d, "  lowest eigenvalue / -2e6 (eq. 8: -1.99e6)", fmin(values[0], values[1]) / -2e6, "0.99 .. 1", fmin(values[0], values[1]) / -2e6 > 0.99 && fmin(values[0], values[1]) / -2e6 <= 1.0);
        demo_check(d, "  other eigenvalue (eq. 8: 3.99)", fmax(values[0], values[1]), "3.99 .. 4", fmax(values[0], values[1]) > 3.99 && fmax(values[0], values[1]) <= 4.0);
        for (k = 0; k < 2; ++k) {
            double m[4];
            memcpy(m, h, sizeof m);
            nh_filter(m, 2, k ? NH_ABS : NH_CLAMP, 1e-3);
            dir[k][0] = -(m[3] * g[0] - m[1] * g[1]) / (m[0] * m[3] - m[1] * m[2]);
            dir[k][1] = -(m[0] * g[1] - m[2] * g[0]) / (m[0] * m[3] - m[1] * m[2]);
        }
        demo_check(d, "  clamped at 1e-3: direction x (eq. 9: -1.19)", dir[0][0], "-1.21 .. -1.18", dir[0][0] > -1.21 && dir[0][0] < -1.18);
        demo_check(d, "  clamped at 1e-3: direction y (eq. 9: -19.99)", dir[0][1], "-20.1 .. -19.9", dir[0][1] > -20.1 && dir[0][1] < -19.9);
        demo_check(d, "  absolute: direction x (eq. 11: -0.99)", dir[1][0], "-1.01 .. -0.98", dir[1][0] > -1.01 && dir[1][0] < -0.98);
        demo_check(d, "  absolute: direction y (eq. 11: 0.01)", dir[1][1], "0 .. 0.02", dir[1][1] > 0.0 && dir[1][1] < 0.02);
        /* Figure 8: iterations until the gradient is below 1e-8, with the same line search as nh_newton. */
        for (k = 0; k < 2; ++k) {
            double p[2] = {start[0], start[1]};
            for (count[k] = 0u; count[k] < 100u; ++count[k]) {
                double m[4], step[2], slope, size, trial_p[2], unused_g[2], unused_h[4];
                f = toy(p, g, m);
                if (sqrt(g[0] * g[0] + g[1] * g[1]) < 1e-8) break;
                nh_filter(m, 2, k ? NH_ABS : NH_CLAMP, 1e-3);
                step[0] = -(m[3] * g[0] - m[1] * g[1]) / (m[0] * m[3] - m[1] * m[2]);
                step[1] = -(m[0] * g[1] - m[2] * g[0]) / (m[0] * m[3] - m[1] * m[2]);
                slope   = step[0] * g[0] + step[1] * g[1];
                for (size = 1.0; size > 1e-12; size *= 0.5) {
                    trial_p[0] = p[0] + size * step[0]; trial_p[1] = p[1] + size * step[1];
                    if (toy(trial_p, unused_g, unused_h) <= f + 1e-4 * size * slope) break;
                }
                p[0] = trial_p[0]; p[1] = trial_p[1];
            }
            demo_check(d, k ? "  absolute: iterations to a gradient below 1e-8 (figure 8: 3)" : "  clamped at 1e-3: iterations to a gradient below 1e-8 (figure 8: 11)",
                       count[k], k ? "fewer than clamped" : "< 100", k ? count[1] < count[0] : count[0] < 100u);
            demo_check(d, "    f where it stopped (0 at the origin, where the circles touch)", f, "< 1e-10", f < 1e-10);
        }
    }

    /* 2. The eigenvalue routine on 200 random symmetric 12 x 12 matrices; then the filters. */
    {
        double rebuilt = 0.0, orthogonal = 0.0, absolute = 0.0, clamped = 0.0;
        for (trial = 0u; trial < 200u; ++trial) {
            double m[144], copy[144], values[12], vectors[144], filtered[144], again[12], unused[144];
            for (r = 0; r < 12; ++r)
                for (c = r; c < 12; ++c) m[r * 12 + c] = m[c * 12 + r] = noise(&rng);
            memcpy(copy, m, sizeof m);
            nh_eigen(copy, 12, values, vectors);
            for (r = 0; r < 12; ++r)
                for (c = 0; c < 12; ++c) {
                    double back = 0.0, vv = 0.0;
                    for (k = 0; k < 12; ++k) { back += vectors[r * 12 + k] * values[k] * vectors[c * 12 + k]; vv += vectors[k * 12 + r] * vectors[k * 12 + c]; }
                    rebuilt    = fmax(rebuilt, fabs(back - m[r * 12 + c]));
                    orthogonal = fmax(orthogonal, fabs(vv - (r == c)));
                }
            /* the filtered matrix has the same eigenvectors, so v^T H+ v gives its eigenvalue for each v */
            for (i = 0u; i < 2u; ++i) {
                memcpy(filtered, m, sizeof m);
                nh_filter(filtered, 12, i ? NH_ABS : NH_CLAMP, 0.0);
                memcpy(copy, filtered, sizeof m);
                nh_eigen(copy, 12, again, unused);
                for (k = 0; k < 12; ++k) {
                    const double want = i ? fabs(values[k]) : fmax(values[k], 0.0);
                    double       nearest = INFINITY;
                    for (c = 0; c < 12; ++c) nearest = fmin(nearest, fabs(again[c] - want));
                    if (i) absolute = fmax(absolute, nearest); else clamped = fmax(clamped, nearest);
                }
            }
        }
        demo_check(d, "eigenvalues of 200 random symmetric 12 x 12 matrices: largest |V L V^T - A|", rebuilt, "< 1e-12", rebuilt < 1e-12);
        demo_check(d, "  largest |V^T V - I|", orthogonal, "< 1e-12", orthogonal < 1e-12);
        demo_check(d, "  after the absolute filter: eigenvalues - |eigenvalues before|", absolute, "< 1e-12", absolute < 1e-12);
        demo_check(d, "  after clamping at 0: eigenvalues - max(eigenvalues before, 0)", clamped, "< 1e-12", clamped < 1e-12);
    }

    /* 3. Eq. 14 to 19: mu +- sigma_k (lambda (J - 1) - mu) are six of the nine eigenvalues of the Hessian in F. */
    {
        const double mu = 1.0, lambda = 99.0;   /* Poisson's ratio 0.495 */
        double       worst = 0.0, lowest = 0.0;
        for (trial = 0u; trial < 500u; ++trial) {
            double f[3][3], h[9][9], ftf[9], sigma[3], unused[9], values[9], vectors[81], j;
            for (r = 0; r < 3; ++r)
                for (c = 0; c < 3; ++c) f[r][c] = (r == c) + 0.8 * noise(&rng);
            j = f[0][0] * (f[1][1] * f[2][2] - f[1][2] * f[2][1]) - f[0][1] * (f[1][0] * f[2][2] - f[1][2] * f[2][0]) + f[0][2] * (f[1][0] * f[2][1] - f[1][1] * f[2][0]);
            for (r = 0; r < 3; ++r)
                for (c = 0; c < 3; ++c) ftf[r * 3 + c] = f[0][r] * f[0][c] + f[1][r] * f[1][c] + f[2][r] * f[2][c];
            nh_eigen(ftf, 3, sigma, unused);
            nh_hessian_f(mu, lambda, f, h);
            nh_eigen(&h[0][0], 9, values, vectors);
            for (k = 0; k < 6; ++k) {
                const double want = mu + (k < 3 ? 1.0 : -1.0) * sqrt(fmax(sigma[k % 3], 0.0)) * (lambda * (j - 1.0) - mu);
                double       nearest = INFINITY;
                for (c = 0; c < 9; ++c) nearest = fmin(nearest, fabs(values[c] - want));
                worst = fmax(worst, nearest);
            }
            for (c = 0; c < 9; ++c) lowest = fmin(lowest, values[c]);
        }
        demo_check(d, "eq. 14 to 19 on 500 random F, mu 1, lambda 99: distance to the nearest eigenvalue of the Hessian in F", worst, "< 1e-9", worst < 1e-9);
        demo_check(d, "  lowest eigenvalue met, in units of mu (reported)", lowest, "", true);
    }

    /* 4. Derivatives on a bar of 72 tetrahedra with every free node moved at random. */
    {
        const size_t n = 3u * 4u * 3u * 3u;
        double      *before, *plus, *minus, worst = 0.0, scale = 0.0, top = 0.0, bottom = 0.0, rest;
        bar(s, &a->mesh, 3u, 2u, 2u, 0.1, 0.45, 1.0, false, false);
        nh_energy(s, s->x, s->gradient);
        for (rest = 0.0, i = 0u; i < n; ++i) rest = fmax(rest, fabs(s->gradient[i]));
        demo_check(d, "bar at rest: largest gradient / (mu x 1 m^2)", rest / s->mu, "< 1e-12", rest / s->mu < 1e-12);
        for (i = 0u; i < n; ++i) s->x[i] += s->held[i] ? 0.0 : 0.01 * noise(&rng);
        before = (double *)malloc(n * sizeof(double)); plus = (double *)malloc(n * sizeof(double)); minus = (double *)malloc(n * sizeof(double));
        nh_energy(s, s->x, s->gradient);
        for (i = 0u; i < n; ++i) {
            const double keep = s->x[i], g = s->gradient[i];
            double       up, down;
            if (s->held[i]) continue;
            s->x[i] = keep + 1e-6; up = nh_energy(s, s->x, NULL);
            s->x[i] = keep - 1e-6; down = nh_energy(s, s->x, NULL);
            s->x[i] = keep;
            worst = fmax(worst, fabs((up - down) / 2e-6 - g));
            scale = fmax(scale, fabs(g));
        }
        demo_check(d, "nodes moved at random: gradient against differences of the energy, / largest gradient", worst / scale, "< 1e-7", worst / scale < 1e-7);
        /* The unfiltered Newton direction d solves H d = -g, so the gradient's change along d must be -g. */
        memcpy(before, s->x, n * sizeof(double));
        s->filter = NH_NONE;
        nh_newton(s);
        for (i = 0u; i < n; ++i) { plus[i] = before[i] + 1e-4 * s->direction[i]; minus[i] = before[i] - 1e-4 * s->direction[i]; }
        nh_energy(s, before, s->gradient);
        memcpy(s->trial, s->gradient, n * sizeof(double));
        nh_energy(s, plus, s->gradient);
        memcpy(plus, s->gradient, n * sizeof(double));
        nh_energy(s, minus, s->gradient);
        for (i = 0u; i < n; ++i) { top = fmax(top, fabs((plus[i] - s->gradient[i]) / 2e-4 + s->trial[i])); bottom = fmax(bottom, fabs(s->trial[i])); }
        demo_check(d, "  Hessian: (gradient's change along the Newton direction + gradient) / largest gradient", top / bottom, "< 1e-6", top / bottom < 1e-6);
        demo_check(d, "  pivots replaced in that factorisation", s->replaced, "0", s->replaced == 0u);
        free(before); free(plus); free(minus);
    }

    /* 5. A pull of 0.1 % with the sides free. Linearised, eq. 13 is mu e:e + (lambda - mu) / 2 tr(e)^2, so the
     * bar should narrow by (lambda - mu) / 2 lambda of the stretch, not by the Poisson's ratio put in. */
    {
        const double wide = 0.2, length = 0.6;
        double       narrow = 0.0, decrement = 0.0;
        uint32_t     count;
        bar(s, &a->mesh, 6u, 2u, 2u, 0.1, 0.3, 1.001, false, false);
        for (i = 0u; i < s->nodes; ++i) {
            const double *p = &a->mesh.rest[3u * i];
            if (!s->held[3u * i]) continue;
            /* the ends keep their x only; one node keeps y and z, one more keeps z, against rigid motion */
            s->held[3u * i + 1u] = p[0] < 1e-9 && p[1] < 1e-9 && p[2] < 1e-9;
            s->held[3u * i + 2u] = p[0] < 1e-9 && p[2] < 1e-9 && (p[1] < 1e-9 || p[1] > wide - 1e-9);
        }
        s->filter = NH_ABS;
        for (count = 0u; count < 50u; ++count) {
            decrement = nh_newton(s);
            if (decrement < 1e-22 * s->lambda) break;
        }
        for (i = 0u; i < s->nodes; ++i) {
            const double *p = &a->mesh.rest[3u * i];
            if (fabs(p[0] - 0.5 * length) < 1e-9 && p[1] > wide - 1e-9 && p[2] < 1e-9) narrow = (s->x[3u * i + 1u] - s->x[1]) / wide - 1.0;
        }
        demo_check(d, "bar pulled 0.1 %, sides free, Poisson's ratio 0.3: narrowing / stretch", -narrow / 0.001, "", true);
        demo_check(d, "  that / ((lambda - mu) / 2 lambda) - 1", -narrow / 0.001 / ((s->lambda - s->mu) / (2.0 * s->lambda)) - 1.0, "|.| < 0.01",
                   fabs(-narrow / 0.001 / ((s->lambda - s->mu) / (2.0 * s->lambda)) - 1.0) < 0.01);
        demo_check(d, "  Newton iterations", count, "< 10", count < 10u);
    }

    /* 6. The paper's experiment: iterations of both filters until the decrement is below 1e-5 lambda
     * (section 7). Rule 1: absolute needs fewer; rule 2: within one of each other; rule 0: reported. */
    {
        static const struct { uint32_t cells; double poisson, amount; bool turned, everywhere; int rule; const char *label; } k_case[10] = {
            {10u, 0.3, 3.0, false, true, 2, "stretched to 3 lengths, Poisson 0.3"},
            {10u, 0.45, 3.0, false, true, 1, "stretched to 3, Poisson 0.45"},
            {10u, 0.495, 3.0, false, true, 1, "stretched to 3, Poisson 0.495"},
            {10u, 0.499, 3.0, false, true, 1, "stretched to 3, Poisson 0.499"},
            {10u, 0.495, 1.2, false, true, 0, "stretched to 1.2, Poisson 0.495"},
            {10u, 0.495, 0.5, false, true, 0, "squeezed to half, Poisson 0.495"},
            {10u, 0.495, 3.0, false, false, 0, "only the end pulled to 3, Poisson 0.495"},
            {10u, 0.495, 2.0, true, false, 0, "only the end pulled to 2 and turned, Poisson 0.495"},
            {16u, 0.495, 3.0, false, true, 1, "16 cubes long, stretched to 3, Poisson 0.495"},
            {16u, 0.495, 3.0, false, false, 1, "16 cubes long, only the end pulled to 3, Poisson 0.495"}};
        static const char *const k_rule[3] = {"reported", "< clamped", "clamped +- 1"};
        for (trial = 0u; trial < 10u; ++trial) {
            const uint32_t side = (3u * k_case[trial].cells + 5u) / 10u;
            double         energy[2] = {0.0, 0.0};
            uint32_t       count[2] = {0u, 0u}, replaced = 0u;
            char           label[160];
            for (k = 0; k < 2; ++k) {
                bar(s, &a->mesh, k_case[trial].cells, side, side, 1.0 / k_case[trial].cells, k_case[trial].poisson, k_case[trial].amount, k_case[trial].turned, k_case[trial].everywhere);
                s->filter = k ? NH_ABS : NH_CLAMP;
                for (count[k] = 1u; count[k] < 200u; ++count[k]) {
                    const double decrement = nh_newton(s);
                    replaced += s->replaced;
                    if (decrement < 1e-5 * s->lambda) break;
                }
                energy[k] = nh_energy(s, s->x, NULL);
            }
            snprintf(label, sizeof label, "%s: iterations, clamped at 0", k_case[trial].label);
            demo_check(d, label, count[0], "< 200", count[0] < 200u);
            demo_check(d, "  absolute", count[1], k_rule[k_case[trial].rule],
                       count[1] < 200u && (k_case[trial].rule == 0 || (k_case[trial].rule == 1 && count[1] < count[0]) || (k_case[trial].rule == 2 && count[1] <= count[0] + 1u && count[0] <= count[1] + 1u)));
            demo_check(d, "  energy where they stopped, clamped / absolute - 1", energy[0] / energy[1] - 1.0, "|.| < 0.03", fabs(energy[0] / energy[1] - 1.0) < 0.03);
            demo_check(d, "  pivots replaced, both", replaced, "0", replaced == 0u);
        }
    }
}

int main(int argc, char **argv) {
    static App app;
    static const DemoOption options[] = {
        {"--scene", .u32 = &app.scene, .lo = 0, .hi = 3, .help = "0 stretched to 3 lengths, 1 only the end pulled to 3, 2 end pulled to 2 and turned a quarter, 3 squeezed to half"},
        {"--poisson", .f32 = &app.poisson, .lo = 0.3, .hi = 0.499, .help = "Poisson's ratio"},
        {"--cells", .u32 = &app.cells, .lo = 4, .hi = 16, .help = "cubes along the bar; its section is 0.3 of that"},
        {"--tolerance", .f32 = &app.tolerance, .lo = -14, .hi = -2, .help = "log10 of (Newton decrement / lambda) at which a bar is done; the paper's is -5"},
        {"--trace", .flag = &app.trace, .help = "print every iteration"},
        {"--pace", .u32 = &app.pace, .lo = 1, .hi = 60, .help = "frames between Newton iterations"},
        {0}};
    Demo *d = &app.demo;
    app.poisson = 0.495f; app.pace = 6u; app.cells = 10u; app.tolerance = -5.0f;
    app.orbit = (DemoOrbit){.target = {1.5f, 0.0f, 0.0f}, .distance = 4.3f, .yaw = 0.0f, .pitch = 0.5f,
                            .fov_deg = 40.0f, .znear = 0.05f, .zfar = 50.0f, .min_distance = 0.5f, .max_distance = 20.0f};
    demo_defaults(d, "absnewton", "Absolute eigenvalue filtering");
    demo_open(d, options, argc, argv);
    canvas_open(d, &app.canvas, 60000u, CANVAS_RGB(38, 42, 52));
    seed(&app);
    if (d->headless && !d->frames) d->frames = 120u;
    while (demo_running(d)) {
        if (d->ui) app_ui(&app, d->ui);
        app_frame(&app);
    }
    if (d->check) app_check(&app);
    return demo_close(d);
}
