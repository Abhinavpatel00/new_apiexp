/* bricks — a distance volume stored sparsely: bricks in an atlas behind an indirection volume.
 *
 *   once:        Classify -> Allocate -> Bricks (indirect) [-> Dense]
 *   every frame: Draw (sphere tracing) -> Present
 *
 * See shaders/bricks_common.slang for the layout and shaders/build.slang for the build.
 */
#include "demo.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SHADERS    "demos/bricks/shaders/"
#define WORLD      16.0f
#define MAX_BRICKS 72u     /* a side: 505 samples */

typedef struct Push {
    uint32_t flags, list, args, bricks;
    uint32_t atlas_side, indirection, atlas, dense;
    uint32_t sampler, view, mode, capacity;
    float    eye[4], right[4], up[4], forward[4], size[4];
    float    world[4];
} Push;
_Static_assert(sizeof(Push) == 144, "Push must match bricks_common.slang");

typedef struct App {
    Demo       demo; /* first: see demo.h */
    DemoOrbit  orbit;
    BufSlice   flags, list, args;
    Target     indirection, atlas, dense, hdr;
    uint32_t   indirection_index, atlas_index, dense_index;
    Pipe       classify_pipe, allocate_pipe, bricks_pipe, dense_pipe, draw_pipe;
    BufUse     flags_write, allocate_reads[1], allocate_writes[2], bricks_reads[3];
    Target     draw_reads[3];
    AttachDesc hdr_color;
    Push       push;
    uint32_t   bricks, atlas_side, view, mode;   /* B; atlas side in bricks */
    uint32_t   frame_no;
    bool       want_dense, have_dense, built, reported;
    uint32_t   stored, wanted;                   /* read back once after the build */
} App;

static uint32_t samples(const App *a) { return a->bricks * 7u + 1u; }

/* ---- frame ------------------------------------------------------------------ */

static void report(App *a) {
    Demo    *d = &a->demo;
    uint32_t args[8];
    const double n = (double)samples(a), side = 8.0 * a->atlas_side, b = (double)a->bricks;
    demo_readback_buffer(d, a->args, args, sizeof args);
    a->stored = args[3]; a->wanted = args[4];
    printf("[bricks] %u^3 samples in %u^3 bricks: %u stored of %u (%.1f %%)\n", samples(a), a->bricks, a->stored,
           a->bricks * a->bricks * a->bricks, 100.0 * a->stored / (b * b * b));
    if (a->wanted > a->stored)
        printf("[bricks] ATLAS FULL: %u bricks are needed, %u fit; the picture has holes. Raise --atlas.\n", a->wanted, a->stored);
    printf("[bricks] sparse %.1f MB used (%.1f MB bricks + %.2f MB indirection; atlas allocated %.1f MB), dense %.1f MB\n",
           (a->stored * 4096.0 + b * b * b * 8.0) / 1048576.0, a->stored * 4096.0 / 1048576.0, b * b * b * 8.0 / 1048576.0,
           side * side * side * 8.0 / 1048576.0, n * n * n * 8.0 / 1048576.0);
}

static void push_all(App *a, PassCtx *p) { pass_push(p, &a->push, sizeof a->push); }

