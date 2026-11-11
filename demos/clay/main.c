/* clay — clay as particles, drawn through a distance volume rebuilt from them.
 *
 *   [Clear -> Predict -> Bin -> Sort -> Solve x K -> Finish] x S -> Clear -> Bin -> Sort -> Bake -> Draw -> Present
 *
 * 4 096 particles of 5 cm radius in a tray, a ball ploughing through them.
 * Everything from Predict to Bake is compute; see shaders/sim.slang and
 * shaders/bake.slang.
 */
#include "demo.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SHADERS       "demos/clay/shaders/"
#define SIDE          16u
#define COUNT         (SIDE * SIDE * SIDE)
#define RADIUS        0.05f
#define CELLS         32u
#define CELL          0.1f
#define CELL_CAP      8u
#define CELL_STRIDE   36u
#define VOXELS        64u
#define VOXEL         0.05f
#define HALF          1.2f
#define REACH         0.1f
#define MAX_SOLVES    8u
#define MAX_SUBSTEPS  8u
#define PADDLE_RADIUS 0.33f
#define STEP          (1.0f / 60.0f)

typedef struct Push {
    uint32_t particles, src, dst, grid;
    uint32_t count, volume, sampler, view;
    float    dt, radius, relax, friction;
    float    paddle[4], look[4];
    float    eye[4], right[4], up[4], forward[4];
    float    size[4];
} Push;
_Static_assert(sizeof(Push) == 160, "Push must match clay_common.slang");

typedef struct Particle { float pos[3], contacts, vel[3], pad; } Particle;
_Static_assert(sizeof(Particle) == 32, "Particle must match clay_common.slang");

typedef struct App {
    Demo          demo; /* first: see demo.h */
    DemoOrbit     orbit;
    BufSlice      particles, predicted[2], grid;
    Target        volume, hdr;
    uint32_t      volume_index;
    Pipe          clear_pipe, predict_pipe, bin_pipe, sort_pipe, solve_pipe, finish_pipe, bake_pipe, draw_pipe;
    BufUse        clear_write, predict_reads[1], predict_writes[1], bin_reads[2], bin_writes[1];
    BufUse        solve_reads[2][2], solve_writes[2][1], finish_reads[1], finish_writes[1], bake_reads[2];
    AttachDesc    hdr_color;
    FrameProgram *simulate, *draw_only;
    Push          push;
    uint32_t      solves, substeps, view, steps;   /* steps: frames simulated */
    float         relax, friction, smooth, inflate;
    bool          paddle, paused;
    Particle     *seed;                             /* the starting block, kept for reset() */
} App;

/* ---- frame ------------------------------------------------------------------ */

static void paddle_at(uint32_t step, float out[3]) {
    const float t = (float)step * STEP;
    out[0] = 0.7f * cosf(1.2f * t);
    out[1] = 0.6f;
    out[2] = 0.7f * sinf(1.2f * t);
}

/* One frame: what changed since the last one, then every pass, in the order app_init listed them. */
static void push_buffers(App *a, PassCtx *p, uint32_t src, uint32_t dst) {
    Push push = a->push;
    push.src = (uint32_t)a->predicted[src].offset;
    push.dst = (uint32_t)a->predicted[dst].offset;
    pass_push(p, &push, sizeof push);
}
/* Where the solved positions end up: Predict writes 0, each Solve flips. */
static uint32_t final_buffer(const App *a) { return a->solves & 1u; }

static void dispatch(App *a, PassCtx *p, uint32_t src, uint32_t dst, uint32_t groups) {
    push_buffers(a, p, src, dst);
    pass_dispatch(p, groups, 1u, 1u);
}

