/* turing — reaction and diffusion: patterns from almost nothing.
 *
 *   Seed -> Step x K (two images take turns) -> Shade -> Present
 *
 * Two chemicals on a grid that wraps; see shaders/turing.slang for the
 * equations. K steps run per frame.
 *
 * --check compares a step with the same arithmetic on the CPU, the decay of
 * one Fourier mode under diffusion alone with the stencil's eigenvalue, and
 * measures the pattern that grows.
 */
#include "demo.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SHADERS   "demos/turing/shaders/"
#define N         256u
#define MAX_STEPS 56u   /* a frame program holds 64 passes */

typedef struct Push {
    uint32_t src, dst, n, flags;
    float    feed, kill, du, dv;
    float    dt;
    uint32_t seed, mode_x, mode_y;
    float    look[4];
} Push;
_Static_assert(sizeof(Push) == 64, "Push must match turing.slang");

typedef struct Preset { const char *name; float feed, kill; } Preset;
static const Preset k_presets[] = {{"coral", 0.0545f, 0.062f},   {"cells that divide", 0.0367f, 0.0649f},
                                   {"maze", 0.029f, 0.057f},     {"spots", 0.030f, 0.062f},
                                   {"spirals", 0.018f, 0.051f}};
#define PRESETS 5u
static const char *const k_preset_names[PRESETS] = {"coral", "cells that divide", "maze", "spots", "spirals"};
static const char *const k_views[] = {"lit", "v", "u"};

typedef struct App {
    Demo     demo;
    Target   state[2];
    uint32_t index[2];
    /* options */
    uint32_t preset, steps, view;
    float    feed, kill, tiles;
    bool     map;                /* F and k vary across the grid */
    /* what the next Seed pass writes; --check changes these */
    bool     reseed, diffusion_only;
    uint32_t seed, mode_x, mode_y;
} App;

static void app_frame(App *a) {
    Demo    *d = &a->demo;
    PassCtx *pass;
    uint32_t i;
    Push     push = {.n = N, .flags = (a->map ? 1u : 0u) | (a->diffusion_only ? 2u : 0u),
                     .feed = a->feed, .kill = a->kill, .du = a->map ? 0.2f : 1.0f, .dv = a->map ? 0.1f : 0.5f, .dt = 1.0f,   /* the map needs finer patterns */
                     .seed = a->seed, .mode_x = a->mode_x, .mode_y = a->mode_y,
                     .look = {(float)a->view, (float)d->width / (float)d->height, a->map ? 1.0f : a->tiles, 0.0f}};
    d->present.tonemap = DEMO_TONEMAP_NONE;

    if (!demo_frame_begin(d)) return;
    /* The pass is in the program every frame and works when a new start was asked for. */
    pass = demo_pass(d, "Seed");
    if (a->reseed) {
        push.dst = a->index[0];
        pass_push(pass, &push, sizeof push);
        pass_dispatch(pass, N / 8u, N / 8u, 1u);
        a->reseed = false;
    }
    /* Step i reads image i & 1 and writes the other; the count is even, so the newest state is image 0. */
    for (i = 0u; i < a->steps; ++i) {
        push.src = a->index[i & 1u];
        push.dst = a->index[(i + 1u) & 1u];
        pass = demo_pass(d, NULL);   /* "StepA" / "StepB" */
        pass_push(pass, &push, sizeof push);
        pass_dispatch(pass, N / 8u, N / 8u, 1u);
    }
    push.src = a->index[0];
    pass = demo_pass(d, "Shade");
    pass_push(pass, &push, sizeof push);
    pass_draw_fullscreen_triangle(pass, d->width, d->height);
    demo_present(d);
    demo_frame_end(d);
}

