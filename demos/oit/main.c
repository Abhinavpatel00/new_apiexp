/* oit — transparency without sorting, measured against depth peeling.
 *
 *   weighted blended:  Opaque -> Accum (add) -> Reveal (multiply) -> Composite
 *   reference:         Opaque -> [Peel k -> Over k] x 12, farthest layer first
 *   draw order:        Opaque -> Blend
 *
 * Twelve tinted slabs and spheres over an opaque scene. See shaders/oit.slang.
 */
#include "demo.h"
#include "mesh.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SHADERS  "demos/oit/shaders/"
#define MAX_OBJS 32u
#define GLASS_RADIUS 1.3f /* bounds every transparent object (the slabs: 0.9 * sqrt 2) */
#define PEELS    12u     /* layers the reference can separate; the scene has 12 transparent objects */

typedef struct Push {
    float    vp[16];
    float    eye[4], light[4];
    uint32_t vertex_offset, object_offset, weight, flags;
    uint32_t tex0, tex1, tex2, pad;
} Push;
_Static_assert(sizeof(Push) == 128, "Push must match oit.slang");

enum { MODE_WEIGHTED, MODE_PEELED, MODE_DRAW_ORDER, MODES };
static const char *const k_modes[MODES] = {"weighted blended", "depth peeled (reference)", "blended in draw order"};
enum { WEIGHT_GENERAL, WEIGHT_NONE, WEIGHT_FITTED, WEIGHTS, WEIGHT_COUNT_LAYERS = 3 };
static const char *const k_weights[WEIGHTS] = {"by depth, general", "none", "by depth, fitted to the scene"};

typedef struct App {
    Demo          demo; /* first: see demo.h */
    DemoOrbit     orbit;
    DemoMesh      mesh;
    DemoObject    objects[MAX_OBJS];
    DemoRange     ranges[MAX_OBJS];
    uint32_t      object_count, opaque_count;      /* objects[0 .. opaque_count) are opaque */
    BufSlice      vertices, object_data;
    Target        hdr, depth, accum, reveal, layer, peel[2];
    uint32_t      depth_index, accum_index, reveal_index, layer_index, peel_index[2];
    Pipe          opaque_pipe, accum_pipe, reveal_pipe, composite_pipe, peel_pipe, over_pipe, blend_pipe;
    AttachDesc    opaque_color, opaque_depth, accum_color, reveal_color, depth_read, hdr_keep, layer_color, peel_depth[2];
    BufUse        reads[2];
    Target        composite_reads[2], peel_reads[2][2];
    FrameProgram *programs[MODES];
    Push          push;
    uint32_t      mode, weight;
    bool          reverse, black, count_layers;
} App;

/* ---- scene ------------------------------------------------------------------ */

static void add(App *a, DemoRange range, float x, float y, float z, float yaw, float r, float g, float b, float alpha) {
    DemoObject *o;
    if (a->object_count >= MAX_OBJS) return;
    a->ranges[a->object_count] = range;
    o = &a->objects[a->object_count++];
    demo_object(o, x, y, z, yaw, 1.0f, r, g, b);
    o->color[3] = alpha;
}