static void app_frame(App *a) {
    Demo       *d = &a->demo;
    Push       *p = &a->push;
    const float aspect = (float)d->width / (float)d->height;
    float       view[16], eye[3], tan_y;
    uint32_t    i;
    PassCtx *pass;

    if (!d->headless) demo_orbit_input(d, &a->orbit);
    demo_orbit_camera(&a->orbit, aspect, view, NULL, NULL, eye);
    tan_y = tanf(a->orbit.fov_deg * 0.017453292f * 0.5f);
    for (i = 0u; i < 3u; ++i) {
        p->eye[i]     = eye[i];
        p->right[i]   = view[i * 4u] * tan_y * aspect;
        p->up[i]      = view[i * 4u + 1u] * tan_y;
        p->forward[i] = -view[i * 4u + 2u];
    }
    p->size[0] = (float)d->width; p->size[1] = (float)d->height;
    p->particles = (uint32_t)a->particles.offset;
    p->grid      = (uint32_t)a->grid.offset;
    p->count     = COUNT;
    p->volume    = a->volume_index;
    p->sampler   = d->linear_clamp_index;
    p->view      = a->view;
    p->dt = STEP / (float)a->substeps; p->radius = RADIUS; p->relax = a->relax; p->friction = a->friction;
    paddle_at(a->steps + 1u, p->paddle);           /* where it will be at the end of this step */
    p->paddle[3] = a->paddle ? PADDLE_RADIUS : 0.0f;
    p->look[0] = a->smooth; p->look[1] = a->inflate;
    d->program = a->paused ? a->draw_only : a->simulate;
    if (!a->paused) ++a->steps;

    if (!demo_frame_begin(d)) return;
    if (d->program == a->simulate) {
        /* Where the solved positions end up: Predict writes 0, each Solve flips. */
        const uint32_t last = final_buffer(a);
        const uint32_t cell_groups = CELLS * CELLS * CELLS / 64u, particle_groups = COUNT / 64u;
        uint32_t       sub, k;
        for (sub = 0u; sub < a->substeps; ++sub) {
            dispatch(a, demo_pass(d, "Clear"), 0u, 0u, cell_groups);
            dispatch(a, demo_pass(d, "Predict"), 0u, 0u, particle_groups);
            dispatch(a, demo_pass(d, "Bin"), 0u, 0u, particle_groups);
            dispatch(a, demo_pass(d, "Sort"), 0u, 0u, cell_groups);
            for (k = 0u; k < a->solves; ++k) dispatch(a, demo_pass(d, "Solve"), k & 1u, (k + 1u) & 1u, particle_groups);
            dispatch(a, demo_pass(d, "Finish"), last, last, particle_groups);
        }
        /* The solved positions, binned once more, become the volume that is drawn. */
        dispatch(a, demo_pass(d, "Clear2"), 0u, 0u, cell_groups);
        dispatch(a, demo_pass(d, "Bin2"), last, last, particle_groups);
        dispatch(a, demo_pass(d, "Sort2"), 0u, 0u, cell_groups);
        pass = demo_pass(d, "Bake");
        push_buffers(a, pass, last, last);
        pass_dispatch(pass, VOXELS / 4u, VOXELS / 4u, VOXELS / 4u);
    }
    pass = demo_pass(d, "Draw");
    push_buffers(a, pass, 0u, 0u);
    pass_draw_fullscreen_triangle(pass, d->width, d->height);
    demo_present(d);
    demo_frame_end(d);
}

static PassDesc *compute_pass(PassDesc *p, const char *name, Pipe pipe, BufUse *reads, uint32_t read_count,
                              BufUse *writes, uint32_t write_count) {
    memset(p, 0, sizeof *p);
    strcpy(p->name, name);
    p->kind = PASS_COMPUTE;
    p->pipe = pipe;
    p->buf_reads = reads; p->buf_read_count = read_count;
    p->buf_writes = writes; p->buf_write_count = write_count;
    return p;
}

static FrameProgram *build(App *a, bool simulate) {
    Demo          *d = &a->demo;
    PassDesc       passes[MAX_SUBSTEPS * (MAX_SOLVES + 5u) + 9u];
    PassDesc      *p;
    uint32_t       count = 0u, k, sub;
    const uint32_t last = final_buffer(a);

    if (simulate) {
        for (sub = 0u; sub < a->substeps; ++sub) {
        compute_pass(&passes[count++], "Clear", a->clear_pipe, NULL, 0u, &a->clear_write, 1u);
        compute_pass(&passes[count++], "Predict", a->predict_pipe, a->predict_reads, 1u, a->predict_writes, 1u);
        compute_pass(&passes[count++], "Bin", a->bin_pipe, &a->solve_reads[0][0], 1u, a->bin_writes, 1u);
        compute_pass(&passes[count++], "Sort", a->sort_pipe, NULL, 0u, &a->clear_write, 1u);
        for (k = 0u; k < a->solves; ++k)
            compute_pass(&passes[count++], "Solve", a->solve_pipe, a->solve_reads[k & 1u], 2u, a->solve_writes[k & 1u], 1u);
        compute_pass(&passes[count++], "Finish", a->finish_pipe, &a->solve_reads[last][0], 1u, a->finish_writes, 1u);
        }
        compute_pass(&passes[count++], "Clear2", a->clear_pipe, NULL, 0u, &a->clear_write, 1u);
        compute_pass(&passes[count++], "Bin2", a->bin_pipe, &a->solve_reads[last][0], 1u, a->bin_writes, 1u);
        compute_pass(&passes[count++], "Sort2", a->sort_pipe, NULL, 0u, &a->clear_write, 1u);
        p = compute_pass(&passes[count++], "Bake", a->bake_pipe, a->solve_reads[last], 2u, NULL, 0u);
        p->tex_writes = &a->volume; p->tex_write_count = 1u;
    }
    passes[count++] = (PassDesc){
        .name = "Draw", .kind = PASS_GRAPHICS,
        .pipe = a->draw_pipe,
        .state = draw_state_opaque(SC_1, CULL_NONE),
        .colors = &a->hdr_color, .color_count = 1u,
        .tex_reads = &a->volume, .tex_read_count = 1u};
    passes[count++] = demo_present_pass(d, a->hdr);
    return demo_program(d, passes, count);
}