static void app_init(App *a) {
    Demo    *d = &a->demo;
    PassDesc passes[MAX_STEPS + 4u];
    uint32_t count = 0u, i;
    Target   hdr = demo_target(d, "turing_hdr", FMT_RGBA16F, d->width, d->height, false, NULL);
    Pipe     step;

    if (a->steps & 1u) demo_fatal(d, "--steps must be even");
    a->state[0] = demo_target(d, "turing_a", FMT_RGBA32F, N, N, true, &a->index[0]);
    a->state[1] = demo_target(d, "turing_b", FMT_RGBA32F, N, N, true, &a->index[1]);
    step = pipe_intern_compute(d->gpu, demo_shader(d, SHADERS "step.comp.spv", STAGE_COMPUTE), "main");

    passes[count++] = (PassDesc){
        .name = "Seed", .kind = PASS_COMPUTE, .state = draw_state_opaque(SC_1, CULL_NONE),
        .pipe = pipe_intern_compute(d->gpu, demo_shader(d, SHADERS "seed.comp.spv", STAGE_COMPUTE), "main"),
        .tex_writes = &a->state[0], .tex_write_count = 1u};
    for (i = 0u; i < a->steps; ++i)
        passes[count++] = (PassDesc){
            .name = "StepA", .kind = PASS_COMPUTE, .state = draw_state_opaque(SC_1, CULL_NONE), .pipe = step,
            .tex_reads = &a->state[i & 1u], .tex_read_count = 1u,
            .tex_writes = &a->state[(i + 1u) & 1u], .tex_write_count = 1u};
    for (i = 1u; i < a->steps; i += 2u) strcpy(passes[1u + i].name, "StepB");
    passes[count++] = (PassDesc){
        .name = "Shade", .kind = PASS_GRAPHICS, .state = draw_state_opaque(SC_1, CULL_NONE),
        .pipe = pipe_intern_graphics(d->gpu, d->fullscreen_vs, demo_shader(d, SHADERS "shade.frag.spv", STAGE_FRAGMENT),
                                     FMT_RGBA16F, FMT_NONE, BLEND_OPAQUE),
        .colors = (AttachDesc[]){attach_color(hdr, ATT_WRITE)}, .color_count = 1u,
        .tex_reads = &a->state[0], .tex_read_count = 1u};
    passes[count++] = demo_present_pass(d, hdr);
    d->program = demo_program(d, passes, count);

    if (!a->feed) { a->feed = k_presets[a->preset].feed; a->kill = k_presets[a->preset].kill; }
    a->reseed = true;
    if (d->headless && !d->frames) d->frames = 200u;
}

static void app_ui(App *a, struct nk_context *ctx) {
    Demo *d = &a->demo;
    if (nk_begin(ctx, "Reaction-diffusion", nk_rect(10, 10, 300, 300),
                 NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_TITLE)) {
        demo_ui_text(ctx, "%.1f ms   %u x %u cells, %u steps a frame", (double)d->dt * 1000.0, N, N, a->steps);
        if (demo_ui_choice(ctx, "Pattern", k_preset_names, PRESETS, &a->preset)) {
            a->feed = k_presets[a->preset].feed;
            a->kill = k_presets[a->preset].kill;
            a->reseed = true;
        }
        demo_ui_float(ctx, "Feed F", &a->feed, 0.006f, 0.086f);
        demo_ui_float(ctx, "Kill k", &a->kill, 0.040f, 0.070f);
        demo_ui_bool(ctx, "F up, k across (every pattern at once)", &a->map);
        demo_ui_bool(ctx, "Start again", &a->reseed);
        demo_ui_choice(ctx, "View", k_views, 3u, &a->view);
        demo_ui_float(ctx, "Tiles shown", &a->tiles, 0.5f, 4.0f);
    }
    nk_end(ctx);
}

/* ---- acceptance ------------------------------------------------------------ */