static void build_scene(App *a) {
    DemoMesh       *m      = &a->mesh;
    const DemoRange ground = demo_mesh_plane(m, 20.0f);
    const DemoRange box    = demo_mesh_box(m, 0.6f, 0.6f, 0.6f);
    const DemoRange pillar = demo_mesh_box(m, 0.3f, 1.5f, 0.3f);
    const DemoRange ball   = demo_mesh_sphere(m, 0.7f, 40u, 20u);
    const DemoRange slab   = demo_mesh_box(m, 0.9f, 0.9f, 0.02f);
    const DemoRange bubble = demo_mesh_sphere(m, 0.55f, 40u, 20u);
    static const float tint[5][4] = {{1.0f, 0.10f, 0.08f, 0.55f}, {0.10f, 0.9f, 0.15f, 0.40f}, {0.10f, 0.25f, 1.0f, 0.65f},
                                     {1.0f, 0.85f, 0.10f, 0.35f}, {0.10f, 0.9f, 0.95f, 0.50f}};
    uint32_t i;
    add(a, ground, 0, 0, 0, 0, 0.55f, 0.55f, 0.55f, 1.0f);
    add(a, box, -3.0f, 0.6f, -1.5f, 0.5f, 0.9f, 0.9f, 0.9f, 1.0f);
    add(a, ball, 2.6f, 0.7f, -2.2f, 0.0f, 0.95f, 0.6f, 0.2f, 1.0f);
    add(a, pillar, 0.2f, 1.5f, -3.6f, 0.3f, 0.9f, 0.9f, 0.95f, 1.0f);
    add(a, box, 3.4f, 0.6f, 1.4f, 1.1f, 0.3f, 0.8f, 0.5f, 1.0f);
    a->opaque_count = a->object_count;
    /* A stack of five filters one behind the other, three more at an angle, four bubbles. */
    for (i = 0u; i < 5u; ++i)
        add(a, slab, 0.0f, 1.0f, -1.6f + 0.8f * (float)i, 0.0f, tint[i][0], tint[i][1], tint[i][2], tint[i][3]);
    add(a, slab, -2.3f, 1.0f, 0.6f, 0.9f, 1.0f, 0.2f, 0.9f, 0.6f);
    add(a, slab, 2.2f, 1.0f, 0.2f, -0.7f, 0.95f, 0.95f, 0.95f, 0.3f);
    add(a, slab, 1.2f, 1.0f, 2.4f, 0.35f, 0.15f, 0.15f, 0.15f, 0.7f);
    add(a, bubble, -0.9f, 0.9f, 1.3f, 0.0f, 1.0f, 0.5f, 0.1f, 0.5f);
    add(a, bubble, 0.7f, 1.3f, -0.4f, 0.0f, 0.6f, 0.2f, 1.0f, 0.45f);
    add(a, bubble, -1.6f, 0.6f, -2.4f, 0.0f, 0.2f, 1.0f, 0.4f, 0.6f);
    add(a, bubble, 1.8f, 0.6f, 3.1f, 0.0f, 1.0f, 1.0f, 1.0f, 0.25f);
}

/* ---- frame ------------------------------------------------------------------ */

/* One frame: what changed since the last one, then every pass, in the order app_init listed them. */
static void draw(App *a, PassCtx *p, bool transparent, uint32_t flags, uint32_t tex0, uint32_t tex1) {
    Push           push  = a->push;
    const uint32_t first = transparent ? a->opaque_count : 0u, end = transparent ? a->object_count : a->opaque_count;
    uint32_t       n;
    push.flags |= flags;
    push.tex0 = tex0; push.tex1 = tex1;
    for (n = first; n < end; ++n) {
        const uint32_t i = (transparent && a->reverse) ? end - 1u - (n - first) : n;
        push.vertex_offset = (uint32_t)a->vertices.offset + a->ranges[i].first * (uint32_t)sizeof(DemoVertex);
        push.object_offset = (uint32_t)a->object_data.offset + i * (uint32_t)sizeof(DemoObject);
        pass_push(p, &push, sizeof push);
        pass_draw(p, a->ranges[i].count, 1u, 0u, 0u);
    }
}
static void fullscreen(App *a, PassCtx *p, uint32_t tex0, uint32_t tex1) {
    Push push = a->push;
    push.tex0 = tex0; push.tex1 = tex1;
    pass_push(p, &push, sizeof push);
    pass_draw_fullscreen_triangle(p, a->demo.width, a->demo.height);
}

