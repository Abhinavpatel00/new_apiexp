/* projdyn — Projective Dynamics: implicit time steps from constraint projections.
 *
 *   Draw -> Present          (the method runs in C: method.c)
 *
 * Bouaziz, Martin, Liu, Kavan, Pauly: "Projective Dynamics: Fusing Constraint
 * Projections for Fast Simulation" (SIGGRAPH 2014). A bar of 1536 tetrahedra
 * with a strain constraint and a volume constraint on each: scene 0 held at
 * one end, scene 1 twisted, scene 2 pulled to 1.6 times its length.
 *
 * --check tests the singular value decomposition, both projections against
 * differences, the factorisation, that the objective falls in every
 * iteration, that the iterations end at implicit Euler, a hanging bar against
 * rho g L^2 / 2E, momentum, and a time step of half a second.
 */
#include "canvas.h"
#include "method.h"
#include "softmesh.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct App {
    Demo      demo;   /* first: see demo.h */
    Canvas    canvas;
    DemoOrbit orbit;
    SoftMesh  mesh;
    Pd        body;
    uint32_t  scene, iterations, built, steps;
    float     stiffness, volume, limit;   /* log10 Pa, log10 Pa, +- range of the singular values */
    bool      keep_volume, restart;
    double    length;                     /* of the bar along x */
} App;

/* A bar of nx x ny x nz cubes at rest, nothing attached. */
static void build(App *a, uint32_t nx, uint32_t ny, uint32_t nz, double cell) {
    Pd      *s = &a->body;
    uint32_t t, i;
    int      r, c, k;
    free(a->mesh.rest); free(a->mesh.tet); free(a->mesh.face);
    free(s->q); free(s->v); free(s->mass); free(s->tet); free(s->target); free(s->attach);
    free(s->factor); free(s->inertia); free(s->rhs); free(s->project);
    memset(s, 0, sizeof *s);
    softmesh_cells(&a->mesh, nx, ny, nz, cell, NULL);
    s->nodes  = a->mesh.nodes;
    s->tets   = a->mesh.tets;
    s->q      = (double *)malloc((size_t)s->nodes * 3u * sizeof(double));
    s->v      = (double *)calloc((size_t)s->nodes * 3u, sizeof(double));
    s->target = (double *)calloc((size_t)s->nodes * 3u, sizeof(double));
    s->mass   = (double *)calloc(s->nodes, sizeof(double));
    s->attach = (double *)calloc(s->nodes, sizeof(double));
    s->tet    = (PdTet *)calloc(s->tets, sizeof(PdTet));
    memcpy(s->q, a->mesh.rest, (size_t)s->nodes * 3u * sizeof(double));
    for (t = 0u; t < s->tets; ++t) {
        PdTet        *e = &s->tet[t];
        const double *p[4];
        double        m[3][3], det;
        for (i = 0u; i < 4u; ++i) { e->node[i] = a->mesh.tet[4u * t + i]; p[i] = &a->mesh.rest[3u * e->node[i]]; }
        /* columns of m are the edges from node 0; the rows of its inverse are the gradients of nodes 1..3 */
        for (r = 0; r < 3; ++r)
            for (c = 0; c < 3; ++c) m[r][c] = p[c + 1][r] - p[0][r];
        det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
        for (r = 0; r < 3; ++r)
            for (c = 0; c < 3; ++c) {
                const int r1 = (c + 1) % 3, r2 = (c + 2) % 3, c1 = (r + 1) % 3, c2 = (r + 2) % 3;
                e->grad[r + 1][c] = (m[r1][c1] * m[r2][c2] - m[r1][c2] * m[r2][c1]) / det;
            }
        for (k = 0; k < 3; ++k) e->grad[0][k] = -e->grad[1][k] - e->grad[2][k] - e->grad[3][k];
        e->volume = det / 6.0;
        for (i = 0u; i < 4u; ++i) s->mass[e->node[i]] += 0.25 * 1000.0 * e->volume;   /* 1000 kg / m^3 */
    }
    s->h = 1.0 / 60.0; s->gravity = 9.81; s->iterations = 10u;
    s->stiffness = 1e5; s->low = s->high = 1.0; s->volume_low = s->volume_high = 1.0;
    a->length = cell * nx;
}