static void app_check(App *a) {
    static const double k_two_pi = 6.283185307179586;
    Demo        *d = &a->demo;
    const size_t cells = (size_t)N * N;
    float       *before = (float *)malloc(cells * 16u), *after = (float *)malloc(cells * 16u);
    uint32_t     x, y, f, c;
    size_t       i;

    /* 1. The pattern the default run grew. After a frame image 1 holds the
     * state one step before image 0, so one step can be redone on the CPU. */
    {
        double worst = 0.0, low = 1e9, high = -1e9, bad = 0.0, covered = 0.0, peak = 0.0;
        double *row_re = (double *)calloc(cells, sizeof(double)), *row_im = (double *)calloc(cells, sizeof(double));
        double  power[N / 2u] = {0};
        uint32_t best = 1u, kx, ky;
        demo_readback(d, a->state[1], before, cells * 16u);
        demo_readback(d, a->state[0], after, cells * 16u);
        for (y = 0u; y < N; ++y)
            for (x = 0u; x < N; ++x) {
                const uint32_t l = (x + N - 1u) % N, r = (x + 1u) % N, b = (y + N - 1u) % N, t = (y + 1u) % N;
                float next[2];
#define AT(X, Y, C) before[((size_t)(Y) * N + (X)) * 4u + (C)]
                const float su = AT(x, y, 0), sv = AT(x, y, 1), react = su * sv * sv;
                for (c = 0u; c < 2u; ++c) {
                    const float lap = 0.2f * (AT(r, y, c) + AT(l, y, c) + AT(x, t, c) + AT(x, b, c)) +
                                      0.05f * (AT(r, t, c) + AT(l, t, c) + AT(r, b, c) + AT(l, b, c)) - AT(x, y, c);
                    next[c] = c == 0u ? su + 1.0f * (1.0f * lap - react + a->feed * (1.0f - su))
                                      : sv + 1.0f * (0.5f * lap + react - (a->feed + a->kill) * sv);
                }
#undef AT
                for (c = 0u; c < 2u; ++c) {
                    const double got = after[((size_t)y * N + x) * 4u + c];
                    if (fabs(got - next[c]) > worst) worst = fabs(got - next[c]);
                    if (got < low) low = got;
                    if (got > high) high = got;
                    bad += !(got == got);
                }
                covered += after[((size_t)y * N + x) * 4u + 1u] > 0.1f;
            }
        demo_check(d, "one step against the same arithmetic on the CPU: largest difference", worst, "< 1e-6", worst < 1e-6);
        demo_check(d, "values that are not numbers", bad, "== 0", bad == 0.0);
        demo_check(d, "lowest amount", low, ">= 0", low >= 0.0);
        demo_check(d, "highest amount", high, "<= 1", high <= 1.0);
        demo_check(d, "part of the grid where v > 0.1 (it started at 0.03)", covered / (double)cells, "0.2 .. 0.8",
                   covered / (double)cells > 0.2 && covered / (double)cells < 0.8);
        /* The pattern's spacing: where the power of v's Fourier transform peaks, by radius. Rows, then columns. */
        for (y = 0u; y < N; ++y)
            for (kx = 0u; kx < N / 2u; ++kx)
                for (x = 0u; x < N; ++x) {
                    const double v = after[((size_t)y * N + x) * 4u + 1u], angle = k_two_pi * kx * x / N;
                    row_re[(size_t)y * N + kx] += v * cos(angle);
                    row_im[(size_t)y * N + kx] -= v * sin(angle);
                }
        for (ky = 0u; ky < N; ++ky)
            for (kx = 0u; kx < N / 2u; ++kx) {
                const double fy = ky < N / 2u ? (double)ky : (double)ky - N, radius = sqrt((double)kx * kx + fy * fy);
                double re = 0.0, im = 0.0;
                for (y = 0u; y < N; ++y) {
                    const double angle = k_two_pi * ky * y / N, cs = cos(angle), sn = sin(angle);
                    re += row_re[(size_t)y * N + kx] * cs + row_im[(size_t)y * N + kx] * sn;
                    im += row_im[(size_t)y * N + kx] * cs - row_re[(size_t)y * N + kx] * sn;
                }
                if (radius >= 1.0 && radius < N / 2u - 1u) power[(uint32_t)(radius + 0.5)] += re * re + im * im;
            }
        for (c = 2u; c < N / 2u; ++c)
            if (power[c] > peak) { peak = power[c]; best = c; }
        demo_check(d, "spacing of the pattern (cells)", (double)N / best, "6 .. 30", (double)N / best > 6.0 && (double)N / best < 30.0);
        free(row_re); free(row_im);
    }

    /* 2. Diffusion alone. A wave cos(2 pi (mx x + my y) / N) is an eigenvector
     * of the stencil, so each step multiplies it by
     *   1 + D (0.4 (cos tx + cos ty) + 0.2 cos tx cos ty - 1),   t = 2 pi m / N,
     * exactly, and the total amount cannot change. */
    a->diffusion_only = true;
    a->seed = 1u;
    for (f = 0u; f < 2u; ++f) {
        static const uint32_t k_mode[2][2] = {{3u, 0u}, {5u, 2u}};
        const double tx = k_two_pi * k_mode[f][0] / N, ty = k_two_pi * k_mode[f][1] / N;
        const double stencil = 0.4 * (cos(tx) + cos(ty)) + 0.2 * cos(tx) * cos(ty) - 1.0;
        const uint32_t frames = 12u, steps = frames * a->steps;
        double amplitude[2] = {0.0, 0.0}, total[2] = {0.0, 0.0};
        char   label[128];
        a->mode_x = k_mode[f][0];
        a->mode_y = k_mode[f][1];
        a->reseed = true;
        for (i = 0u; i < frames; ++i) app_frame(a);
        demo_readback(d, a->state[0], after, cells * 16u);
        for (y = 0u; y < N; ++y)
            for (x = 0u; x < N; ++x)
                for (c = 0u; c < 2u; ++c) {
                    const double value = after[((size_t)y * N + x) * 4u + c];
                    amplitude[c] += value * cos(k_two_pi * (k_mode[f][0] * x + k_mode[f][1] * y) / N) * 2.0 / (double)cells;
                    total[c] += value / (double)cells;
                }
        for (c = 0u; c < 2u; ++c) {
            const double expected = 0.25 * pow(1.0 + (c == 0u ? 1.0 : 0.5) * stencil, (double)steps);
            snprintf(label, sizeof label, "wave (%u, %u), %s after %u steps: amplitude / 0.25 lambda^steps - 1", k_mode[f][0],
                     k_mode[f][1], c == 0u ? "u (D = 1)" : "v (D = 0.5)", steps);
            demo_check(d, label, amplitude[c] / expected - 1.0, "|x| < 1e-4", fabs(amplitude[c] / expected - 1.0) < 1e-4);
            demo_check(d, "  left of the wave (reported)", amplitude[c] / 0.25, "", true);
            /* Single precision: every step rounds every cell to 6e-8. */
            demo_check(d, "  mean amount - 0.5", total[c] - 0.5, "|x| < 2e-5", fabs(total[c] - 0.5) < 2e-5);
        }
    }

    /* 3. u = 1, v = 0 is a state the full equations leave alone. */
    {
        double worst = 0.0;
        a->diffusion_only = false;
        a->seed   = 2u;
        a->reseed = true;
        for (i = 0u; i < 4u; ++i) app_frame(a);
        demo_readback(d, a->state[0], after, cells * 16u);
        for (i = 0u; i < cells; ++i) {
            if (fabs((double)after[i * 4u] - 1.0) > worst) worst = fabs((double)after[i * 4u] - 1.0);
            if (fabs((double)after[i * 4u + 1u]) > worst) worst = fabs((double)after[i * 4u + 1u]);
        }
        demo_check(d, "u = 1, v = 0 after 4 frames: largest change", worst, "< 1e-6", worst < 1e-6);
    }
    free(before); free(after);
}

