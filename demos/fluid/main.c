/* fluid — Stam's stable fluids in compute, on a staggered grid.
 *
 *   AdvectVelocity -> AdvectDye -> CopyDye
 *     -> Divergence -> Jacobi x K (pressure) -> Project
 *     -> Show -> Present
 *
 * Two jets blow dyed fluid into a closed box. Every step ends with the
 * projection: find the pressure whose gradient, subtracted, leaves a
 * velocity field with nothing flowing out of any cell. --check measures how
 * much is left.
 */
#include "demo.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SHADERS "demos/fluid/shaders/"
#define N       256u
#define MAX_K   48u

typedef struct Push {
    uint32_t src, aux, dst, sampler;
    float    size, dt, time, kind;
    float    fade[4];
} Push;
_Static_assert(sizeof(Push) == 48, "Push must match fluid.slang");

enum { U0, U1, D0, D1, P0, P1, DIV, IMAGES };

typedef struct App {
    Demo     demo; /* first: see demo.h */
    Target   img[IMAGES], hdr;
    uint32_t idx[IMAGES];
    Pipe     advect_pipe, divergence_pipe, jacobi_pipe, project_pipe, copy_pipe, relax_pipe, show_pipe;
    AttachDesc show_color;
    FrameProgram *solve;        /* the pressure passes and the projection alone: --check runs it repeatedly */
    Target   reads_dye[2], reads_jacobi_even[2], reads_jacobi_odd[2], reads_project[2], reads_show[3];
    uint32_t iterations, view, step;
    float    velocity_kept, dye_kept;
    float    omega;             /* over-relaxation of the in-place solver */
    bool     jacobi;            /* --jacobi: the plain solver, for comparison */
} App;

static const char *const k_views[] = {"dye", "speed", "pressure"};

static void step(App *a, PassCtx *p, int src, int aux, int dst, float kind) {
    Push push;
    memset(&push, 0, sizeof push);
    push.src = a->idx[src]; push.aux = a->idx[aux]; push.dst = a->idx[dst];
    push.sampler = a->demo.linear_clamp_index;
    push.size = (float)N;
    push.dt   = 1.0f / 60.0f;                       /* fixed: runs repeat */
    push.time = (float)a->step / 60.0f;
    push.kind = kind;
    push.fade[0] = a->velocity_kept; push.fade[1] = a->dye_kept; push.fade[2] = (float)a->view;
    push.fade[3] = a->omega;
    pass_push(p, &push, sizeof push);
}
static void grid(PassCtx *p) { pass_dispatch(p, N / 8u, N / 8u, 1u); }

/* The pressure passes and the projection, in the order solver_passes() put
 * them in `program`. */
static void solve_pressure(App *a, FrameProgram *program) {
    PassCtx *p;
    uint32_t i;
    for (i = 0u; i < a->iterations; ++i) {
        const bool even = (i & 1u) == 0u;
        p = frame_pass(a->demo.gpu, program, NULL);
        if (a->jacobi) step(a, p, even ? P0 : P1, DIV, even ? P1 : P0, 0.0f);   /* "JacobiA" / "JacobiB" */
        else           step(a, p, P0, DIV, P0, even ? 0.0f : 1.0f);             /* "RelaxRed" / "RelaxBlack" */
        grid(p);
    }
    p = frame_pass(a->demo.gpu, program, "Project");
    step(a, p, U1, P0, U0, 0.0f);
    grid(p);
}

/* One frame: what changed since the last one, then every pass, in the order app_init listed them. */
static void app_frame(App *a) {
    Demo    *d = &a->demo;
    PassCtx *pass;
    ++a->step;
    d->present.tonemap = DEMO_TONEMAP_NONE;

    if (!demo_frame_begin(d)) return;
    pass = demo_pass(d, "AdvectVelocity"); step(a, pass, U0, U0, U1, 0.0f); grid(pass);
    pass = demo_pass(d, "AdvectDye");      step(a, pass, D0, U0, D1, 1.0f); grid(pass);
    pass = demo_pass(d, "CopyDye");        step(a, pass, D1, D1, D0, 0.0f); grid(pass);
    pass = demo_pass(d, "Divergence");     step(a, pass, U1, U1, DIV, 0.0f); grid(pass);
    solve_pressure(a, d->program);
    pass = demo_pass(d, "Show");
    step(a, pass, D0, U0, P0, 0.0f);
    pass_draw_fullscreen_triangle(pass, d->width, d->height);
    demo_present(d);
    demo_frame_end(d);
}

