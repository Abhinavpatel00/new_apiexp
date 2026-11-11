/* ipbf — a fluid of particles kept at its rest density by a Newton step for each particle.
 *
 *   Draw -> Present          (the method runs in C: method.c)
 *
 * Diaz, Hsu, Montalvo-Ruiz, Giles, Yuksel: "Implicit Position-Based Fluids" (SIGGRAPH Asia
 * 2025), in the plane, next to Position Based Fluids (Macklin and Mueller 2013). The scenes
 * follow the paper's: two columns that fall and meet, a block packed to seven times the rest
 * density, and a column at rest. Particles are coloured by how far over the rest density they are.
 *
 * --check tests the kernel, the force and the Hessian against differences, that the approximate
 * Hessian is positive definite where the exact one is not, the energy over iterations, the
 * column at rest, the falling columns at four time steps and the packed block.
 */
#include "canvas.h"
#include "method.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct App {
    Demo     demo;   /* first: see demo.h */
    Canvas   canvas;
    Fl       world;
    uint32_t scene, built, method, iterations, rate;   /* rate: time steps a second */
    float    compliance, clock;
    bool     no_damping, exact, active, restart;
} App;

static void seed(Fl *s, uint32_t scene) {
    fl_open(s, 4.0, 3.0, 0.05);
    if (scene == 0u) { fl_block(s, 0.0, 0.0, 1.0, 2.0, 0.05); fl_block(s, 3.0, 0.0, 4.0, 2.0, 0.05); }
    if (scene == 1u) fl_block(s, 1.7, 1.2, 2.3, 1.8, 0.05 / sqrt(7.0));
    if (scene == 2u) fl_block(s, 0.0, 0.0, 4.0, 1.0, 0.05);
}

static void app_ui(App *a, struct nk_context *ctx) {
    static const char *const k_scenes[3] = {"Two columns", "A block at 7x the rest density", "A column at rest"};
    static const char *const k_methods[2] = {"Implicit position-based fluids", "Position based fluids"};
    static const char *const k_rates[6] = {"1/30 s", "1/60 s", "1/120 s", "1/240 s", "1/480 s", "1/960 s"};
    Fl      *s = &a->world;
    uint32_t rate = a->rate <= 30u ? 0u : a->rate <= 60u ? 1u : a->rate <= 120u ? 2u : a->rate <= 240u ? 3u : a->rate <= 480u ? 4u : 5u;
    float    iterations = (float)a->iterations;
    bool     damping = !a->no_damping;
    if (nk_begin(ctx, "Implicit position-based fluids", nk_rect(16, 16, 340, 330), NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_TITLE)) {
        if (demo_ui_choice(ctx, "Scene", k_scenes, 3u, &a->scene)) a->restart = true;
        demo_ui_choice(ctx, "Method", k_methods, 2u, &a->method);
        demo_ui_choice(ctx, "Time step", k_rates, 6u, &rate);
        demo_ui_float(ctx, "Iterations", &iterations, 1.0f, 16.0f);
        demo_ui_float(ctx, "Compliance", &a->compliance, 0.0f, 10.0f);
        demo_ui_bool(ctx, "Damping (eq. 17)", &damping);
        demo_ui_bool(ctx, "Exact second-order term", &a->exact);
        demo_ui_text(ctx, "%u particles", s->fluid);
        demo_ui_text(ctx, "mean density over rest: +%.2f %%", 100.0 * s->error);
        demo_ui_text(ctx, "densest: %.2f x rest", s->densest);
        nk_layout_row_dynamic(ctx, 24, 1);
        if (nk_button_label(ctx, "Restart")) a->restart = true;
    }
    nk_end(ctx);
    a->rate = 30u << rate; a->iterations = (uint32_t)(iterations + 0.5f); a->no_damping = !damping;
}

