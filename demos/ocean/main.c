/* ocean — a patch of ocean from its wave spectrum, by an inverse FFT in compute.
 *
 *   Spectrum (compute) -> 2 * log2(N) butterfly passes (compute) -> Ocean (mesh) -> Present
 *
 * See shaders/ocean.slang.
 */
#include "demo.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SHADERS  "demos/ocean/shaders/"
#define MAX_LOG  9u          /* 512 x 512 */
#define GRAVITY  9.81

typedef struct Push {
    float    vp[16];
    float    eye[4], sun[4];
    uint32_t h0, src, dst, stage;
    uint32_t n, log_n, vertical, tiles;
    float    time, patch, chop, pad0;
    uint32_t view, pad[3];
} Push;
_Static_assert(sizeof(Push) == 160, "Push must match ocean.slang");

typedef struct App {
    Demo       demo; /* first: see demo.h */
    DemoOrbit  orbit;
    BufSlice   h0;
    Target     spectrum, field[2], hdr, depth;
    uint32_t   spectrum_index, field_index[2];
    Pipe       spectrum_pipe, fft_pipe, ocean_pipe;
    BufUse     h0_read;
    AttachDesc color, depth_attach;
    Push       push;
    float     *h0_cpu;                 /* n * n * 4: h0(k) and h0(-k), as uploaded */
    /* settings */
    uint32_t   n, log_n, tiles, view;
    float      patch, chop, wind, height;   /* metres, -, m/s, rms metres */
    bool       single;                      /* --check: one wave only */
    bool       manual;
    float      manual_time;
} App;

/* ---- the spectrum ------------------------------------------------------------- */

static uint32_t pcg(uint32_t v) {
    uint32_t s = v * 747796405u + 2891336453u;
    uint32_t w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
    return (w >> 22u) ^ w;
}
static double uniform(uint32_t i) { return ((double)pcg(i) + 0.5) / 4294967296.0; }

/* Phillips: waves much longer than the wind can raise are cut off, waves across
 * the wind are weak, waves against it weaker still, and the shortest are damped. */
static double phillips(const App *a, double kx, double kz) {
    const double k2 = kx * kx + kz * kz, reach = (double)a->wind * a->wind / GRAVITY;
    const double wx = 0.9578, wz = 0.2873, cell = 0.35 * (double)a->patch / a->n;
    double       along;
    if (k2 <= 0.0) return 0.0;
    along = (kx * wx + kz * wz) / sqrt(k2);
    return exp(-1.0 / (k2 * reach * reach)) / (k2 * k2) * along * along * (along < 0.0 ? 0.07 : 1.0) * exp(-k2 * cell * cell);
}

static int wave_number(const App *a, uint32_t index) { return index < a->n / 2u ? (int)index : (int)index - (int)a->n; }

/* h0(k) for every k, then each texel gets h0(k) and h0(-k). Scaled so that the
 * height's standard deviation over time is a->height. */
static void make_spectrum(App *a) {
    const uint32_t n = a->n;
    float   *own = (float *)calloc((size_t)n * n * 2u, sizeof(float));
    double   energy = 0.0, scale;
    uint32_t x, y;
    memset(a->h0_cpu, 0, (size_t)n * n * 4u * sizeof(float));
    if (!own) return;
    if (a->single) {
        own[(3u + 2u * n) * 2u] = 0.25f; own[(3u + 2u * n) * 2u + 1u] = 0.10f;     /* wave number (3, 2) */
    } else {
        for (y = 0u; y < n; ++y)
            for (x = 0u; x < n; ++x) {
                const double kx = 6.283185307179586 * wave_number(a, x) / a->patch, kz = 6.283185307179586 * wave_number(a, y) / a->patch;
                const double amplitude = sqrt(0.5 * phillips(a, kx, kz));
                const double r = sqrt(-2.0 * log(uniform((x + y * n) * 2u))), t = 6.283185307179586 * uniform((x + y * n) * 2u + 1u);
                own[(x + y * n) * 2u]      = (float)(amplitude * r * cos(t));
                own[(x + y * n) * 2u + 1u] = (float)(amplitude * r * sin(t));
                energy += amplitude * amplitude * r * r;
            }
        scale = energy > 0.0 ? (double)a->height / sqrt(2.0 * energy) : 0.0;
        for (x = 0u; x < n * n * 2u; ++x) own[x] = (float)(own[x] * scale);
    }
    for (y = 0u; y < n; ++y)
        for (x = 0u; x < n; ++x) {
            const uint32_t mirror = ((n - x) % n) + ((n - y) % n) * n;
            float         *o      = a->h0_cpu + (size_t)(x + y * n) * 4u;
            o[0] = own[(x + y * n) * 2u]; o[1] = own[(x + y * n) * 2u + 1u];
            o[2] = own[mirror * 2u];      o[3] = own[mirror * 2u + 1u];
        }
    free(own);
}

