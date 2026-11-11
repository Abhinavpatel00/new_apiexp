/* schrodinger — one quantum particle and two slits.
 *
 *   Seed -> Half -> (StepR -> StepI) x K -> Shade -> Present
 *
 * The wave function of a particle on a grid, as two images: its real part
 * and its imaginary part half a time step later. See
 * shaders/schrodinger.slang for the equation and the scheme. A wave packet
 * is thrown at a wall with two slits; a plate behind the wall adds up what
 * arrives.
 *
 * --check measures the bright bands on the plate against the path
 * difference from the two slits, closes one slit, and follows a free packet:
 * nothing is lost, it moves at the group velocity and it spreads as the
 * uncertainty principle says.
 */
#include "demo.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SHADERS    "demos/schrodinger/shaders/"
#define NX         384u
#define NY         216u
#define MAX_PAIRS  28u          /* a frame program holds 64 passes */
#define WALL_X     170u
#define WALL_THICK 6.0f
#define SCREEN_X   336u
#define SLIT_SEP   48.0f
#define SLIT_WIDTH 8.0f
#define TIME_STEP  0.2f

typedef struct Push {
    uint32_t real, imag, part, slits;
    uint32_t nx, ny, wall_x, screen_x;
    float    dt, k_x, k_y, wall_v;
    float    start_x, start_y, sigma_x, sigma_y;
    float    slit_sep, slit_width, absorb, view;
    float    gain, plate_gain, aspect, wall_thick;
} Push;
_Static_assert(sizeof(Push) == 96, "Push must match schrodinger.slang");

static const char *const k_views[] = {"phase as colour", "how likely", "real part"};
static const char *const k_slits[] = {"no wall", "one slit", "two slits"};

typedef struct App {
    Demo     demo;
    Target   state[2];           /* real part, imaginary part */
    uint32_t index[2];
    /* options */
    uint32_t slits, pairs, view;
    float    momentum;
    bool     hard_walls;         /* no absorbing border */
    /* the packet the next Seed pass makes; --check changes these */
    bool     reseed;
    float    start_x, sigma_x, sigma_y;
    uint32_t run;                /* step pairs this frame runs, at most `pairs` */
} App;

static void app_frame(App *a) {
    Demo    *d = &a->demo;
    PassCtx *pass;
    uint32_t i;
    Push     push = {.real = a->index[0], .imag = a->index[1], .slits = a->slits,
                     .nx = NX, .ny = NY, .wall_x = WALL_X, .screen_x = SCREEN_X,
                     .dt = TIME_STEP, .k_x = a->momentum, .wall_v = 2.0f,
                     .start_x = a->start_x, .start_y = 0.5f * NY, .sigma_x = a->sigma_x, .sigma_y = a->sigma_y,
                     .slit_sep = SLIT_SEP, .slit_width = SLIT_WIDTH, .absorb = a->hard_walls ? 0.0f : 0.4f,
                     .view = (float)a->view, .gain = 12.0f, .plate_gain = 0.13f,
                     .aspect = (float)d->width / (float)d->height, .wall_thick = WALL_THICK};
    d->present.tonemap = DEMO_TONEMAP_NONE;

    if (!demo_frame_begin(d)) return;
    /* Both passes are in the program every frame and work when a new start was asked for. */
    pass = demo_pass(d, "Seed");
    if (a->reseed) {
        pass_push(pass, &push, sizeof push);
        pass_dispatch(pass, NX / 8u, NY / 8u, 1u);
    }
    /* The imaginary part belongs half a step after the real part. */
    pass = demo_pass(d, "Half");
    if (a->reseed) {
        push.part = 1u;
        push.dt   = 0.5f * TIME_STEP;
        pass_push(pass, &push, sizeof push);
        pass_dispatch(pass, NX / 8u, NY / 8u, 1u);
        push.dt   = TIME_STEP;
        a->reseed = false;
    }
    for (i = 0u; i < 2u * a->pairs; ++i) {
        pass = demo_pass(d, NULL);   /* "StepR" / "StepI" */
        if (i / 2u >= a->run) continue;
        push.part = i & 1u;
        pass_push(pass, &push, sizeof push);
        pass_dispatch(pass, NX / 8u, NY / 8u, 1u);
    }
    pass = demo_pass(d, "Shade");
    pass_push(pass, &push, sizeof push);
    pass_draw_fullscreen_triangle(pass, d->width, d->height);
    demo_present(d);
    demo_frame_end(d);
}