static void compute_pass(PassDesc *p, const char *name, Pipe pipe,
                         Target *reads, uint32_t read_count, Target *write) {
    memset(p, 0, sizeof *p);
    strcpy(p->name, name);
    p->kind   = PASS_COMPUTE;
    p->pipe   = pipe;
    p->state  = draw_state_opaque(SC_1, CULL_NONE);
    p->tex_reads  = reads; p->tex_read_count  = read_count;
    p->tex_writes = write; p->tex_write_count = 1u;
}

/* The pressure starts from last frame's (it changes little) and ends in P0
 * either way: Jacobi goes back and forth between two images an even number of
 * times; the in-place solver updates one checkerboard colour per pass. Both
 * get the same number of passes. Then the projection. Returns the count. */
static uint32_t solver_passes(App *a, PassDesc *passes) {
    uint32_t count = 0u, i;
    for (i = 0u; i < a->iterations; ++i) {
        const bool even = (i & 1u) == 0u;
        if (a->jacobi)
            compute_pass(&passes[count++], even ? "JacobiA" : "JacobiB", a->jacobi_pipe,
                         even ? a->reads_jacobi_even : a->reads_jacobi_odd, 2u, even ? &a->img[P1] : &a->img[P0]);
        else
            compute_pass(&passes[count++], even ? "RelaxRed" : "RelaxBlack", a->relax_pipe, &a->img[DIV], 1u, &a->img[P0]);
    }
    compute_pass(&passes[count++], "Project", a->project_pipe, a->reads_project, 2u, &a->img[U0]);
    return count;
}

static void app_init(App *a) {
    static const char *const k_names[IMAGES] = {"velocity", "velocity_advected", "dye", "dye_advected",
                                                "pressure", "pressure_other", "divergence"};
    Demo     *d = &a->demo;
    ShaderId  cs[6], show_fs;
    PassDesc  passes[MAX_K + 12u];
    uint32_t  count = 0u, i;
    static const char *const k_cs[6] = {"advect", "divergence", "jacobi", "project", "copy", "relax"};
    Pipe     *pipes[6] = {&a->advect_pipe, &a->divergence_pipe, &a->jacobi_pipe, &a->project_pipe, &a->copy_pipe,
                          &a->relax_pipe};
    char      path[96];

    if (a->iterations & 1u) demo_fatal(d, "--iterations must be even");
    for (i = 0u; i < IMAGES; ++i) {
        a->img[i] = demo_target(d, k_names[i], (i == D0 || i == D1) ? FMT_RGBA16F : FMT_RGBA32F, N, N, true, &a->idx[i]);
    }
    a->hdr = demo_target(d, "fluid_hdr", FMT_RGBA16F, d->width, d->height, false, NULL);
    for (i = 0u; i < 6u; ++i) {
        snprintf(path, sizeof path, SHADERS "%s.comp.spv", k_cs[i]);
        cs[i] = demo_shader(d, path, STAGE_COMPUTE);
        *pipes[i] = pipe_intern_compute(d->gpu, cs[i], "main");
    }
    show_fs = demo_shader(d, SHADERS "show.frag.spv", STAGE_FRAGMENT);
    a->show_pipe = pipe_intern_graphics(d->gpu, d->fullscreen_vs, show_fs, FMT_RGBA16F, FMT_NONE, BLEND_OPAQUE);

    a->reads_dye[0] = a->img[D0]; a->reads_dye[1] = a->img[U0];
    a->reads_jacobi_even[0] = a->img[P0]; a->reads_jacobi_even[1] = a->img[DIV];
    a->reads_jacobi_odd[0]  = a->img[P1]; a->reads_jacobi_odd[1]  = a->img[DIV];
    a->reads_project[0] = a->img[U1]; a->reads_project[1] = a->img[P0];
    a->reads_show[0] = a->img[D0]; a->reads_show[1] = a->img[U0]; a->reads_show[2] = a->img[P0];

    compute_pass(&passes[count++], "AdvectVelocity", a->advect_pipe, &a->img[U0], 1u, &a->img[U1]);
    compute_pass(&passes[count++], "AdvectDye", a->advect_pipe, a->reads_dye, 2u, &a->img[D1]);
    compute_pass(&passes[count++], "CopyDye", a->copy_pipe, &a->img[D1], 1u, &a->img[D0]);
    compute_pass(&passes[count++], "Divergence", a->divergence_pipe, &a->img[U1], 1u, &a->img[DIV]);
    count += solver_passes(a, &passes[count]);
    {
        PassDesc *p = &passes[count++];
        memset(p, 0, sizeof *p);
        a->show_color = attach_color(a->hdr, ATT_WRITE);
        strcpy(p->name, "Show");
        p->kind  = PASS_GRAPHICS;
        p->pipe  = a->show_pipe;
        p->state = draw_state_opaque(SC_1, CULL_NONE);
        p->colors = &a->show_color;    p->color_count = 1u;
        p->tex_reads = a->reads_show;  p->tex_read_count = 3u;
    }
    passes[count++] = demo_present_pass(d, a->hdr);
    d->program = demo_program(d, passes, count);
    /* The same solver passes and the projection, without the rest of the step. */
    count = solver_passes(a, passes);
    a->solve = frame_program_build(d->gpu, passes, count);
    if (d->headless && !d->frames) d->frames = 240u;
}