static uint32_t lcg(uint32_t *s) { *s = *s * 1664525u + 1013904223u; return *s >> 8; }

static void app_init(App *a) {
    Demo     *d = &a->demo;
    Particle *seed = (Particle *)calloc(COUNT, sizeof *seed);
    ShaderId  cs[7], fs;
    static const char *const k_cs[7] = {"clear", "predict", "bin", "solve", "finish", "bake", "sort"};
    Pipe     *pipes[7];
    uint32_t  i, rng = 12345u;
    char      path[96];

    if (a->substeps * (a->solves + 5u) + 7u > MAX_PASSES)
        demo_fatal(d, "%u substeps of %u solver passes need more than %d passes a frame", a->substeps, a->solves, MAX_PASSES);
    /* A 16^3 block, 1.6 m on a side, hanging 90 cm above the floor; 3 mm of noise so it does not stay a crystal. */
    for (i = 0u; i < COUNT; ++i) {
        const uint32_t x = i % SIDE, y = i / SIDE % SIDE, z = i / (SIDE * SIDE);
        seed[i].pos[0] = ((float)x - 7.5f) * 0.102f + ((float)(lcg(&rng) % 1000u) / 1000.0f - 0.5f) * 0.006f;
        seed[i].pos[1] = 0.9f + (float)y * 0.102f + ((float)(lcg(&rng) % 1000u) / 1000.0f - 0.5f) * 0.006f;
        seed[i].pos[2] = ((float)z - 7.5f) * 0.102f + ((float)(lcg(&rng) % 1000u) / 1000.0f - 0.5f) * 0.006f;
    }
    a->particles    = demo_alloc(d, seed, (uint64_t)COUNT * sizeof(Particle));
    a->seed         = seed;
    a->predicted[0] = demo_alloc(d, NULL, (uint64_t)COUNT * 16u);
    a->predicted[1] = demo_alloc(d, NULL, (uint64_t)COUNT * 16u);
    a->grid         = demo_alloc(d, NULL, (uint64_t)CELLS * CELLS * CELLS * CELL_STRIDE);

    a->volume = demo_volume(d, "clay_volume", FMT_RGBA16F, VOXELS, VOXELS, VOXELS, &a->volume_index);
    a->hdr    = demo_target(d, "clay_hdr", FMT_RGBA16F, d->width, d->height, false, NULL);

    pipes[0] = &a->clear_pipe; pipes[1] = &a->predict_pipe; pipes[2] = &a->bin_pipe;
    pipes[3] = &a->solve_pipe; pipes[4] = &a->finish_pipe; pipes[5] = &a->bake_pipe; pipes[6] = &a->sort_pipe;
    for (i = 0u; i < 7u; ++i) {
        snprintf(path, sizeof path, SHADERS "%s.comp.spv", k_cs[i]);
        cs[i] = demo_shader(d, path, STAGE_COMPUTE);
        *pipes[i] = pipe_intern_compute(d->gpu, cs[i], "main");
    }
    fs = demo_shader(d, SHADERS "draw.frag.spv", STAGE_FRAGMENT);
    a->draw_pipe = pipe_intern_graphics(d->gpu, d->fullscreen_vs, fs, FMT_RGBA16F, FMT_NONE, BLEND_OPAQUE);

#define READ(s)  use_read((s).buffer, (s).offset, (s).size)
#define WRITE(s) use_write((s).buffer, (s).offset, (s).size)
    a->clear_write       = WRITE(a->grid);
    a->predict_reads[0]  = READ(a->particles);
    a->predict_writes[0] = WRITE(a->predicted[0]);
    a->bin_writes[0]     = WRITE(a->grid);
    for (i = 0u; i < 2u; ++i) {
        a->solve_reads[i][0]  = READ(a->predicted[i]);
        a->solve_reads[i][1]  = READ(a->grid);
        a->solve_writes[i][0] = WRITE(a->predicted[1u - i]);
        a->solve_reads[i][0].stages = a->solve_reads[i][1].stages = a->solve_writes[i][0].stages = SH_COMP;
    }
    a->finish_writes[0] = WRITE(a->particles);
#undef READ
#undef WRITE
    a->clear_write.stages = a->predict_reads[0].stages = a->predict_writes[0].stages = SH_COMP;
    a->bin_writes[0].stages = a->finish_writes[0].stages = SH_COMP;
    a->hdr_color = attach_color(a->hdr, ATT_WRITE);

    a->simulate  = build(a, true);
    a->draw_only = build(a, false);
    d->program = a->simulate;
}