static void app_init(App *a) {
    Demo    *d = &a->demo;
    PassDesc passes[2u * MAX_PAIRS + 5u];
    uint32_t count = 0u, i;
    Target   hdr = demo_target(d, "schrodinger_hdr", FMT_RGBA16F, d->width, d->height, false, NULL);
    Pipe     step;

    a->state[0] = demo_target(d, "schrodinger_real", FMT_RGBA32F, NX, NY, true, &a->index[0]);
    a->state[1] = demo_target(d, "schrodinger_imag", FMT_RGBA32F, NX, NY, true, &a->index[1]);
    step = pipe_intern_compute(d->gpu, demo_shader(d, SHADERS "step.comp.spv", STAGE_COMPUTE), "main");

    passes[count++] = (PassDesc){
        .name = "Seed", .kind = PASS_COMPUTE, .state = draw_state_opaque(SC_1, CULL_NONE),
        .pipe = pipe_intern_compute(d->gpu, demo_shader(d, SHADERS "seed.comp.spv", STAGE_COMPUTE), "main"),
        .tex_writes = a->state, .tex_write_count = 2u};
    passes[count++] = (PassDesc){
        .name = "Half", .kind = PASS_COMPUTE, .state = draw_state_opaque(SC_1, CULL_NONE), .pipe = step,
        .tex_reads = &a->state[0], .tex_read_count = 1u, .tex_writes = &a->state[1], .tex_write_count = 1u};
    for (i = 0u; i < 2u * a->pairs; ++i) {
        passes[count] = (PassDesc){
            .kind = PASS_COMPUTE, .state = draw_state_opaque(SC_1, CULL_NONE), .pipe = step,
            .tex_reads = &a->state[(i + 1u) & 1u], .tex_read_count = 1u,
            .tex_writes = &a->state[i & 1u], .tex_write_count = 1u};
        strcpy(passes[count++].name, (i & 1u) ? "StepI" : "StepR");
    }
    passes[count++] = (PassDesc){
        .name = "Shade", .kind = PASS_GRAPHICS, .state = draw_state_opaque(SC_1, CULL_NONE),
        .pipe = pipe_intern_graphics(d->gpu, d->fullscreen_vs, demo_shader(d, SHADERS "shade.frag.spv", STAGE_FRAGMENT),
                                     FMT_RGBA16F, FMT_NONE, BLEND_OPAQUE),
        .colors = (AttachDesc[]){attach_color(hdr, ATT_WRITE)}, .color_count = 1u,
        .tex_reads = a->state, .tex_read_count = 2u};
    passes[count++] = demo_present_pass(d, hdr);
    d->program = demo_program(d, passes, count);

    a->reseed = true;
    a->run    = a->pairs;
    /* 64 frames catch the packet behind the wall; the plate needs 110 to collect all of it. */
    if (d->headless && !d->frames) d->frames = d->check ? 110u : 64u;
}

static void app_ui(App *a, struct nk_context *ctx) {
    Demo *d = &a->demo;
    if (nk_begin(ctx, "Schrodinger", nk_rect(10, 10, 300, 260),
                 NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_TITLE)) {
        demo_ui_text(ctx, "%.1f ms   %u x %u cells, %u steps a frame", (double)d->dt * 1000.0, NX, NY, a->pairs);
        if (demo_ui_choice(ctx, "Wall", k_slits, 3u, &a->slits)) a->reseed = true;
        demo_ui_float(ctx, "Momentum (next throw)", &a->momentum, 0.3f, 1.0f);
        demo_ui_bool(ctx, "Throw again", &a->reseed);
        demo_ui_bool(ctx, "Hard walls round the grid", &a->hard_walls);
        demo_ui_choice(ctx, "View", k_views, 3u, &a->view);
    }
    nk_end(ctx);
}