int main(int argc, char **argv) {
    static App app;
    static const DemoOption options[] = {
        {"--preset", .u32 = &app.preset, .lo = 0, .hi = PRESETS - 1, .help = "0 coral, 1 cells that divide, 2 maze, 3 spots, 4 spirals"},
        {"--feed", .f32 = &app.feed, .lo = 0.001, .hi = 0.1, .help = "F, instead of the preset's"},
        {"--kill", .f32 = &app.kill, .lo = 0.03, .hi = 0.08, .help = "k, instead of the preset's (give --feed too)"},
        {"--map", .flag = &app.map, .help = "k across, F up: every pattern at once"},
        {"--steps", .u32 = &app.steps, .lo = 2, .hi = MAX_STEPS, .help = "steps per frame (even)"},
        {"--tiles", .f32 = &app.tiles, .lo = 0.5, .hi = 4, .help = "copies of the grid across the picture's height"},
        {"--view", .u32 = &app.view, .lo = 0, .hi = 2, .help = "0 lit, 1 v, 2 u"},
        {0}};
    demo_defaults(&app.demo, "turing", "Reaction-diffusion - Gray-Scott");
    app.steps = 32u;
    app.tiles = 1.5f;
    app.demo.note = "Headless: 200 frames unless --frames says otherwise (the pattern needs a few thousand steps).";
    demo_open(&app.demo, options, argc, argv);
    app_init(&app);
    while (demo_running(&app.demo)) {
        if (app.demo.ui) app_ui(&app, app.demo.ui);
        app_frame(&app);
    }
    if (app.demo.check) app_check(&app);
    return demo_close(&app.demo);
}