static void app_frame(App *a) {
    Demo    *d = &a->demo;
    Push    *p = &a->push;
    float    eye[3];
    uint32_t k;
    if (!d->headless) demo_orbit_input(d, &a->orbit);
    demo_orbit_camera(&a->orbit, (float)d->width / (float)d->height, NULL, NULL, p->vp, eye);
    p->eye[0] = eye[0]; p->eye[1] = eye[1]; p->eye[2] = eye[2];
    p->light[0] = 0.40f; p->light[1] = 0.80f; p->light[2] = 0.45f;
    {   /* View depth (clip w) of the nearest and farthest transparent thing, for the fitted weight. */
        float    lo = 1e30f, hi = 0.0f;
        uint32_t i;
        for (i = a->opaque_count; i < a->object_count; ++i) {
            const float *m = a->objects[i].model;
            const float  w = p->vp[3] * m[12] + p->vp[7] * m[13] + p->vp[11] * m[14] + p->vp[15];
            if (w - GLASS_RADIUS < lo) lo = w - GLASS_RADIUS;
            if (w + GLASS_RADIUS > hi) hi = w + GLASS_RADIUS;
        }
        p->eye[3]   = lo > a->orbit.znear ? lo : a->orbit.znear;
        p->light[3] = hi;
    }
    p->weight = a->count_layers ? (uint32_t)WEIGHT_COUNT_LAYERS : a->weight;
    p->flags  = a->black ? 1u : 0u;
    d->program = a->programs[a->mode];

    if (!demo_frame_begin(d)) return;
    draw(a, demo_pass(d, "Opaque"), false, 0u, 0u, 0u);
    if (a->mode == MODE_WEIGHTED) {
        draw(a, demo_pass(d, "Accum"), true, 0u, 0u, 0u);
        draw(a, demo_pass(d, "Reveal"), true, 0u, 0u, 0u);
        fullscreen(a, demo_pass(d, "Composite"), a->accum_index, a->reveal_index);
    } else if (a->mode == MODE_PEELED) {
        /* Peel k writes peel[k & 1] and reads the other one; the first reads none. */
        for (k = 0u; k < PEELS; ++k) {
            draw(a, demo_pass(d, "Peel"), true, k ? 0u : 2u, a->depth_index, k ? a->peel_index[(k + 1u) & 1u] : 0u);
            fullscreen(a, demo_pass(d, "Over"), a->layer_index, 0u);
        }
    } else {
        draw(a, demo_pass(d, "Blend"), true, 0u, 0u, 0u);
    }
    demo_present(d);
    demo_frame_end(d);
}

static PassDesc *graphics_pass(PassDesc *p, App *a, const char *name, Pipe pipe, AttachDesc *color, AttachDesc *depth,
                               uint8_t blend) {
    memset(p, 0, sizeof *p);
    strcpy(p->name, name);
    p->kind  = PASS_GRAPHICS;
    p->pipe  = pipe;
    p->state = draw_state_opaque(SC_1, depth ? CULL_BACK : CULL_NONE);
    p->state.blend = blend;
    if (depth) {                       /* transparent geometry: test against the opaque scene, write nothing */
        p->state.depth_cmp   = CMP_LESS;
        p->state.depth_write = 0u;
        p->depth          = depth;
        p->buf_reads      = a->reads;
        p->buf_read_count = 2u;
    }
    p->colors = color; p->color_count = 1u;
    return p;
}