/* One frame: what changed since the last one, then every pass, in the order app_init listed them. */
static void app_frame(App *a) {
    Demo       *d = &a->demo;
    Push       *p = &a->push;
    const float aspect = (float)d->width / (float)d->height;
    float       view[16], eye[3], tan_y;
    uint32_t    i;
    PassCtx *pass;

    if (a->frame_no > 0u) a->built = true;         /* the first frame ran the build passes */
    ++a->frame_no;
    if (a->built && !a->reported) { report(a); a->reported = true; }
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
    p->flags = (uint32_t)a->flags.offset; p->list = (uint32_t)a->list.offset; p->args = (uint32_t)a->args.offset;
    p->bricks = a->bricks; p->atlas_side = a->atlas_side;
    p->capacity = a->atlas_side * a->atlas_side * a->atlas_side;
    p->indirection = a->indirection_index; p->atlas = a->atlas_index; p->dense = a->dense_index;
    p->sampler = d->linear_clamp_index;
    p->view = a->view;
    p->mode = a->have_dense ? a->mode : 0u;
    p->world[0] = WORLD / (float)(samples(a) - 1u);

    if (!demo_frame_begin(d)) return;
    /* The build passes are in the program every frame and do their work in the first. */
    pass = demo_pass(d, "Classify");
    if (!a->built) {
        const uint32_t groups = (a->bricks + 3u) / 4u;
        push_all(a, pass);
        pass_dispatch(pass, groups, groups, groups);
    }
    pass = demo_pass(d, "Allocate");
    if (!a->built) {
        push_all(a, pass);
        pass_dispatch(pass, 1u, 1u, 1u);
    }
    pass = demo_pass(d, "Bricks");
    if (!a->built) {
        push_all(a, pass);
        pass_dispatch_indirect(pass, (BufSlice){a->args.buffer, a->args.offset, 12u});
    }
    if (a->have_dense) {
        pass = demo_pass(d, "Dense");
        if (!a->built) {
            const uint32_t groups = (samples(a) + 3u) / 4u;
            push_all(a, pass);
            pass_dispatch(pass, groups, groups, groups);
        }
    }
    pass = demo_pass(d, "Draw");
    push_all(a, pass);
    pass_draw_fullscreen_triangle(pass, d->width, d->height);
    demo_present(d);
    demo_frame_end(d);
}

static void app_init(App *a) {
    static const char *const k_cs[4] = {"classify", "allocate", "bricks", "dense"};
    Demo     *d = &a->demo;
    PassDesc  passes[8];
    PassDesc *p;
    Pipe     *pipes[4];
    ShaderId  fs;
    uint32_t  count = 0u, i;
    const uint32_t cells = a->bricks * a->bricks * a->bricks, capacity = a->atlas_side * a->atlas_side * a->atlas_side;
    char      path[96];

    a->have_dense = a->want_dense || d->check;
    if (a->want_dense) a->mode = 1u;   /* --dense also draws it */
    a->flags = demo_alloc(d, NULL, (uint64_t)cells * 4u);
    a->list  = demo_alloc(d, NULL, (uint64_t)capacity * 4u);
    a->args  = demo_alloc(d, NULL, 32u);

    a->indirection = demo_volume(d, "bricks_indirection", FMT_RGBA16F, a->bricks, a->bricks, a->bricks, &a->indirection_index);
    a->atlas       = demo_volume(d, "bricks_atlas", FMT_RGBA16F, a->atlas_side * 8u, a->atlas_side * 8u, a->atlas_side * 8u,
                                 &a->atlas_index);
    a->hdr         = demo_target(d, "bricks_hdr", FMT_RGBA16F, d->width, d->height, false, NULL);
    if (a->have_dense) {
        a->dense = demo_volume(d, "bricks_dense", FMT_RGBA16F, samples(a), samples(a), samples(a), &a->dense_index);
    }

    pipes[0] = &a->classify_pipe; pipes[1] = &a->allocate_pipe; pipes[2] = &a->bricks_pipe; pipes[3] = &a->dense_pipe;
    for (i = 0u; i < 4u; ++i) {
        ShaderId cs;
        snprintf(path, sizeof path, SHADERS "%s.comp.spv", k_cs[i]);
        cs = demo_shader(d, path, STAGE_COMPUTE);
        *pipes[i] = pipe_intern_compute(d->gpu, cs, "main");
    }
    fs = demo_shader(d, SHADERS "draw.frag.spv", STAGE_FRAGMENT);
    a->draw_pipe = pipe_intern_graphics(d->gpu, d->fullscreen_vs, fs, FMT_RGBA16F, FMT_NONE, BLEND_OPAQUE);

    a->flags_write        = use_write(a->flags.buffer, a->flags.offset, a->flags.size);
    a->allocate_reads[0]  = use_read(a->flags.buffer, a->flags.offset, a->flags.size);
    a->allocate_writes[0] = use_write(a->list.buffer, a->list.offset, a->list.size);
    a->allocate_writes[1] = use_write(a->args.buffer, a->args.offset, a->args.size);
    a->bricks_reads[0]    = use_read(a->list.buffer, a->list.offset, a->list.size);
    a->bricks_reads[1]    = use_read(a->args.buffer, a->args.offset, a->args.size);
    a->bricks_reads[2]    = use_indirect(a->args.buffer, a->args.offset, a->args.size);
    a->flags_write.stages = a->allocate_reads[0].stages = a->allocate_writes[0].stages = SH_COMP;
    a->allocate_writes[1].stages = a->bricks_reads[0].stages = a->bricks_reads[1].stages = SH_COMP;
    a->hdr_color = attach_color(a->hdr, ATT_WRITE);
    a->draw_reads[0] = a->indirection; a->draw_reads[1] = a->atlas; a->draw_reads[2] = a->dense;

#define COMPUTE(NAME, PIPE)                                               \
    p = &passes[count++]; memset(p, 0, sizeof *p); strcpy(p->name, NAME); \
    p->kind = PASS_COMPUTE; p->pipe = PIPE
    COMPUTE("Classify", a->classify_pipe);
    p->buf_writes = &a->flags_write; p->buf_write_count = 1u;
    COMPUTE("Allocate", a->allocate_pipe);
    p->buf_reads = a->allocate_reads; p->buf_read_count = 1u;
    p->buf_writes = a->allocate_writes; p->buf_write_count = 2u;
    p->tex_writes = &a->indirection; p->tex_write_count = 1u;
    COMPUTE("Bricks", a->bricks_pipe);
    p->buf_reads = a->bricks_reads; p->buf_read_count = 3u;
    p->tex_writes = &a->atlas; p->tex_write_count = 1u;
    if (a->have_dense) {
        COMPUTE("Dense", a->dense_pipe);
        p->tex_writes = &a->dense; p->tex_write_count = 1u;
    }
#undef COMPUTE
    passes[count++] = (PassDesc){
        .name = "Draw", .kind = PASS_GRAPHICS,
        .pipe = a->draw_pipe,
        .state = draw_state_opaque(SC_1, CULL_NONE),
        .colors = &a->hdr_color, .color_count = 1u,
        .tex_reads = a->draw_reads, .tex_read_count = a->have_dense ? 3u : 2u};
    passes[count++] = demo_present_pass(d, a->hdr);
    d->program = demo_program(d, passes, count);
}