static void app_ui(App *a, struct nk_context *ctx) {
    Demo *d = &a->demo;
    if (nk_begin(ctx, "Fluid", nk_rect(10, 10, 300, 220),
                 NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_TITLE)) {
        demo_ui_text(ctx, "%.1f ms   %u x %u cells, %u pressure steps", (double)d->dt * 1000.0, N, N, a->iterations);
        demo_ui_choice(ctx, "View", k_views, 3u, &a->view);
        demo_ui_float(ctx, "Velocity kept per step", &a->velocity_kept, 0.95f, 1.0f);
        demo_ui_float(ctx, "Dye kept per step", &a->dye_kept, 0.95f, 1.0f);
    }
    nk_end(ctx);
}

/* ---- acceptance ------------------------------------------------------------ */

/* A grid as floats: the dye is half floats, everything else already is. */
static float *read_image(App *a, int which) {
    const size_t count = (size_t)N * N * 4u;
    float       *out;
    if (which == D0 || which == D1) return demo_read_floats(&a->demo, a->img[which], count);
    out = (float *)malloc(count * sizeof(float));
    demo_readback(&a->demo, a->img[which], out, count * sizeof(float));
    return out;
}

/* What flows out of cell (x, y): the shader's divergenceAt(). */
static double outflow(const float *u, uint32_t x, uint32_t y) {
    const float *here  = &u[((size_t)y * N + x) * 4u];
    const double left  = x == 0u ? 0.0 : here[0], below = y == 0u ? 0.0 : here[1];
    const double right = x == N - 1u ? 0.0 : u[((size_t)y * N + x + 1u) * 4u];
    const double above = y == N - 1u ? 0.0 : u[((size_t)(y + 1u) * N + x) * 4u + 1u];
    return right - left + above - below;
}