/* What the user's numbers mean for the solver; the matrix has to be factored again after. */
static void material(App *a) {
    Pd *s = &a->body;
    s->stiffness        = pow(10.0, a->stiffness);
    s->low              = 1.0 - a->limit;
    s->high             = 1.0 + a->limit;
    s->volume_stiffness = a->keep_volume ? pow(10.0, a->volume) : 0.0;
    pd_factor(s);
}

static void seed(App *a) {
    Pd      *s = &a->body;
    uint32_t i;
    build(a, 16u, 4u, 4u, 0.05);
    for (i = 0u; i < s->nodes; ++i) {
        const double x = a->mesh.rest[3u * i];
        s->q[3u * i + 1u] += 0.6;
        /* scene 0 holds the end at x = 0; the others hold both ends */
        if (x < 1e-9 || (a->scene > 0u && x > a->length - 1e-9)) s->attach[i] = 1e7;
        memcpy(&s->target[3u * i], &s->q[3u * i], sizeof(double[3]));
    }
    material(a);
    a->built = a->scene;
    a->steps = 0u;
}

static void app_ui(App *a, struct nk_context *ctx) {
    static const char *const k_scenes[3] = {"Held at one end", "Twisted", "Pulled"};
    const float before[3] = {a->stiffness, a->volume, a->limit};
    const bool  kept = a->keep_volume;
    if (nk_begin(ctx, "Projective Dynamics", nk_rect(16, 16, 330, 250), NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_TITLE)) {
        int iterations = (int)a->iterations;
        if (demo_ui_choice(ctx, "Scene", k_scenes, 3u, &a->scene)) a->restart = true;
        nk_layout_row_dynamic(ctx, 22, 1);
        nk_property_int(ctx, "Iterations", 1, &iterations, 100, 1, 1.0f);
        a->iterations = (uint32_t)iterations;
        demo_ui_float(ctx, "log10 stiffness, Pa", &a->stiffness, 3.5f, 7.0f);
        demo_ui_float(ctx, "Strain allowed, +-", &a->limit, 0.0f, 0.5f);
        demo_ui_bool(ctx, "Keep volume", &a->keep_volume);
        demo_ui_float(ctx, "log10 volume stiffness, Pa", &a->volume, 3.5f, 7.0f);
        nk_layout_row_dynamic(ctx, 24, 1);
        if (nk_button_label(ctx, "Restart")) a->restart = true;
    }
    nk_end(ctx);
    if (before[0] != a->stiffness || before[1] != a->volume || before[2] != a->limit || kept != a->keep_volume) material(a);
}