/* Back to the starting block. */
static void reset(App *a) {
    a->steps = 0u;
    demo_upload(&a->demo, a->particles, a->seed, (uint64_t)COUNT * sizeof(Particle));
}

static void app_ui(App *a, struct nk_context *ctx) {
    static const char *const k_views[3] = {"lit", "steps per ray", "normal"};
    Demo *d = &a->demo;
    if (nk_begin(ctx, "Clay", nk_rect(10, 10, 320, 260),
                 NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_TITLE)) {
        demo_ui_text(ctx, "%.1f ms   %u particles, %u x %u solver passes", (double)d->dt * 1000.0, COUNT, a->substeps, a->solves);
        demo_ui_choice(ctx, "View", k_views, 3u, &a->view);
        demo_ui_bool(ctx, "Paddle", &a->paddle);
        demo_ui_bool(ctx, "Pause", &a->paused);
        nk_layout_row_dynamic(ctx, 22, 1);
        if (nk_button_label(ctx, "Drop the block again")) reset(a);
        demo_ui_float(ctx, "Stickiness", &a->friction, 0.0f, 0.9f);
        demo_ui_float(ctx, "Smooth union, m", &a->smooth, 0.0f, 0.08f);
        demo_ui_float(ctx, "Skin, m", &a->inflate, 0.0f, 0.04f);
    }
    nk_end(ctx);
}

#include "check.inc"

static void app_shutdown(App *a) {
    free(a->seed);
    if (a->simulate && a->simulate != a->demo.program) frame_program_destroy(a->demo.gpu, a->simulate);
    if (a->draw_only && a->draw_only != a->demo.program) frame_program_destroy(a->demo.gpu, a->draw_only);
}

int main(int argc, char **argv) {
    static App app;
    static const DemoOption options[] = {
        {"--view", .u32 = &app.view, .lo = 0, .hi = 2, .help = "0 lit, 1 steps per ray, 2 normal"},   /* 3 is --check's */
        {"--substeps", .u32 = &app.substeps, .lo = 1, .hi = MAX_SUBSTEPS, .help = "simulation steps per frame"},
        {"--solves", .u32 = &app.solves, .lo = 1, .hi = MAX_SOLVES, .help = "solver passes per step"},
        {"--relax", .f32 = &app.relax, .lo = 0, .hi = 4, .help = "how much of each pair's overlap a pass removes"},
        {"--sticky", .f32 = &app.friction, .lo = 0, .hi = 1, .help = "share of its speed a touching particle loses per step"},
        {"--no-paddle", .off = &app.paddle, .help = "nothing stirs the clay"},
        {0}};
    demo_defaults(&app.demo, "clay", "Clay - particles drawn through a distance volume");
    app.orbit = (DemoOrbit){.target = {0.0f, 0.45f, 0.0f}, .distance = 3.9f, .yaw = 0.6f, .pitch = 0.6f,
                            .fov_deg = 50.0f, .znear = 0.1f, .zfar = 100.0f, .min_distance = 1.5f, .max_distance = 20.0f};
    app.solves = 1u; app.substeps = 8u; app.relax = 1.5f; app.friction = 0.15f; app.smooth = 0.05f; app.inflate = 0.015f;
    app.paddle = true;
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