/* ---- acceptance ------------------------------------------------------------ */

static void app_check(App *a) {
    Demo        *d = &a->demo;
    const size_t cells = (size_t)NX * NY, bytes = cells * 16u;
    float       *real = (float *)malloc(bytes), *imag = (float *)malloc(bytes), *next = (float *)malloc(bytes);
    const uint32_t full = d->frames;   /* the frames the run before this took */
    uint32_t     y, pass, f, e, axis;
    size_t       i;
    int          j;

    /* 1. The plate, with two slits (the run that just ended) and then with one.
     * A point is bright where its distances to the two slits differ by a whole
     * number n of wavelengths: a hyperbola, which meets the plate at
     * y = a sqrt(1 + L^2 / b^2), a = n lambda / 2, b^2 = (sep / 2)^2 - a^2. */
    for (pass = 0u; pass < 2u; ++pass) {
        const double lambda = 6.283185307179586 / a->momentum;
        const double length = (double)SCREEN_X + 0.5 - ((double)WALL_X + 0.5 * WALL_THICK);
        const double half = 0.5 * lambda, across = sqrt(0.25 * SLIT_SEP * SLIT_SEP - half * half);
        const double expected = half * sqrt(1.0 + length * length / (across * across));
        double   plate[NY], smooth[NY] = {0}, peak = 0.0, where[16] = {0}, height[16] = {0}, lowest = 1e30;
        uint32_t maxima = 0u, centre = 0u, dark = 0u, brightest = 0u;
        if (pass == 1u) {
            a->slits  = 1u;
            a->reseed = true;
            for (f = 0u; f < full; ++f) app_frame(a);
        }
        demo_readback(d, a->state[0], real, bytes);
        for (y = 0u; y < NY; ++y) plate[y] = real[((size_t)y * NX + SCREEN_X) * 4u + 1u];
        for (y = 2u; y < NY - 2u; ++y) {
            smooth[y] = (plate[y - 2u] + plate[y - 1u] + plate[y] + plate[y + 1u] + plate[y + 2u]) / 5.0;
            if (smooth[y] > peak) { peak = smooth[y]; brightest = y; }
        }
        /* Rows within 70 cells of the axis that collected under a tenth of the brightest row. */
        for (y = NY / 2u - 70u; y < NY / 2u + 70u; ++y)
            if (smooth[y] < 0.1 * peak) ++dark;
        /* Bright bands: maxima above a twentieth of the brightest, placed by a parabola through three rows. */
        for (y = 26u; y < NY - 26u; ++y)
            if (smooth[y] > smooth[y - 1u] && smooth[y] >= smooth[y + 1u] && smooth[y] > 0.05 * peak && maxima < 16u) {
                const double bend = smooth[y - 1u] - 2.0 * smooth[y] + smooth[y + 1u];
                where[maxima]  = (double)y + 0.5 - 0.5 * NY + 0.5 * (smooth[y - 1u] - smooth[y + 1u]) / bend;
                height[maxima] = smooth[y];
                if (fabs(where[maxima]) < fabs(where[centre])) centre = maxima;
                ++maxima;
            }
        if (pass == 0u) {
            const bool sides = centre >= 1u && centre + 1u < maxima;
            const double first = sides ? 0.5 * (where[centre + 1u] - where[centre - 1u]) : 0.0;
            demo_check(d, "two slits: bright bands on the plate", maxima, ">= 5", maxima >= 5u);
            demo_check(d, "  the middle band, cells from the axis", where[centre], "|x| < 0.5", fabs(where[centre]) < 0.5);
            demo_check(d, "  the bands next to it, cells from the axis", first, "", true);
            demo_check(d, "    / where one wavelength of path difference puts them - 1", first / expected - 1.0, "|x| < 0.05",
                       sides && fabs(first / expected - 1.0) < 0.05);
            /* The dark band between the middle and its neighbour. */
            for (y = 0u; sides && y < NY; ++y) {
                const double at = (double)y + 0.5 - 0.5 * NY;
                if (at > where[centre] && at < where[centre + 1u] && smooth[y] < lowest) lowest = smooth[y];
            }
            demo_check(d, "  dark band between them / the middle band", lowest / height[centre], "< 0.1",
                       sides && lowest / height[centre] < 0.1);
            demo_check(d, "  dark rows within 70 cells of the axis", dark, ">= 10", dark >= 10u);
            demo_check(d, "  most the plate collected (reported)", peak, "", true);
        } else {
            const double at = 0.5 * NY - ((double)brightest + 0.5);
            demo_check(d, "one slit: dark rows within 70 cells of the axis", dark, "== 0", dark == 0u);
            demo_check(d, "  brightest row, cells above the axis (the slit is at 24)", at, "15 .. 40", at > 15.0 && at < 40.0);
        }
    }

    /* 2. A free packet between hard walls, measured at the start and 1201 steps later.
     *    - nothing lost: Visscher's sum of R(t) R(t + dt) + I(t + dt/2)^2
     *    - mean position moves at the mean group velocity
     *    - variance grows by t^2 times the variance of the group velocity
     * The group velocity of this grid and time step, for wave number k along one
     * axis: w = 5/4 - 4/3 cos k + 1/12 cos 2k,
     *       v = (4/3 sin k - 1/6 sin 2k) / sqrt(1 - (w dt / 2)^2).
     * The packet holds wave numbers spread as a Gaussian of width 1 / (2 sigma). */
    {
        const double sigma = 8.0, spread = 0.5 / sigma, elapsed = (1.0 + 60.0 * a->pairs) * TIME_STEP;
        double kept[2], mean[2][2], variance[2][2], speed[2], speed_variance[2];
        a->slits      = 0u;
        a->hard_walls = true;
        a->start_x    = 90.0f;
        a->sigma_x = a->sigma_y = (float)sigma;
        a->reseed     = true;
        a->run        = 0u;
        app_frame(a);
        for (e = 0u; e < 2u; ++e) {
            double total = 0.0, sum[2] = {0.0, 0.0}, square[2] = {0.0, 0.0};
            demo_readback(d, a->state[0], real, bytes);
            demo_readback(d, a->state[1], imag, bytes);
            a->run = 1u;
            app_frame(a);
            demo_readback(d, a->state[0], next, bytes);
            kept[e] = 0.0;
            for (i = 0u; i < cells; ++i) {
                const double re = real[i * 4u], im = imag[i * 4u], density = re * re + im * im;
                const double at[2] = {(double)(i % NX), (double)(i / NX)};
                kept[e] += re * (double)next[i * 4u] + im * im;
                total += density;
                for (axis = 0u; axis < 2u; ++axis) { sum[axis] += density * at[axis]; square[axis] += density * at[axis] * at[axis]; }
            }
            for (axis = 0u; axis < 2u; ++axis) {
                mean[e][axis]     = sum[axis] / total;
                variance[e][axis] = square[axis] / total - mean[e][axis] * mean[e][axis];
            }
            a->run = a->pairs;
            for (f = 0u; e == 0u && f < 60u; ++f) app_frame(a);
        }
        for (axis = 0u; axis < 2u; ++axis) {
            double weight = 0.0, first = 0.0, second = 0.0;
            for (j = -300; j <= 300; ++j) {
                const double k = (axis == 0u ? a->momentum : 0.0) + spread * j / 50.0, g = exp(-0.5 * (j / 50.0) * (j / 50.0));
                const double w = 1.25 - 4.0 / 3.0 * cos(k) + cos(2.0 * k) / 12.0;
                const double v = (4.0 / 3.0 * sin(k) - sin(2.0 * k) / 6.0) / sqrt(1.0 - 0.25 * w * w * TIME_STEP * TIME_STEP);
                weight += g; first += g * v; second += g * v * v;
            }
            speed[axis]          = first / weight;
            speed_variance[axis] = second / weight - speed[axis] * speed[axis];
        }
        {
            const double measured = (mean[1][0] - mean[0][0]) / elapsed;
            const double grown_x = (variance[1][0] - variance[0][0]) / (elapsed * elapsed * speed_variance[0]);
            const double grown_y = (variance[1][1] - variance[0][1]) / (elapsed * elapsed * speed_variance[1]);
            demo_check(d, "free packet, 1201 steps: what is kept / at the start - 1", kept[1] / kept[0] - 1.0, "|x| < 1e-5",
                       fabs(kept[1] / kept[0] - 1.0) < 1e-5);
            demo_check(d, "  width at the start, sqrt(variance) (asked for 8)", sqrt(variance[0][0]), "7.99 .. 8.01",
                       fabs(sqrt(variance[0][0]) - 8.0) < 0.01);
            demo_check(d, "  speed / group velocity of the grid - 1", measured / speed[0] - 1.0, "|x| < 1e-3",
                       fabs(measured / speed[0] - 1.0) < 1e-3);
            demo_check(d, "  speed / momentum - 1 (what the grid costs, reported)", measured / a->momentum - 1.0, "", true);
            demo_check(d, "  sideways drift (cells)", mean[1][1] - mean[0][1], "|x| < 0.01", fabs(mean[1][1] - mean[0][1]) < 0.01);
            demo_check(d, "  width at the end, along / across (cells, reported)", sqrt(variance[1][0]), "", true);
            demo_check(d, "  ", sqrt(variance[1][1]), "", true);
            demo_check(d, "  growth of the variance along the motion / t^2 var(v) - 1", grown_x - 1.0, "|x| < 0.01", fabs(grown_x - 1.0) < 0.01);
            demo_check(d, "  growth of the variance across / t^2 var(v) - 1", grown_y - 1.0, "|x| < 0.01", fabs(grown_y - 1.0) < 0.01);
            demo_check(d, "  width across / sqrt(8^2 + (t / 16)^2) - 1 (no grid, reported)",
                       sqrt(variance[1][1]) / sqrt(64.0 + elapsed * elapsed / 256.0) - 1.0, "", true);
        }
    }
    free(real); free(imag); free(next);
}

