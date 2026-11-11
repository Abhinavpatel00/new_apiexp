/* taa — temporal anti-aliasing, measured against 64 samples per pixel.
 *
 *   Scene (jittered; colour + velocity + depth) -> Resolve (history A -> B, next frame B -> A) -> Present
 *
 * See shaders/taa.slang. The history image alternates between two targets,
 * so there are two frame programs, one per parity.
 */
#include "demo.h"
#include "mesh.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SHADERS  "demos/taa/shaders/"
#define MAX_OBJS 48u

enum { RESOLVE_CLAMP = 1, RESOLVE_VELOCITY = 2, RESOLVE_PASS_THROUGH = 4, RESOLVE_NO_HISTORY = 8 };

typedef struct Push {
    float    vp[16], prev_vp[16];
    float    eye[4], jitter_size[4];
    uint32_t vertex_offset, object_offset, prev_object_offset, flags;
    uint32_t color, velocity, history, sampler;
    float    alpha, pad[3];
} Push;
_Static_assert(sizeof(Push) == 208, "Push must match taa.slang");

typedef struct App {
    Demo          demo; /* first: see demo.h */
    DemoOrbit     orbit;
    DemoMesh      mesh;
    DemoObject    objects[MAX_OBJS], prev_objects[MAX_OBJS];
    DemoRange     ranges[MAX_OBJS];
    uint32_t      object_count, mover;
    BufSlice      vertices, object_data, prev_object_data;
    Target        color, velocity, depth, history[2];
    uint32_t      color_index, velocity_index, history_index[2];
    Pipe          scene_pipe, resolve_pipe;
    AttachDesc    scene_colors[2], scene_depth, resolve_color[2];
    BufUse        reads[3];
    Target        resolve_reads[2][3];
    FrameProgram *programs[2];
    Push          push;
    /* settings */
    bool          taa, clamp, use_velocity, still;
    float         alpha, spin;
    /* state */
    uint32_t      frame_no, parity;
    bool          history_valid;
    /* --check drives these */
    bool          manual;
    float         manual_time, manual_jitter[2];
    bool          override_jitter;
    float         base_yaw;
} App;

/* ---- scene ------------------------------------------------------------------ */

static void add(App *a, DemoRange range, float x, float y, float z, float yaw, float r, float g, float b, bool ground) {
    DemoObject *o;
    if (a->object_count >= MAX_OBJS) return;
    a->ranges[a->object_count] = range;
    o = &a->objects[a->object_count++];
    demo_object(o, x, y, z, yaw, 1.0f, r, g, b);
    o->color[3] = ground ? 0.0f : 1.0f;
}

static void build_scene(App *a) {
    DemoMesh       *m      = &a->mesh;
    const DemoRange ground = demo_mesh_plane(m, 30.0f);
    const DemoRange box    = demo_mesh_box(m, 0.6f, 0.6f, 0.6f);
    const DemoRange bar    = demo_mesh_box(m, 0.012f, 0.9f, 0.012f);   /* thinner than a pixel a few metres away */
    const DemoRange rail   = demo_mesh_box(m, 2.2f, 0.012f, 0.012f);
    const DemoRange ball   = demo_mesh_sphere(m, 0.5f, 40u, 20u);
    uint32_t        i;
    add(a, ground, 0, 0, 0, 0, 0.85f, 0.85f, 0.85f, true);
    add(a, box, -2.6f, 0.6f, -1.8f, 0.5f, 0.9f, 0.55f, 0.2f, false);
    add(a, box, 2.8f, 0.6f, -2.4f, 1.1f, 0.25f, 0.6f, 0.9f, false);
    add(a, box, 0.4f, 0.6f, -4.2f, 0.2f, 0.9f, 0.9f, 0.9f, false);
    for (i = 0u; i < 15u; ++i) add(a, bar, -2.1f + 0.3f * (float)i, 0.9f, -0.4f, 0.0f, 0.95f, 0.95f, 0.95f, false);
    add(a, rail, 0.0f, 1.8f, -0.4f, 0.0f, 0.95f, 0.95f, 0.95f, false);
    add(a, rail, 0.0f, 0.9f, -0.4f, 0.0f, 0.95f, 0.95f, 0.95f, false);
    a->mover = a->object_count;
    add(a, ball, 0.0f, 0.5f, 1.2f, 0.0f, 0.9f, 0.15f, 0.12f, false);
}