static void app_frame(App *a) {
    Demo    *d = &a->demo;
    Canvas  *c = &a->canvas;
    Pd      *s = &a->body;
    uint32_t i;

    if (a->restart || a->built != a->scene) { seed(a); a->restart = false; }
    /* The far end of scenes 1 and 2 moves: a turn every 4 s up to one and a half turns, or a pull to 1.6 lengths in 2 s. */
    {
        const double time = (double)a->steps / 60.0, turn = fmin(time * 1.5708, 9.42478), pull = fmin(time * 0.3, 0.6) * a->length;
        for (i = 0u; i < s->nodes && a->scene > 0u; ++i) {
            const double *r = &a->mesh.rest[3u * i], y = r[1] - 0.1, z = r[2] - 0.1;
            if (r[0] < a->length - 1e-9) continue;
            s->target[3u * i]      = r[0] + (a->scene == 2u ? pull : 0.0);
            s->target[3u * i + 1u] = 0.7 + (a->scene == 1u ? cos(turn) * y - sin(turn) * z : y);
            s->target[3u * i + 2u] = 0.1 + (a->scene == 1u ? sin(turn) * y + cos(turn) * z : z);
        }
    }
    s->iterations = a->iterations;
    pd_step(s, NULL);
    a->steps++;

    canvas_view_orbit(d, c, &a->orbit);
    for (i = 0u; i < 2u; ++i) {
        const float q[4][3] = {{-4, 0, -4}, {4, 0, -4}, {4, 0, 4}, {-4, 0, 4}};
        canvas_triangle(c, q[0], q[1 + i], q[2 + i], CANVAS_RGB(92, 98, 110));
    }
    for (i = 0u; i <= 32u; ++i) {
        const float at = -4.0f + 0.25f * (float)i, lines[4][3] = {{at, 0.002f, -4}, {at, 0.002f, 4}, {-4, 0.002f, at}, {4, 0.002f, at}};
        canvas_line(c, lines[0], lines[1], 0.004f, CANVAS_RGB(70, 75, 86));
        canvas_line(c, lines[2], lines[3], 0.004f, CANVAS_RGB(70, 75, 86));
    }
    softmesh_draw(&a->mesh, s->q, c, CANVAS_RGB(90, 160, 215), 0.0025f);
    for (i = 0u; i < s->nodes; ++i) {
        const float p[3] = {(float)s->target[3u * i], (float)s->target[3u * i + 1u], (float)s->target[3u * i + 2u]};
        if (s->attach[i] > 0.0) canvas_dot(c, p, 0.008f, CANVAS_RGB(235, 120, 50));
    }
    canvas_frame(d, c);
}

static void app_init(App *a) {
    canvas_open(&a->demo, &a->canvas, 60000u, CANVAS_RGB(38, 42, 52));
    seed(a);
    if (a->demo.headless && !a->demo.frames) a->demo.frames = 120u;
}

static double noise(uint32_t *state) {
    *state = *state * 1664525u + 1013904223u;
    return (double)(*state >> 8) / 8388608.0 - 1.0;
}