static void app_frame(App *a) {
    Demo       *d = &a->demo;
    Canvas     *c = &a->canvas;
    Fl         *s = &a->world;
    const float corner[4][3] = {{0, 0, 0}, {4, 0, 0}, {4, 3, 0}, {0, 3, 0}};
    uint32_t    i;

    if (a->restart || a->built != a->scene) { seed(s, a->scene); a->built = a->scene; a->restart = false; a->clock = 0.0f; }
    s->method = (FlMethod)a->method; s->iterations = a->iterations; s->h = 1.0 / a->rate;
    s->compliance = a->compliance; s->damping = !a->no_damping; s->exact = a->exact; s->active = a->active;
    /* a frame is 1/60 s: as many whole steps as have come due */
    for (a->clock += 1.0f / 60.0f; a->clock > 0.999f / (float)a->rate; a->clock -= 1.0f / (float)a->rate) fl_step(s);

    canvas_view_flat(d, c, 2.0f, 1.5f, 1.65f);
    for (i = 0u; i < 4u; ++i) canvas_line(c, corner[i], corner[(i + 1u) % 4u], 0.02f, CANVAS_RGB(120, 126, 140));
    for (i = 0u; i < s->fluid; ++i) {
        const float at[3] = {(float)s->x[2u * i], (float)s->x[2u * i + 1u], 0.1f};
        const float over = fminf(fmaxf((float)s->c[i] / 0.25f, 0.0f), 1.0f);   /* red at 25 % over the rest density */
        canvas_dot(c, at, 0.5f * (float)s->spacing, CANVAS_RGB((int)(70.0f + 170.0f * over), (int)(150.0f - 70.0f * over), (int)(230.0f - 170.0f * over)));
    }
    canvas_frame(d, c);
}

static double kinetic(const Fl *s) {
    double   energy = 0.0;
    uint32_t i;
    for (i = 0u; i < s->fluid; ++i) energy += 0.5 * s->mass * (s->v[2u * i] * s->v[2u * i] + s->v[2u * i + 1u] * s->v[2u * i + 1u]);
    return energy;
}
static double height(const Fl *s) {
    double   sum = 0.0;
    uint32_t i;
    for (i = 0u; i < s->fluid; ++i) sum += s->x[2u * i + 1u] / s->fluid;
    return sum;
}
/* `steps` time steps; the mean of the density error over them and the fastest particle in any. */
static void run(Fl *s, uint32_t steps, double *error, double *fastest) {
    uint32_t n, i;
    *error = *fastest = 0.0;
    for (n = 0u; n < steps; ++n) {
        fl_step(s);
        *error += s->error / steps;
        for (i = 0u; i < s->fluid; ++i) *fastest = fmax(*fastest, hypot(s->v[2u * i], s->v[2u * i + 1u]));
    }
}
/* a lattice from lo to hi in a box of 1 m, packed to `step` between particles, and shaken */
static void packed(Fl *s, double lo, double hi, double step) {
    uint32_t i;
    fl_open(s, 1.0, 1.0, 0.05);
    fl_block(s, lo, lo, hi, hi, step);
    srand(7u);
    for (i = 0u; i < 2u * s->fluid; ++i) s->x[i] += 0.006 * (rand() / (double)RAND_MAX - 0.5);
    memcpy(s->y, s->x, 2u * (s->fluid + s->walls) * sizeof(double));
    for (i = 0u; i < 2u * s->fluid; ++i) s->y[i] += 0.004 * (rand() / (double)RAND_MAX - 0.5);
}