/* Halton(2, 3), entries 1..8, centred: the usual TAA sequence. */
static void halton(uint32_t index, float out[2]) {
    static const uint32_t k_base[2] = {2u, 3u};
    int axis;
    for (axis = 0; axis < 2; ++axis) {
        float    f = 1.0f, r = 0.0f;
        uint32_t i = index % 8u + 1u;
        while (i) { f /= (float)k_base[axis]; r += f * (float)(i % k_base[axis]); i /= k_base[axis]; }
        out[axis] = r - 0.5f;
    }
}

/* ---- frame ------------------------------------------------------------------ */

static void reset_history(App *a) { a->history_valid = false; }

static void resolve(App *a, PassCtx *p, uint32_t parity) {
    Push push = a->push;
    push.color    = a->color_index;
    push.velocity = a->velocity_index;
    push.history  = a->history_index[1u - parity];
    push.sampler  = a->demo.linear_clamp_index;
    pass_push(p, &push, sizeof push);
    pass_draw_fullscreen_triangle(p, a->demo.width, a->demo.height);
}
/* One frame: what changed since the last one, then every pass, in the order app_init listed them. */
static void app_frame(App *a) {
    Demo       *d = &a->demo;
    Push       *p = &a->push;
    DemoOrbit   orbit;
    float       eye[3], jitter[2] = {0.0f, 0.0f};
    const float t = a->manual ? a->manual_time : a->still ? 0.0f : d->time;
    PassCtx *pass;

    if (!d->headless) demo_orbit_input(d, &a->orbit);
    memcpy(a->prev_objects, a->objects, sizeof a->objects);
    memcpy(p->prev_vp, p->vp, sizeof p->vp);

    a->objects[a->mover].model[12] = 2.5f * sinf(0.8f * t);
    orbit = a->orbit;
    orbit.yaw += a->spin * t;
    demo_orbit_camera(&orbit, (float)d->width / (float)d->height, NULL, NULL, p->vp, eye);
    p->eye[0] = eye[0]; p->eye[1] = eye[1]; p->eye[2] = eye[2];
    if (!a->history_valid) {             /* nothing to come from: last frame = this frame */
        memcpy(a->prev_objects, a->objects, sizeof a->objects);
        memcpy(p->prev_vp, p->vp, sizeof p->vp);
    }
    if (a->override_jitter) { jitter[0] = a->manual_jitter[0]; jitter[1] = a->manual_jitter[1]; }
    else if (a->taa) halton(a->frame_no, jitter);
    p->jitter_size[0] = jitter[0]; p->jitter_size[1] = jitter[1];
    p->jitter_size[2] = (float)d->width; p->jitter_size[3] = (float)d->height;
    p->alpha = a->alpha;
    p->flags = (a->clamp ? RESOLVE_CLAMP : 0u) | (a->use_velocity ? RESOLVE_VELOCITY : 0u) |
               (a->taa ? 0u : RESOLVE_PASS_THROUGH) | (a->history_valid ? 0u : RESOLVE_NO_HISTORY);
    demo_upload(d, a->object_data, a->objects, (uint64_t)a->object_count * sizeof(DemoObject));
    demo_upload(d, a->prev_object_data, a->prev_objects, (uint64_t)a->object_count * sizeof(DemoObject));

    a->parity = a->frame_no & 1u;        /* this frame writes history[parity] and reads the other */
    d->program = a->programs[a->parity];
    ++a->frame_no;
    a->history_valid = true;

    if (!demo_frame_begin(d)) return;
    pass = demo_pass(d, "Scene");
    {
        Push     push = a->push;
        uint32_t i;
        for (i = 0u; i < a->object_count; ++i) {
            push.vertex_offset      = (uint32_t)a->vertices.offset + a->ranges[i].first * (uint32_t)sizeof(DemoVertex);
            push.object_offset      = (uint32_t)a->object_data.offset + i * (uint32_t)sizeof(DemoObject);
            push.prev_object_offset = (uint32_t)a->prev_object_data.offset + i * (uint32_t)sizeof(DemoObject);
            pass_push(pass, &push, sizeof push);
            pass_draw(pass, a->ranges[i].count, 1u, 0u, 0u);
        }
    }
    resolve(a, demo_pass(d, "Resolve"), a->parity);
    demo_present(d);
    demo_frame_end(d);
}