int main(int argc, char **argv) {
    static App app;
    static const DemoOption options[] = {
        {"--slits", .u32 = &app.slits, .lo = 0, .hi = 2, .help = "0 no wall, 1 one slit, 2 two slits"},
        {"--momentum", .f32 = &app.momentum, .lo = 0.3, .hi = 1.0, .help = "wave number of the packet (wavelength = 2 pi / this, in cells)"},
        {"--steps", .u32 = &app.pairs, .lo = 1, .hi = MAX_PAIRS, .help = "time steps per frame"},
        {"--hard-walls", .flag = &app.hard_walls, .help = "the edge of the grid reflects instead of absorbing"},
        {"--view", .u32 = &app.view, .lo = 0, .hi = 2, .help = "0 phase as colour, 1 how likely, 2 real part"},
        {0}};
    demo_defaults(&app.demo, "schrodinger", "Schrodinger - two slits");
    app.slits    = 2u;
    app.pairs    = 20u;
    app.momentum = 0.7f;
    app.start_x  = 80.0f;
    app.sigma_x  = 14.0f;
    app.sigma_y  = 36.0f;
    app.demo.note = "Headless: 64 frames unless --frames says otherwise (the packet is then behind the wall).";
    demo_open(&app.demo, options, argc, argv);
    app_init(&app);
    while (demo_running(&app.demo)) {
        if (app.demo.ui) app_ui(&app, app.demo.ui);
        app_frame(&app);
    }
    if (app.demo.check) app_check(&app);
    return demo_close(&app.demo);
}