/* ---- frame ------------------------------------------------------------------ */

/* Pass k of 2 * log_n: first all stages along x, then along y. Pass 0 reads the
 * spectrum; pass k writes field[k % 2], so the last one always writes field[1]. */
static void fft(App *a, PassCtx *p, uint32_t k) {
    Push push = a->push;
    push.src      = k == 0u ? a->spectrum_index : a->field_index[(k - 1u) % 2u];
    push.dst      = a->field_index[k % 2u];
    push.vertical = k >= a->log_n ? 1u : 0u;
    push.stage    = k % a->log_n;
    pass_push(p, &push, sizeof push);
    pass_dispatch(p, a->n / 8u, a->n / 8u, 1u);
}

/* One frame: what changed since the last one, then every pass, in the order app_init listed them. */
static void app_frame(App *a) {
    Demo    *d = &a->demo;
    Push    *p = &a->push;
    PassCtx *pass;
    float    eye[3];
    uint32_t k;
    if (!d->headless) demo_orbit_input(d, &a->orbit);
    demo_orbit_camera(&a->orbit, (float)d->width / (float)d->height, NULL, NULL, p->vp, eye);
    p->eye[0] = eye[0]; p->eye[1] = eye[1]; p->eye[2] = eye[2];
    p->sun[0] = -0.60f; p->sun[1] = 0.22f; p->sun[2] = -0.77f;
    p->h0 = (uint32_t)a->h0.offset;
    p->n = a->n; p->log_n = a->log_n; p->tiles = a->tiles;
    p->time = a->manual ? a->manual_time : d->time;
    p->patch = a->patch; p->chop = a->chop;
    p->view = a->view;
    d->present.tonemap = a->view ? DEMO_TONEMAP_NONE : DEMO_TONEMAP_ACES_FITTED;

    if (!demo_frame_begin(d)) return;
    pass = demo_pass(d, "Spectrum");
    {
        Push push = a->push;
        push.dst = a->spectrum_index;
        pass_push(pass, &push, sizeof push);
        pass_dispatch(pass, a->n / 8u, a->n / 8u, 1u);
    }
    for (k = 0u; k < 2u * a->log_n; ++k) fft(a, demo_pass(d, NULL), k);   /* "FFT x0" .. "FFT y<log_n - 1>" */
    pass = demo_pass(d, "Ocean");
    {
        Push           push = a->push;
        const uint32_t side = a->n * a->tiles;
        push.src = a->field_index[1];
        pass_push(pass, &push, sizeof push);
        pass_draw(pass, side * side * 6u, 1u, 0u, 0u);
    }
    demo_present(d);
    demo_frame_end(d);
}