static void app_check(App *a) {
    Demo    *d = &a->demo;
    Fl       w = {0}, *s = &w;
    double   k[3], kp[3], km[3], error, fastest, speed[4][4], over[4][4];
    uint32_t i, n, m;
    int      j;
    char     label[160];

    /* 1: the kernel */
    {
        double integral = 0.0, slope = 0.0, curve = 0.0, lattice = 0.0;
        fl_open(s, 2.0, 2.0, 0.05);
        for (i = 0u; i < 20000u; ++i) {
            const double r = (i + 0.5) * s->radius / 20000.0, e = 1e-5 * s->radius;
            fl_kernel(s, r, k); fl_kernel(s, r + e, kp); fl_kernel(s, r - e, km);
            integral += 2.0 * 3.14159265358979323846 * r * k[0] * s->radius / 20000.0;
            slope = fmax(slope, fabs((kp[0] - km[0]) / (2.0 * e) - k[1]) * s->radius * s->radius * s->radius);
            curve = fmax(curve, fabs((kp[1] - km[1]) / (2.0 * e) - k[2]) * s->radius * s->radius * s->radius * s->radius);
        }
        fl_block(s, 0.0, 0.0, 2.0, 2.0, 0.05);
        fl_constraints(s, s->x);
        for (i = 0u; i < s->fluid; ++i) lattice = fmax(lattice, fabs(s->c[i]));
        demo_check(d, "cubic spline kernel: integral over the plane", integral, "1 to 1e-6", fabs(integral - 1.0) < 1e-6);
        demo_check(d, "  dW/dr against differences, x radius^3", slope, "< 1e-6", slope < 1e-6);
        demo_check(d, "  d2W/dr2 against differences, x radius^4", curve, "< 1e-4", curve < 1e-4);
        demo_check(d, "a box full of the lattice, walls included: largest |density / rest - 1|", lattice, "< 1e-12", lattice < 1e-12);
    }

    /* 2: eq. 10 and eq. 11 with the exact eq. 15 against differences of eq. 7; section 3.5 */
    {
        double   force = 0.0, hessian = 0.0, least = 1e300, exact_least = 1e300, f[2], H[3], fp[2], fm[2], Hd[3], scale_f = 0.0, scale_h = 0.0;
        uint32_t indefinite = 0u, inactive = 0u;
        packed(s, 0.0, 1.0, 1.0 / 24.0);   /* the whole box, so that no constraint is clamped */
        s->active = true;
        fl_constraints(s, s->x);
        for (i = 0u; i < s->fluid; ++i) inactive += s->c[i] <= 0.0;
        for (i = 0u; i < s->fluid; i += 7u) {
            const double e = 1e-6 * s->spacing;
            for (j = 0; j < 2; ++j) {
                double up, down;
                s->x[2u * i + (uint32_t)j] += e;       fl_constraints(s, s->x); up   = fl_energy(s, s->x, s->y, 0.5); fl_local(s, s->x, s->y, 0.5, true, i, fp, Hd);
                s->x[2u * i + (uint32_t)j] -= 2.0 * e; fl_constraints(s, s->x); down = fl_energy(s, s->x, s->y, 0.5); fl_local(s, s->x, s->y, 0.5, true, i, fm, Hd);
                s->x[2u * i + (uint32_t)j] += e;       fl_constraints(s, s->x);
                fl_local(s, s->x, s->y, 0.5, true, i, f, H);
                force   = fmax(force, fabs(-(up - down) / (2.0 * e) - f[j]));
                scale_f = fmax(scale_f, fabs(f[j]));
                /* H = -df/dx: row j is {H[j], H[j + 1]} */
                hessian = fmax(hessian, fmax(fabs(-(fp[0] - fm[0]) / (2.0 * e) - H[j]), fabs(-(fp[1] - fm[1]) / (2.0 * e) - H[j + 1])));
                scale_h = fmax(scale_h, fabs(H[0]) + fabs(H[2]));
            }
        }
        /* eigenvalues with and without the approximation, at 7x the rest density where C is large */
        fl_open(s, 4.0, 3.0, 0.05); fl_block(s, 1.7, 1.2, 2.3, 1.8, 0.05 / sqrt(7.0));
        srand(3u);
        for (i = 0u; i < 2u * s->fluid; ++i) s->x[i] += 0.004 * (rand() / (double)RAND_MAX - 0.5);
        fl_constraints(s, s->x);
        for (i = 0u; i < s->fluid; ++i) {
            fl_local(s, s->x, s->x, 0.0, true, i, f, H);
            exact_least = fmin(exact_least, (0.5 * (H[0] + H[2]) - sqrt(0.25 * (H[0] - H[2]) * (H[0] - H[2]) + H[1] * H[1])) / s->scale);
            indefinite += 0.5 * (H[0] + H[2]) < sqrt(0.25 * (H[0] - H[2]) * (H[0] - H[2]) + H[1] * H[1]);
            fl_local(s, s->x, s->x, 0.0, false, i, f, H);
            least = fmin(least, (0.5 * (H[0] + H[2]) - sqrt(0.25 * (H[0] - H[2]) * (H[0] - H[2]) + H[1] * H[1])) / s->scale);
        }
        demo_check(d, "576 particles at 1.44x the rest density: constraints that the clamp switched off", inactive, "0", inactive == 0u);
        demo_check(d, "  eq. 10 against differences of eq. 7, relative to the largest force", force / scale_f, "< 1e-6", force / scale_f < 1e-6);
        demo_check(d, "  eq. 11 with the exact eq. 15 against differences of eq. 10, relative", hessian / scale_h, "< 1e-5", hessian / scale_h < 1e-5);
        demo_check(d, "1024 particles at 7x the rest density, alpha 0: with an indefinite exact Hessian", indefinite, "> 100", indefinite > 100u);
        demo_check(d, "  smallest eigenvalue of the exact Hessian, in units of a lattice's", exact_least, "< 0", exact_least < 0.0);
        demo_check(d, "  smallest eigenvalue with the approximation of section 3.5", least, "> 0", least > 0.0);
    }

    /* 3: the energy of eq. 7 after 1 .. 32 iterations of one time step */
    for (m = 0u; m < 3u; ++m) {
        double before = 0.0, last = 0.0, settled = 0.0;
        for (n = 0u; n < 7u; ++n) {
            packed(s, 0.15, 0.83, 0.0425);
            memcpy(s->y, s->x, 2u * (s->fluid + s->walls) * sizeof(double));
            s->gravity = 0.0; s->iterations = n ? 1u << (n - 1u) : 1u; s->method = m == 2u ? FL_PBF : FL_IPBF; s->compliance = m == 1u ? 0.5 : 0.0;
            fl_constraints(s, s->x);
            before = fl_energy(s, s->x, s->y, s->compliance);
            if (n == 0u) continue;
            fl_step(s);
            error = fl_energy(s, s->x, s->y, s->compliance) / before;
            snprintf(label, sizeof label, "%s%s, %2u iterations: energy of eq. 7 / before", n == 1u ? "" : "  ", m == 2u ? "PBF" : m == 1u ? "IPBF compliance 0.5" : "IPBF alpha 0", s->iterations);
            if (m == 2u) demo_check(d, label, error, "info", true);
            if (m != 2u) demo_check(d, label, error, n == 1u ? "< 1" : "< at half as many", error < (n == 1u ? 1.0 : last));
            settled = fabs(error - last) / error;
            last    = error;
        }
        if (m == 1u) demo_check(d, "  change from 16 to 32 iterations, relative", settled, "< 1e-2", settled < 1e-2);
    }

    /* 4: a column at rest, 4 s at 1/240 s */
    {
        static const struct { const char *name; FlMethod method; uint32_t iterations; bool damping, active; double compliance; } k_rows[] = {
            {"IPBF 2 iterations", FL_IPBF, 2u, true, false, 0.0},   {"IPBF 4 iterations", FL_IPBF, 4u, true, false, 0.0},
            {"IPBF 8 iterations", FL_IPBF, 8u, true, false, 0.0},   {"PBF 2 iterations", FL_PBF, 2u, true, false, 0.0},
            {"PBF 4 iterations", FL_PBF, 4u, true, false, 0.0},     {"PBF 8 iterations", FL_PBF, 8u, true, false, 0.0},
            {"IPBF 4 iterations, no damping", FL_IPBF, 4u, false, false, 0.0},
            {"IPBF 4 iterations, clamped constraints left out of the Hessian", FL_IPBF, 4u, true, true, 0.0},
            {"IPBF 4 iterations, compliance 0.1", FL_IPBF, 4u, true, false, 0.1}, {"IPBF 4 iterations, compliance 1", FL_IPBF, 4u, true, false, 1.0},
            {"IPBF 4 iterations, compliance 10", FL_IPBF, 4u, true, false, 10.0}};
        double result[11][3];
        for (n = 0u; n < 11u; ++n) {
            fl_open(s, 1.0, 1.5, 0.05); fl_block(s, 0.0, 0.0, 1.0, 0.5, 0.05);
            s->method = k_rows[n].method; s->iterations = k_rows[n].iterations; s->damping = k_rows[n].damping; s->active = k_rows[n].active; s->compliance = k_rows[n].compliance;
            run(s, 720u, &error, &fastest);
            run(s, 240u, &error, &fastest);
            result[n][0] = error; result[n][1] = height(s) / 0.25; result[n][2] = fastest;
            snprintf(label, sizeof label, "column at rest, %s: mean density over rest in the 4th second", k_rows[n].name);
            demo_check(d, label, error, "info", true);
            demo_check(d, "  mean height / at the start", result[n][1], "info", true);
            demo_check(d, "  fastest particle in the 4th second, m/s", fastest, "info", true);
        }
        demo_check(d, "column at rest: IPBF error halves from 2 to 4 iterations (ratio)", result[1][0] / result[0][0], "0.4 .. 0.6", result[1][0] / result[0][0] > 0.4 && result[1][0] / result[0][0] < 0.6);
        demo_check(d, "  and from 4 to 8", result[2][0] / result[1][0], "0.4 .. 0.6", result[2][0] / result[1][0] > 0.4 && result[2][0] / result[1][0] < 0.6);
        demo_check(d, "  IPBF error / PBF error at 4 iterations", result[1][0] / result[4][0], "0.8 .. 1.25", result[1][0] / result[4][0] > 0.8 && result[1][0] / result[4][0] < 1.25);
        demo_check(d, "  fastest particle without damping / with", result[6][2] / result[1][2], "> 2", result[6][2] / result[1][2] > 2.0);
        demo_check(d, "  height with clamped constraints left out of the Hessian / at the start", result[7][1], "> 1.1", result[7][1] > 1.1);
        demo_check(d, "  height at compliance 10 / at compliance 0", result[10][1] / result[1][1], "< 0.98", result[10][1] / result[1][1] < 0.98);
    }

    /* 5: two columns, 3 s with 2 iterations at four time steps */
    for (m = 0u; m < 3u; ++m)
        for (n = 0u; n < 4u; ++n) {
            fl_open(s, 2.0, 1.5, 0.05); fl_block(s, 0.0, 0.0, 0.5, 1.0, 0.05); fl_block(s, 1.5, 0.0, 2.0, 1.0, 0.05);
            s->method = m == 2u ? FL_PBF : FL_IPBF; s->damping = m == 0u; s->iterations = 2u; s->h = 1.0 / (240u >> n);
            run(s, 3u * (240u >> n), &error, &fastest);
            snprintf(label, sizeof label, "two columns, %s, 2 iterations, h 1/%u s: mean density over rest", m == 2u ? "PBF" : m == 1u ? "IPBF no damping" : "IPBF", 240u >> n);
            demo_check(d, label, error, "info", true);
            speed[m][n] = fastest; over[m][n] = error;
            demo_check(d, "  fastest particle, m/s", fastest, m == 0u ? "< 10" : "info", m != 0u || fastest < 10.0);
            demo_check(d, "  kinetic energy at 3 s / potential energy that the fall to a level surface gives up", kinetic(s) / (s->fluid * s->mass * s->gravity * 0.25), "info", true);
            demo_check(d, "  mean height at 3 s / of a level surface", height(s) / 0.25, "info", true);
        }
    demo_check(d, "two columns: IPBF error at 1/30 s / at 1/240 s", over[0][3] / over[0][0], "> 10", over[0][3] / over[0][0] > 10.0);
    demo_check(d, "  PBF fastest / IPBF fastest at 1/60 s", speed[2][2] / speed[0][2], "> 1.3", speed[2][2] / speed[0][2] > 1.3);
    demo_check(d, "  PBF fastest / IPBF fastest at 1/30 s", speed[2][3] / speed[0][3], "> 1.3", speed[2][3] / speed[0][3] > 1.3);
    demo_check(d, "  PBF error / IPBF error at 1/60 s", over[2][2] / over[0][2], "info", true);
    demo_check(d, "  PBF error / IPBF error at 1/30 s", over[2][3] / over[0][3], "info", true);

    /* 6: a block at 7x the rest density, 3 iterations at 1/120 s, no gravity */
    for (m = 0u; m < 4u; ++m) {
        double start;
        fl_open(s, 4.0, 3.0, 0.05); fl_block(s, 1.7, 1.2, 2.3, 1.8, 0.05 / sqrt(7.0));
        s->method = m == 3u ? FL_PBF : FL_IPBF; s->damping = m != 1u; s->exact = m == 2u; s->iterations = 3u; s->h = 1.0 / 120.0; s->gravity = 0.0;
        fl_constraints(s, s->x);
        for (start = 0.0, i = 0u; i < s->fluid; ++i) start = fmax(start, s->c[i] + 1.0);
        run(s, 20u, &error, &fastest);
        snprintf(label, sizeof label, "block at %.2fx rest, %s: densest after 20 steps", start, m == 3u ? "PBF" : m == 2u ? "IPBF with the exact second-order term" : m == 1u ? "IPBF no damping" : "IPBF");
        speed[3][m] = fastest;
        demo_check(d, label, s->densest, m == 0u ? "< 1.5" : "info", m != 0u || s->densest < 1.5);
        demo_check(d, "  fastest particle in those steps, m/s", fastest, "info", true);
    }
    demo_check(d, "block: fastest with the exact second-order term / with the approximation", speed[3][2] / speed[3][0], "> 5", speed[3][2] / speed[3][0] > 5.0);
    demo_check(d, "  fastest with PBF / with IPBF", speed[3][3] / speed[3][0], "> 2", speed[3][3] / speed[3][0] > 2.0);
    fl_close(s);
    (void)a;
}