static FrameProgram *build(App *a, uint32_t mode) {
    Demo     *d = &a->demo;
    PassDesc  passes[PEELS * 2u + 8u];
    PassDesc *p;
    uint32_t  count = 0u, k;

    p = graphics_pass(&passes[count++], a, "Opaque", a->opaque_pipe, &a->opaque_color, &a->opaque_depth, BLEND_OPAQUE);
    p->state.depth_write = 1u;
    if (mode == MODE_WEIGHTED) {
        graphics_pass(&passes[count++], a, "Accum", a->accum_pipe, &a->accum_color, &a->depth_read, BLEND_ADD);
        graphics_pass(&passes[count++], a, "Reveal", a->reveal_pipe, &a->reveal_color, &a->depth_read, BLEND_PREMUL);
        p = graphics_pass(&passes[count++], a, "Composite", a->composite_pipe, &a->hdr_keep, NULL, BLEND_ALPHA);
        p->tex_reads = a->composite_reads; p->tex_read_count = 2u;
    } else if (mode == MODE_PEELED) {
        for (k = 0u; k < PEELS; ++k) {
            p = graphics_pass(&passes[count++], a, "Peel", a->peel_pipe, &a->layer_color, &a->peel_depth[k & 1u], BLEND_OPAQUE);
            p->state.depth_cmp   = CMP_GREATER;     /* the farthest of what the shader lets through */
            p->state.depth_write = 1u;
            p->tex_reads      = a->peel_reads[k & 1u];
            p->tex_read_count = k == 0u ? 1u : 2u;
            p = graphics_pass(&passes[count++], a, "Over", a->over_pipe, &a->hdr_keep, NULL, BLEND_PREMUL);
            p->tex_reads = &a->layer; p->tex_read_count = 1u;
        }
    } else {
        graphics_pass(&passes[count++], a, "Blend", a->blend_pipe, &a->hdr_keep, &a->depth_read, BLEND_PREMUL);
    }
    passes[count++] = demo_present_pass(d, a->hdr);
    return demo_program(d, passes, count);
}

static void app_init(App *a) {
    static const char *const k_fs[7] = {"opaque", "accum", "reveal", "composite", "peel", "over", "blend"};
    Demo     *d = &a->demo;
    ShaderId  vs, fs[7];
    uint32_t  i;
    char      path[96];

    build_scene(a);
    a->vertices    = demo_alloc(d, a->mesh.vertices, (uint64_t)a->mesh.count * sizeof(DemoVertex));
    a->object_data = demo_alloc(d, a->objects, (uint64_t)a->object_count * sizeof(DemoObject));
    a->reads[0] = use_read(a->vertices.buffer, a->vertices.offset, a->vertices.size);
    a->reads[1] = use_read(a->object_data.buffer, a->object_data.offset, a->object_data.size);
    a->reads[0].stages = a->reads[1].stages = SH_VERT;

    a->hdr     = demo_target(d, "oit_hdr", FMT_RGBA16F, d->width, d->height, false, NULL);
    a->depth   = demo_target(d, "oit_depth", FMT_D32F, d->width, d->height, false, &a->depth_index);
    a->accum   = demo_target(d, "oit_accum", FMT_RGBA16F, d->width, d->height, false, &a->accum_index);
    a->reveal  = demo_target(d, "oit_reveal", FMT_R16F, d->width, d->height, false, &a->reveal_index);
    a->layer   = demo_target(d, "oit_layer", FMT_RGBA16F, d->width, d->height, false, &a->layer_index);
    a->peel[0] = demo_target(d, "oit_peel0", FMT_D32F, d->width, d->height, false, &a->peel_index[0]);
    a->peel[1] = demo_target(d, "oit_peel1", FMT_D32F, d->width, d->height, false, &a->peel_index[1]);

    vs = demo_shader(d, SHADERS "main.vert.spv", STAGE_VERTEX);
    for (i = 0u; i < 7u; ++i) {
        snprintf(path, sizeof path, SHADERS "%s.frag.spv", k_fs[i]);
        fs[i] = demo_shader(d, path, STAGE_FRAGMENT);
    }
    a->opaque_pipe    = pipe_intern_graphics(d->gpu, vs, fs[0], FMT_RGBA16F, FMT_D32F, BLEND_OPAQUE);
    a->accum_pipe     = pipe_intern_graphics(d->gpu, vs, fs[1], FMT_RGBA16F, FMT_D32F, BLEND_ADD);
    a->reveal_pipe    = pipe_intern_graphics(d->gpu, vs, fs[2], FMT_R16F, FMT_D32F, BLEND_PREMUL);
    a->composite_pipe = pipe_intern_graphics(d->gpu, d->fullscreen_vs, fs[3], FMT_RGBA16F, FMT_NONE, BLEND_ALPHA);
    a->peel_pipe      = pipe_intern_graphics(d->gpu, vs, fs[4], FMT_RGBA16F, FMT_D32F, BLEND_OPAQUE);
    a->over_pipe      = pipe_intern_graphics(d->gpu, d->fullscreen_vs, fs[5], FMT_RGBA16F, FMT_NONE, BLEND_PREMUL);
    a->blend_pipe     = pipe_intern_graphics(d->gpu, vs, fs[6], FMT_RGBA16F, FMT_D32F, BLEND_PREMUL);

    a->opaque_color  = attach_clear(a->hdr, 0.30f, 0.42f, 0.60f, 0.0f);
    a->opaque_depth  = attach_depth_clear(a->depth, 1.0f);
    a->accum_color   = attach_clear(a->accum, 0.0f, 0.0f, 0.0f, 0.0f);
    a->reveal_color  = attach_clear(a->reveal, 1.0f, 1.0f, 1.0f, 1.0f);
    a->depth_read    = attach_color(a->depth, ATT_DEPTH_READ);
    a->hdr_keep      = attach_color(a->hdr, ATT_WRITE_KEEP);
    a->layer_color   = attach_clear(a->layer, 0.0f, 0.0f, 0.0f, 0.0f);
    a->peel_depth[0] = attach_depth_clear(a->peel[0], 0.0f);
    a->peel_depth[1] = attach_depth_clear(a->peel[1], 0.0f);
    a->composite_reads[0] = a->accum; a->composite_reads[1] = a->reveal;
    a->peel_reads[0][0] = a->depth; a->peel_reads[0][1] = a->peel[1];
    a->peel_reads[1][0] = a->depth; a->peel_reads[1][1] = a->peel[0];

    for (i = 0u; i < MODES; ++i) a->programs[i] = build(a, i);
    d->program = a->programs[a->mode];
}