static FrameProgram *build(App *a, uint32_t parity) {
    Demo    *d = &a->demo;
    PassDesc passes[5];
    uint32_t count = 0u;

    passes[count++] = (PassDesc){
        .name = "Scene", .kind = PASS_GRAPHICS,
        .pipe = a->scene_pipe,
        .state = draw_state_opaque(SC_1, CULL_BACK),
        .colors = a->scene_colors, .color_count = 2u,
        .depth = &a->scene_depth,
        .buf_reads = a->reads, .buf_read_count = 3u};

    passes[count++] = (PassDesc){
        .name = "Resolve", .kind = PASS_GRAPHICS,
        .pipe = a->resolve_pipe,
        .state = draw_state_opaque(SC_1, CULL_NONE),
        .colors = &a->resolve_color[parity], .color_count = 1u,
        .tex_reads = a->resolve_reads[parity], .tex_read_count = 3u};

    passes[count++] = demo_present_pass(d, a->history[parity]);
    return demo_program(d, passes, count);
}

static void app_init(App *a) {
    Demo    *d = &a->demo;
    ShaderId vs, scene_fs, resolve_fs;
    PipeKey  key;
    uint32_t i;

    build_scene(a);
    memcpy(a->prev_objects, a->objects, sizeof a->objects);
    a->vertices         = demo_alloc(d, a->mesh.vertices, (uint64_t)a->mesh.count * sizeof(DemoVertex));
    a->object_data      = demo_alloc(d, a->objects, (uint64_t)a->object_count * sizeof(DemoObject));
    a->prev_object_data = demo_alloc(d, a->objects, (uint64_t)a->object_count * sizeof(DemoObject));
    a->reads[0] = use_read(a->vertices.buffer, a->vertices.offset, a->vertices.size);
    a->reads[1] = use_read(a->object_data.buffer, a->object_data.offset, a->object_data.size);
    a->reads[2] = use_read(a->prev_object_data.buffer, a->prev_object_data.offset, a->prev_object_data.size);
    a->reads[0].stages = a->reads[1].stages = a->reads[2].stages = SH_VERT;

    a->color      = demo_target(d, "taa_color", FMT_RGBA16F, d->width, d->height, false, &a->color_index);
    a->velocity   = demo_target(d, "taa_velocity", FMT_RG16F, d->width, d->height, false, &a->velocity_index);
    a->depth      = demo_target(d, "taa_depth", FMT_D32F, d->width, d->height, false, NULL);
    a->history[0] = demo_target(d, "taa_history0", FMT_RGBA16F, d->width, d->height, false, &a->history_index[0]);
    a->history[1] = demo_target(d, "taa_history1", FMT_RGBA16F, d->width, d->height, false, &a->history_index[1]);

    vs         = demo_shader(d, SHADERS "main.vert.spv", STAGE_VERTEX);
    scene_fs   = demo_shader(d, SHADERS "scene.frag.spv", STAGE_FRAGMENT);
    resolve_fs = demo_shader(d, SHADERS "resolve.frag.spv", STAGE_FRAGMENT);
    memset(&key, 0, sizeof key);
    key.vs = vs; key.fs = scene_fs;
    key.color[0] = FMT_RGBA16F; key.color[1] = FMT_RG16F; key.color_count = 2u;
    key.depth = FMT_D32F;
    key.blend_fallback = BLEND_OPAQUE;
    a->scene_pipe   = pipe_intern(d->gpu, &key);
    a->resolve_pipe = pipe_intern_graphics(d->gpu, d->fullscreen_vs, resolve_fs, FMT_RGBA16F, FMT_NONE, BLEND_OPAQUE);

    a->scene_colors[0] = attach_clear(a->color, 0.30f, 0.42f, 0.60f, 0.0f);
    a->scene_colors[1] = attach_clear(a->velocity, 0.0f, 0.0f, 0.0f, 0.0f);
    a->scene_depth     = attach_depth_clear(a->depth, 1.0f);
    for (i = 0u; i < 2u; ++i) {
        a->resolve_color[i]    = attach_color(a->history[i], ATT_WRITE);
        a->resolve_reads[i][0] = a->color;
        a->resolve_reads[i][1] = a->velocity;
        a->resolve_reads[i][2] = a->history[1u - i];
        a->programs[i] = build(a, i);
    }
    d->program = a->programs[0];
}