static void app_check(App *a) {
    Demo    *d = &a->demo;
    Pd      *s = &a->body;
    uint32_t rng = 7u, i, step, trial;
    int      r, c, k;

    /* 1. The decomposition, on 2000 random matrices, a third of them squashed flat or nearly, half inside out. */
    {
        double rebuilt = 0.0, orthogonal = 0.0, handed = 0.0, ordered = 0.0;
        for (trial = 0u; trial < 2000u; ++trial) {
            double f[3][3], u[3][3], sigma[3], v[3][3];
            for (r = 0; r < 3; ++r)
                for (c = 0; c < 3; ++c) f[r][c] = noise(&rng) * (trial % 3u == 0u && c == 2 ? (trial % 6u ? 1e-9 : 0.0) : 1.0);
            pd_svd(f, u, sigma, v);
            for (r = 0; r < 3; ++r)
                for (c = 0; c < 3; ++c) {
                    double back = 0.0, uu = 0.0, vv = 0.0;
                    for (k = 0; k < 3; ++k) { back += u[r][k] * sigma[k] * v[c][k]; uu += u[k][r] * u[k][c]; vv += v[k][r] * v[k][c]; }
                    rebuilt    = fmax(rebuilt, fabs(back - f[r][c]));
                    orthogonal = fmax(orthogonal, fmax(fabs(uu - (r == c)), fabs(vv - (r == c))));
                }
            for (k = 0; k < 2; ++k) {
                const double (*m)[3] = k ? v : u;
                handed = fmax(handed, fabs(m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]) - 1.0));
            }
            ordered = fmax(ordered, fmax(sigma[1] - sigma[0], fabs(sigma[2]) - sigma[1]));
        }
        demo_check(d, "singular values of 2000 random F: largest entry of U S V^T - F", rebuilt, "< 1e-12", rebuilt < 1e-12);
        demo_check(d, "  of U^T U - 1 and V^T V - 1", orthogonal, "< 1e-12", orthogonal < 1e-12);
        demo_check(d, "  det U - 1, det V - 1", handed, "< 1e-12", handed < 1e-12);
        demo_check(d, "  out of order: s1 - s0, |s2| - s1", ordered, "< 1e-12", ordered < 1e-12);
    }

    /* 2. Each projection p(F) is the nearest point of its set, so d/dF of |F - p(F)|^2 / 2 is F - p(F). */
    {
        double worst[2] = {0.0, 0.0}, outside[2] = {0.0, 0.0};
        for (trial = 0u; trial < 400u; ++trial) {
            double f[3][3], p[3][3], u[3][3], sigma[3], v[3][3];
            const int kind = (int)(trial % 2u);
            for (r = 0; r < 3; ++r)
                for (c = 0; c < 3; ++c) f[r][c] = (r == c ? 1.0 : 0.0) + 0.6 * noise(&rng);
            if (kind == 0) pd_project_strain(f, 0.9, 1.2, p); else pd_project_volume(f, 0.9, 1.1, p);
            pd_svd(p, u, sigma, v);
            outside[kind] = fmax(outside[kind], kind == 0 ? fmax(0.9 - sigma[2], sigma[0] - 1.2) : fmax(0.9 - sigma[0] * sigma[1] * sigma[2], sigma[0] * sigma[1] * sigma[2] - 1.1));
            for (r = 0; r < 3; ++r)
                for (c = 0; c < 3; ++c) {
                    double value[2];
                    const double keep = f[r][c], step_size = 1e-6;
                    for (k = 0; k < 2; ++k) {
                        double moved[3][3];
                        int    x, y;
                        f[r][c] = keep + (k ? -step_size : step_size);
                        if (kind == 0) pd_project_strain(f, 0.9, 1.2, moved); else pd_project_volume(f, 0.9, 1.1, moved);
                        value[k] = 0.0;
                        for (x = 0; x < 3; ++x)
                            for (y = 0; y < 3; ++y) value[k] += 0.5 * (f[x][y] - moved[x][y]) * (f[x][y] - moved[x][y]);
                    }
                    f[r][c] = keep;
                    worst[kind] = fmax(worst[kind], fabs((value[0] - value[1]) / (2.0 * step_size) - (f[r][c] - p[r][c])));
                }
        }
        demo_check(d, "strain projection onto singular values in [0.9, 1.2]: how far outside the result is", outside[0], "< 1e-12", outside[0] < 1e-12);
        demo_check(d, "  F - p(F) against differences of |F - p(F)|^2 / 2 (it is the nearest point)", worst[0], "< 1e-7", worst[0] < 1e-7);
        demo_check(d, "volume projection onto det in [0.9, 1.1]: how far outside the result is", outside[1], "< 1e-10", outside[1] < 1e-10);
        demo_check(d, "  F - p(F) against differences", worst[1], "< 1e-6", worst[1] < 1e-6);
    }

    /* 3. The scene with the bar held at one end: factorisation, falling objective, where the iterations end. */
    {
        double *trace = (double *)malloc(1001u * sizeof(double)), *gradient = (double *)malloc((size_t)s->nodes * 3u * sizeof(double));
        double *far = (double *)malloc((size_t)s->nodes * 3u * sizeof(double)), *start = (double *)malloc((size_t)s->nodes * 6u * sizeof(double));
        double  rise = 0.0, size = 0.0, left = 0.0, at_ten = 0.0, moved = 0.0, energy_gap;
        a->scene = 0u; a->stiffness = 5.5f; a->limit = 0.0f; a->keep_volume = true; a->volume = 5.0f;
        seed(a);
        s->volume_low = 0.95; s->volume_high = 1.05;
        demo_check(d, "system matrix, 425 x 425: largest |L L^T - A| / largest |A|", pd_factor(s), "< 1e-13", pd_factor(s) < 1e-13);
        s->iterations = 10u;
        for (step = 0u; step < 40u; ++step) {
            pd_step(s, trace);
            for (i = 0u; i < 10u; ++i) rise = fmax(rise, (trace[i + 1u] - trace[i]) / trace[0]);
        }
        demo_check(d, "40 steps of 10 iterations: largest rise of the objective in one iteration, / its start", rise, "<= 1e-13", rise <= 1e-13);
        /* one more step from the same state, with 10 and with 1000 iterations */
        memcpy(start, s->q, (size_t)s->nodes * 3u * sizeof(double));
        memcpy(start + s->nodes * 3u, s->v, (size_t)s->nodes * 3u * sizeof(double));
        s->iterations = 1000u;
        pd_step(s, trace);
        memcpy(far, s->q, (size_t)s->nodes * 3u * sizeof(double));
        pd_energy(s, s->q, gradient);
        for (i = 0u; i < s->nodes * 3u; ++i) { left += gradient[i] * gradient[i]; moved = fmax(moved, fabs(far[i] - start[i])); }
        energy_gap = (trace[10] - trace[1000]) / (trace[0] - trace[1000]);
        pd_energy(s, s->inertia, gradient);
        for (i = 0u; i < s->nodes * 3u; ++i) size += gradient[i] * gradient[i];
        demo_check(d, "one step, 1000 iterations: |gradient of the objective| / at the start (0 is implicit Euler)", sqrt(left / size), "< 1e-6", sqrt(left / size) < 1e-6);
        demo_check(d, "  share of the objective's fall still to come after 10 iterations (reported)", energy_gap, "", true);
        memcpy(s->q, start, (size_t)s->nodes * 3u * sizeof(double));
        memcpy(s->v, start + s->nodes * 3u, (size_t)s->nodes * 3u * sizeof(double));
        s->iterations = 10u;
        pd_step(s, NULL);
        for (i = 0u; i < s->nodes * 3u; ++i) at_ten = fmax(at_ten, fabs(s->q[i] - far[i]));
        demo_check(d, "  10 iterations - 1000 iterations, largest distance / largest move in the step (reported)", at_ten / moved, "", true);
        free(trace); free(gradient); free(far); free(start);
    }

    /* 4. A bar hanging from its top, 0.6 m long, with strain constraints alone: w |F - R|^2 / 2 is a
     * material with Young's modulus w and no sideways contraction, so the tip drops rho g L^2 / 2E. */
    {
        double tip = 0.0, expected, speed = 0.0;
        uint32_t tips = 0u;
        build(a, 2u, 12u, 2u, 0.05);
        s->stiffness = 3e5;
        for (i = 0u; i < s->nodes; ++i) {
            if (a->mesh.rest[3u * i + 1u] > 0.6 - 1e-9) s->attach[i] = 1e9;
            memcpy(&s->target[3u * i], &s->q[3u * i], sizeof(double[3]));
        }
        s->h = 0.05; s->iterations = 30u;
        pd_factor(s);
        for (step = 0u; step < 400u; ++step) pd_step(s, NULL);
        for (i = 0u; i < s->nodes; ++i) {
            speed = fmax(speed, fabs(s->v[3u * i + 1u]));
            if (a->mesh.rest[3u * i + 1u] < 1e-9) { tip -= s->q[3u * i + 1u]; tips++; }
        }
        expected = 1000.0 * 9.81 * 0.6 * 0.6 / (2.0 * 3e5);
        demo_check(d, "hanging bar at rest: fastest node, m / s", speed, "< 1e-6", speed < 1e-6);
        demo_check(d, "  drop of its lower end / (rho g L^2 / 2E) - 1", tip / tips / expected - 1.0, "< 0.02", fabs(tip / tips / expected - 1.0) < 0.02);
    }

    /* 5. A free bar thrown spinning, no gravity: momentum, and a time step of half a second. */
    {
        double before[3] = {0, 0, 0}, after[3] = {0, 0, 0}, spin[2] = {0.0, 0.0}, total = 0.0, largest = 0.0;
        for (trial = 0u; trial < 2u; ++trial) {
            build(a, 8u, 2u, 2u, 0.05);
            s->gravity = 0.0; s->h = trial ? 0.5 : 1.0 / 60.0; s->iterations = 20u;
            pd_factor(s);
            for (i = 0u; i < s->nodes; ++i) {
                const double *x = &s->q[3u * i];
                s->v[3u * i] = 0.3 - 4.0 * x[1]; s->v[3u * i + 1u] = 0.2 + 4.0 * (x[0] - 0.2); s->v[3u * i + 2u] = -0.1;
                for (k = 0; k < 3 && trial == 0u; ++k) before[k] += s->mass[i] * s->v[3u * i + k];
                if (trial == 0u) { spin[0] += s->mass[i] * (x[0] * s->v[3u * i + 1u] - x[1] * s->v[3u * i]); total += s->mass[i]; }
            }
            for (step = 0u; step < 120u; ++step) pd_step(s, NULL);
            for (i = 0u; i < s->nodes; ++i) {
                const double *x = &s->q[3u * i];
                for (k = 0; k < 3 && trial == 0u; ++k) after[k] += s->mass[i] * s->v[3u * i + k];
                if (trial == 0u) spin[1] += s->mass[i] * (x[0] * s->v[3u * i + 1u] - x[1] * s->v[3u * i]);
                if (trial == 1u) largest = fmax(largest, fabs(s->v[3u * i]) + fabs(s->v[3u * i + 1u]) + fabs(s->v[3u * i + 2u]));
            }
        }
        demo_check(d, "free spinning bar, 2 s: change of momentum / (mass x 1 m/s)",
                   (fabs(after[0] - before[0]) + fabs(after[1] - before[1]) + fabs(after[2] - before[2])) / total, "< 1e-10",
                   (fabs(after[0] - before[0]) + fabs(after[1] - before[1]) + fabs(after[2] - before[2])) / total < 1e-10);
        demo_check(d, "  angular momentum about z, after / before (implicit Euler loses some; reported)", spin[1] / spin[0], "", true);
        demo_check(d, "the same with a time step of 0.5 s, 120 steps: fastest node at the end, m / s (it started at 3.5)", largest, "< 3.5", largest < 3.5);
    }
}