int main(int argc, char **argv) {
    static App app;
    static const DemoOption options[] = {
        {"--scene", .u32 = &app.scene, .lo = 0, .hi = 2, .help = "0 two columns, 1 a block at 7x the rest density, 2 a column at rest"},
        {"--method", .u32 = &app.method, .lo = 0, .hi = 1, .help = "0 implicit position-based fluids, 1 position based fluids"},
        {"--iterations", .u32 = &app.iterations, .lo = 1, .hi = 64, .help = "iterations in a time step"},
        {"--rate", .u32 = &app.rate, .lo = 30, .hi = 960, .help = "time steps a second"},
        {"--compliance", .f32 = &app.compliance, .lo = 0, .hi = 100, .help = "weight of inertia against pressure (the paper's alpha, rescaled); 0: incompressible"},
        {"--no-damping", .flag = &app.no_damping, .help = "leave out the damping of section 3.6"},
        {"--exact", .flag = &app.exact, .help = "the exact second-order term instead of the approximation of section 3.5"},
        {"--active", .flag = &app.active, .help = "leave constraints that the clamp set to zero out of the Hessian"},
        {0}};
    Demo *d = &app.demo;
    app.iterations = 4u; app.rate = 240u; app.built = ~0u;
    demo_defaults(d, "ipbf", "Implicit position-based fluids");
    demo_open(d, options, argc, argv);
    canvas_open(d, &app.canvas, 120000u, CANVAS_RGB(24, 27, 36));
    if (d->headless && !d->frames) d->frames = 90u;
    while (demo_running(d)) {
        if (d->ui) app_ui(&app, d->ui);
        app_frame(&app);
    }
    if (d->check) app_check(&app);
    fl_close(&app.world);
    return demo_close(d);
}