static void app_init(App *a) {
    Demo     *d = &a->demo;
    PassDesc  passes[2u * MAX_LOG + 5u];
    PassDesc *p;
    ShaderId  spectrum_cs, fft_cs, vs, fs;
    uint32_t  count = 0u, k;
    const uint32_t n = a->n;

    for (a->log_n = 0u; (1u << a->log_n) < n; ++a->log_n) {}
    if (n & (n - 1u)) demo_fatal(d, "--grid must be a power of two");
    a->h0_cpu = (float *)malloc((size_t)n * n * 4u * sizeof(float));
    make_spectrum(a);
    a->h0 = demo_alloc(d, a->h0_cpu, (uint64_t)n * n * 16u);
    a->h0_read = use_read(a->h0.buffer, a->h0.offset, a->h0.size);
    a->h0_read.stages = SH_COMP;

    a->spectrum = demo_target(d, "ocean_spectrum", FMT_RGBA32F, n, n, true, &a->spectrum_index);
    a->field[0] = demo_target(d, "ocean_field0", FMT_RGBA32F, n, n, true, &a->field_index[0]);
    a->field[1] = demo_target(d, "ocean_field1", FMT_RGBA32F, n, n, true, &a->field_index[1]);
    a->hdr      = demo_target(d, "ocean_hdr", FMT_RGBA16F, d->width, d->height, false, NULL);
    a->depth    = demo_target(d, "ocean_depth", FMT_D32F, d->width, d->height, false, NULL);

    spectrum_cs = demo_shader(d, SHADERS "spectrum.comp.spv", STAGE_COMPUTE);
    fft_cs      = demo_shader(d, SHADERS "fft.comp.spv", STAGE_COMPUTE);
    vs          = demo_shader(d, SHADERS "ocean.vert.spv", STAGE_VERTEX);
    fs          = demo_shader(d, SHADERS "ocean.frag.spv", STAGE_FRAGMENT);
    a->spectrum_pipe = pipe_intern_compute(d->gpu, spectrum_cs, "main");
    a->fft_pipe      = pipe_intern_compute(d->gpu, fft_cs, "main");
    a->ocean_pipe    = pipe_intern_graphics(d->gpu, vs, fs, FMT_RGBA16F, FMT_D32F, BLEND_OPAQUE);

    passes[count++] = (PassDesc){
        .name = "Spectrum", .kind = PASS_COMPUTE, .pipe = a->spectrum_pipe, .state = draw_state_opaque(SC_1, CULL_NONE),
        .buf_reads = &a->h0_read, .buf_read_count = 1u,
        .tex_writes = &a->spectrum, .tex_write_count = 1u};
    for (k = 0u; k < 2u * a->log_n; ++k) {
        p = &passes[count++];
        memset(p, 0, sizeof *p);
        snprintf(p->name, sizeof p->name, "FFT %c%u", k < a->log_n ? 'x' : 'y', k % a->log_n);
        p->kind = PASS_COMPUTE; p->pipe = a->fft_pipe; p->state = draw_state_opaque(SC_1, CULL_NONE);
        p->tex_reads = k == 0u ? &a->spectrum : &a->field[(k - 1u) % 2u]; p->tex_read_count = 1u;
        p->tex_writes = &a->field[k % 2u]; p->tex_write_count = 1u;
    }
    a->color        = attach_clear(a->hdr, 0.62f, 0.70f, 0.78f, 0.0f);
    a->depth_attach = attach_depth_clear(a->depth, 1.0f);
    passes[count++] = (PassDesc){
        .name = "Ocean", .kind = PASS_GRAPHICS,
        .pipe = a->ocean_pipe,
        .state = draw_state_opaque(SC_1, CULL_NONE),
        .colors = &a->color, .color_count = 1u,
        .depth = &a->depth_attach,
        .tex_reads = &a->field[1], .tex_read_count = 1u};
    passes[count++] = demo_present_pass(d, a->hdr);
    d->program = demo_program(d, passes, count);
}

static void app_shutdown(App *a) { free(a->h0_cpu); }

static void app_ui(App *a, struct nk_context *ctx) {
    static const char *const k_views[3] = {"lit", "height", "folding"};
    Demo *d = &a->demo;
    if (nk_begin(ctx, "Ocean", nk_rect(10, 10, 320, 200),
                 NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_TITLE)) {
        demo_ui_text(ctx, "%.1f ms   %u x %u, %u passes", (double)d->dt * 1000.0, a->n, a->n, 2u * a->log_n + 1u);
        demo_ui_choice(ctx, "View", k_views, 3u, &a->view);
        demo_ui_float(ctx, "Choppiness", &a->chop, 0.0f, 2.0f);
    }
    nk_end(ctx);
}

#include "check.inc"

int main(int argc, char **argv) {
    static App app;
    static const DemoOption options[] = {
        {"--grid", .u32 = &app.n, .lo = 16, .hi = 1u << MAX_LOG, .help = "samples a side, a power of two"},
        {"--patch", .f32 = &app.patch, .lo = 0, .hi = 100000, .help = "size of the repeating patch in metres"},
        {"--tiles", .u32 = &app.tiles, .lo = 1, .hi = 8, .help = "patches drawn a side"},
        {"--wind", .f32 = &app.wind, .lo = 0, .hi = 1000, .help = "wind speed, m/s"},
        {"--height", .f32 = &app.height, .lo = 0, .hi = 1000, .help = "standard deviation of the height, metres"},
        {"--chop", .f32 = &app.chop, .lo = 0, .hi = 1000, .help = "horizontal displacement, 0 = none"},
        {"--view", .u32 = &app.view, .lo = 0, .hi = 2, .help = "0 lit, 1 height, 2 folding"},
        {0}};
    demo_defaults(&app.demo, "ocean", "Ocean - a wave spectrum through an inverse FFT in compute");
    app.orbit = (DemoOrbit){.target = {0.0f, 0.0f, 0.0f}, .distance = 30.0f, .yaw = 0.9f, .pitch = 0.2f,
                            .fov_deg = 55.0f, .znear = 0.2f, .zfar = 600.0f, .min_distance = 4.0f, .max_distance = 200.0f};
    app.n = 128u; app.tiles = 4u;
    app.patch = 64.0f; app.chop = 1.0f; app.wind = 12.0f; app.height = 0.4f;
    demo_open(&app.demo, options, argc, argv);
    app_init(&app);
    while (demo_running(&app.demo)) {
        if (app.demo.ui) app_ui(&app, app.demo.ui);
        app_frame(&app);
    }
    if (app.demo.check) app_check(&app);
    app_shutdown(&app);
    return demo_close(&app.demo);
}