int main(int argc, char **argv) {
    static App app;
    static const DemoOption options[] = {
        {"--scene", .u32 = &app.scene, .lo = 0, .hi = 2, .help = "0 held at one end, 1 twisted, 2 pulled to 1.6 lengths"},
        {"--iterations", .u32 = &app.iterations, .lo = 1, .hi = 100, .help = "local and global steps per time step (1/60 s)"},
        {"--stiffness", .f32 = &app.stiffness, .lo = 3.5, .hi = 7, .help = "log10 of the strain constraints' stiffness, Pa"},
        {"--limit", .f32 = &app.limit, .lo = 0, .hi = 0.5, .help = "singular values within 1 +- this cost nothing (strain limiting)"},
        {"--volume", .f32 = &app.volume, .flag = &app.keep_volume, .lo = 3.5, .hi = 7, .help = "add volume constraints with this log10 stiffness, Pa"},
        {0}};
    Demo *d = &app.demo;
    app.iterations = 10u; app.stiffness = 5.0f; app.volume = 6.0f;
    app.orbit = (DemoOrbit){.target = {0.45f, 0.55f, 0.1f}, .distance = 2.4f, .yaw = 0.5f, .pitch = 0.2f,
                            .fov_deg = 40.0f, .znear = 0.05f, .zfar = 50.0f, .min_distance = 0.5f, .max_distance = 15.0f};
    demo_defaults(d, "projdyn", "Projective Dynamics");
    demo_open(d, options, argc, argv);
    app_init(&app);
    while (demo_running(d)) {
        if (d->ui) app_ui(&app, d->ui);
        app_frame(&app);
    }
    if (d->check) app_check(&app);
    return demo_close(d);
}