static void app_ui(App *a, struct nk_context *ctx) {
    Demo *d = &a->demo;
    if (nk_begin(ctx, "Transparency", nk_rect(10, 10, 320, 220),
                 NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_TITLE)) {
        demo_ui_text(ctx, "%.1f ms   %u transparent objects", (double)d->dt * 1000.0, a->object_count - a->opaque_count);
        demo_ui_choice(ctx, "Method", k_modes, MODES, &a->mode);
        demo_ui_choice(ctx, "Weight", k_weights, WEIGHTS, &a->weight);
        demo_ui_bool(ctx, "Draw in reverse order", &a->reverse);
    }
    nk_end(ctx);
}

#include "check.inc"

static void app_shutdown(App *a) {
    uint32_t i;
    /* demo.c destroys the program it last ran. */
    for (i = 0u; i < MODES; ++i)
        if (a->programs[i] && a->programs[i] != a->demo.program) frame_program_destroy(a->demo.gpu, a->programs[i]);
    demo_mesh_free(&a->mesh);
}

int main(int argc, char **argv) {
    static App app;
    static const DemoOption options[] = {
        {"--mode", .u32 = &app.mode, .lo = 0, .hi = MODES - 1, .help = "0 weighted blended, 1 depth peeled (reference), 2 blended in draw order"},
        {"--weight", .u32 = &app.weight, .lo = 0, .hi = WEIGHTS - 1, .help = "0 by depth (general), 1 none, 2 by depth (fitted to the scene)"},
        {"--reverse", .flag = &app.reverse, .help = "draw the transparent objects in the opposite order"},
        {0}};
    demo_defaults(&app.demo, "oit", "Transparency - weighted blended, against depth peeling");
    app.orbit = (DemoOrbit){.target = {0.0f, 1.0f, 0.0f},   .distance = 6.5f, .yaw = 0.35f, .pitch = 0.32f,
                            .fov_deg = 50.0f, .znear = 0.1f, .zfar = 100.0f, .min_distance = 2.0f, .max_distance = 30.0f};
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