static void app_ui(App *a, struct nk_context *ctx) {
    Demo      *d   = &a->demo;
    const bool was = a->taa;
    if (nk_begin(ctx, "Temporal anti-aliasing", nk_rect(10, 10, 320, 240),
                 NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_TITLE)) {
        demo_ui_text(ctx, "%.1f ms", (double)d->dt * 1000.0);
        demo_ui_bool(ctx, "Temporal anti-aliasing", &a->taa);
        demo_ui_bool(ctx, "Clamp history to the neighbourhood", &a->clamp);
        demo_ui_bool(ctx, "Follow motion (velocity)", &a->use_velocity);
        demo_ui_float(ctx, "New frame weight", &a->alpha, 0.02f, 1.0f);
        demo_ui_float(ctx, "Camera spin, rad/s", &a->spin, 0.0f, 1.0f);
        demo_ui_bool(ctx, "Freeze the scene", &a->still);
    }
    nk_end(ctx);
    if (was != a->taa) reset_history(a);
}

#include "check.inc"

static void app_shutdown(App *a) {
    uint32_t i;
    for (i = 0u; i < 2u; ++i)   /* demo.c destroys the program it last ran */
        if (a->programs[i] && a->programs[i] != a->demo.program) frame_program_destroy(a->demo.gpu, a->programs[i]);
    demo_mesh_free(&a->mesh);
}

int main(int argc, char **argv) {
    static App app;
    static const DemoOption options[] = {
        {"--off", .off = &app.taa, .help = "no anti-aliasing"},
        {"--no-clamp", .off = &app.clamp, .help = "do not clamp the history to the current neighbourhood"},
        {"--no-velocity", .off = &app.use_velocity, .help = "take the history from the same pixel"},
        {"--alpha", .f32 = &app.alpha, .lo = 0.001, .hi = 1, .help = "weight of the new frame"},
        {"--spin", .f32 = &app.spin, .lo = -100, .hi = 100, .help = "camera rotation, rad/s"},
        {"--still", .flag = &app.still, .help = "freeze the scene at time 0"},
        {0}};
    demo_defaults(&app.demo, "taa", "Temporal anti-aliasing");
    app.orbit = (DemoOrbit){.target = {0.0f, 0.8f, 0.0f}, .distance = 6.5f, .yaw = 0.3f, .pitch = 0.27f,
                            .fov_deg = 50.0f, .znear = 0.1f, .zfar = 200.0f, .min_distance = 2.0f, .max_distance = 30.0f};
    app.taa = app.clamp = app.use_velocity = true;
    app.alpha = 0.1f;
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