static void app_ui(App *a, struct nk_context *ctx) {
    static const char *const k_views[4] = {"lit", "steps per ray", "normal", "bricks"};
    static const char *const k_modes[2] = {"sparse", "dense"};
    Demo *d = &a->demo;
    if (nk_begin(ctx, "Sparse distance volume", nk_rect(10, 10, 330, 200),
                 NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_TITLE)) {
        demo_ui_text(ctx, "%.1f ms   %u^3 samples", (double)d->dt * 1000.0, samples(a));
        demo_ui_text(ctx, "%u of %u bricks stored", a->stored, a->bricks * a->bricks * a->bricks);
        demo_ui_choice(ctx, "View", k_views, 4u, &a->view);
        if (a->have_dense) demo_ui_choice(ctx, "Volume", k_modes, 2u, &a->mode);
    }
    nk_end(ctx);
}

#include "check.inc"

int main(int argc, char **argv) {
    static App app;
    static const DemoOption options[] = {
        {"--bricks", .u32 = &app.bricks, .lo = 4, .hi = MAX_BRICKS, .help = "bricks a side (36: 253^3 samples)"},
        {"--atlas", .u32 = &app.atlas_side, .lo = 2, .hi = 40, .help = "atlas side in bricks (20: room for 8000)"},
        {"--dense", .flag = &app.want_dense, .help = "also build the dense volume and draw that"},
        {"--view", .u32 = &app.view, .lo = 0, .hi = 3, .help = "0 lit, 1 steps per ray, 2 normal, 3 bricks"},
        {0}};
    demo_defaults(&app.demo, "bricks", "Sparse distance volume - bricks behind an indirection volume");
    app.orbit = (DemoOrbit){.target = {8.0f, 3.0f, 8.0f}, .distance = 21.0f, .yaw = 0.5f, .pitch = 0.42f,
                            .fov_deg = 50.0f, .znear = 0.1f, .zfar = 200.0f, .min_distance = 3.0f, .max_distance = 60.0f};
    app.bricks = 36u; app.atlas_side = 20u;
    demo_open(&app.demo, options, argc, argv);
    app_init(&app);
    while (demo_running(&app.demo)) {
        if (app.demo.ui) app_ui(&app, app.demo.ui);
        app_frame(&app);
    }
    if (app.demo.check) app_check(&app);
    return demo_close(&app.demo);
}