static void app_check(App *a) {
    Demo   *d = &a->demo;
    float  *before, *after, *pressure, *div, *dye;
    double  sum_before = 0.0, sum_after = 0.0, worst_before = 0.0, worst_after = 0.0, div_error = 0.0, div_peak = 0.0;
    double  project_error = 0.0, fastest = 0.0, dye_low = 1e9, dye_high = -1e9;
    size_t  wall = 0u, bad = 0u;
    uint32_t x, y;

    /* One more frame; everything it wrote is still there afterwards. */
    app_frame(a);
    before = read_image(a, U1); after = read_image(a, U0); pressure = read_image(a, P0);
    div = read_image(a, DIV);   dye = read_image(a, D0);

    for (y = 0u; y < N; ++y)
        for (x = 0u; x < N; ++x) {
            const size_t i = ((size_t)y * N + x) * 4u;
            const double b = outflow(before, x, y), f = outflow(after, x, y);
            const double p = pressure[i];
            const double ex = x == 0u ? 0.0 : (double)before[i] - (p - pressure[i - 4u]);
            const double ey = y == 0u ? 0.0 : (double)before[i + 1u] - (p - pressure[i - (size_t)N * 4u]);
            const double speed = sqrt((double)after[i] * after[i] + (double)after[i + 1u] * after[i + 1u]);
            uint32_t c;
            sum_before += fabs(b); sum_after += fabs(f);
            if (fabs(b) > worst_before) worst_before = fabs(b);
            if (fabs(f) > worst_after) worst_after = fabs(f);
            if (fabs(b - div[i]) > div_error) div_error = fabs(b - div[i]);
            if (fabs((double)div[i]) > div_peak) div_peak = fabs((double)div[i]);
            if (fabs(ex - after[i]) > project_error) project_error = fabs(ex - after[i]);
            if (fabs(ey - after[i + 1u]) > project_error) project_error = fabs(ey - after[i + 1u]);
            if (speed > fastest) fastest = speed;
            if ((x == 0u && after[i] != 0.0f) || (y == 0u && after[i + 1u] != 0.0f)) ++wall;
            for (c = 0u; c < 3u; ++c) {
                if (!(dye[i + c] == dye[i + c])) ++bad;
                if (dye[i + c] < dye_low) dye_low = dye[i + c];
                if (dye[i + c] > dye_high) dye_high = dye[i + c];
            }
            if (!(after[i] == after[i]) || !(after[i + 1u] == after[i + 1u])) ++bad;
        }
    demo_check(d, "divergence pass against the same sum on the CPU (largest difference / largest value)",
                     div_error / (div_peak > 0.0 ? div_peak : 1.0), "< 1e-5", div_error < 1e-5 * div_peak);
    demo_check(d, "projection against the same formula on the CPU (cells/s)", project_error, "< 1e-3",
                     project_error < 1e-3);
    demo_check(d, "mean outflow per cell before projection (cells/s, reported)", sum_before / ((double)N * N), "", true);
    demo_check(d, "mean outflow per cell after projection (cells/s, reported)", sum_after / ((double)N * N), "", true);
    demo_check(d, "mean outflow after / before", sum_after / sum_before, "< 0.5", sum_after < 0.5 * sum_before);
    demo_check(d, "largest outflow after / before", worst_after / worst_before, "< 0.25",
                     worst_after < 0.25 * worst_before);
    demo_check(d, "velocity through the left and bottom walls: cells not exactly 0", (double)wall, "== 0", wall == 0u);
    demo_check(d, "values that are not numbers", (double)bad, "== 0", bad == 0u);
    demo_check(d, "fastest cell (cells per step, reported)", fastest / 60.0, "", true);
    demo_check(d, "dye stays between 0 and 1: lowest", dye_low, ">= 0", dye_low >= 0.0);
    demo_check(d, "dye stays between 0 and 1: highest", dye_high, "<= 1", dye_high <= 1.0);
    /* Is what is left the solver stopping early, or the method being wrong?
     * Keep solving the same step: the outflow must go to nothing. */
    {
        float   *solved;
        double   sum_solved = 0.0;
        uint32_t round;
        for (round = 0u; round < 120u; ++round) {
            frame_begin(d->gpu);
            solve_pressure(a, a->solve);
            frame_end(d->gpu);
        }
        solved = read_image(a, U0);
        for (y = 0u; y < N; ++y)
            for (x = 0u; x < N; ++x) sum_solved += fabs(outflow(solved, x, y));
        demo_check(d, "mean outflow after 120 more rounds of the solver / before", sum_solved / sum_before,
                   a->jacobi ? "reported" : "< 0.05", a->jacobi || sum_solved < 0.05 * sum_before);
        free(solved);
    }
    free(before); free(after); free(pressure); free(div); free(dye);
}

int main(int argc, char **argv) {
    static App app;
    static const DemoOption options[] = {
        {"--view", .u32 = &app.view, .lo = 0, .hi = 2, .help = "0 dye, 1 speed, 2 pressure"},
        {"--iterations", .u32 = &app.iterations, .lo = 2, .hi = MAX_K, .help = "pressure passes per frame, even"},
        {"--jacobi", .flag = &app.jacobi, .help = "plain Jacobi steps instead of red-black over-relaxation"},
        {"--omega", .f32 = &app.omega, .lo = 0.01, .hi = 1.99, .help = "over-relaxation factor"},
        {0}};
    demo_defaults(&app.demo, "fluid", "Fluid - stable fluids on a staggered grid");
    app.demo.note = "headless runs 240 frames before the capture; --frames N changes that";
    app.iterations    = 40u;
    app.velocity_kept = 0.999f;
    app.dye_kept      = 0.997f;
    app.omega         = 1.8f;
    demo_open(&app.demo, options, argc, argv);
    app_init(&app);
    while (demo_running(&app.demo)) {
        if (app.demo.ui) app_ui(&app, app.demo.ui);
        app_frame(&app);
    }
    if (app.demo.check) app_check(&app);
    return demo_close(&app.demo);
}
