/* screenspace — ambient occlusion and reflections computed from the depth buffer,
 * each next to something it can be measured against.
 *
 *   GBuffer -> Mirror (the scene reflected in the floor: the reference for reflections)
 *     -> AO -> Blur -> SSR -> Planar -> Composite -> Present
 *
 * See shaders/screenspace.slang.
 */
#include "demo.h"
#include "mesh.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SHADERS  "demos/screenspace/shaders/"
#define MAX_OBJS 32u
enum { FLAG_MIRROR = 1u, FLAG_NO_AO = 2u, FLAG_NO_BLUR = 4u };

typedef struct Push {
    float    vp[16];
    float    eye[4], right[4], up[4], forward[4], size[4], light[4];
    uint32_t vertex_offset, object_offset, flags, samples;
    uint32_t albedo, normal, depth, ao;
    uint32_t ao_raw, ssr, planar, sampler;
    uint32_t mode, view, steps, pad;
} Push;
_Static_assert(sizeof(Push) == 224, "Push must match screenspace.slang");

typedef struct GBuffer { Target albedo, normal, depth, z; uint32_t albedo_index, normal_index, depth_index; } GBuffer;

typedef struct App {
    Demo       demo; /* first: see demo.h */
    DemoOrbit  orbit;
    DemoMesh   mesh;
    DemoObject objects[MAX_OBJS];
    DemoRange  ranges[MAX_OBJS];
    uint32_t   object_count, draw_count;
    BufSlice   vertices, object_data;
    GBuffer    scene, mirror;
    Target     ao_raw, ao, ssr, planar, hdr;
    uint32_t   ao_raw_index, ao_index, ssr_index, planar_index;
    Pipe       scene_pipe, ao_pipe, blur_pipe, ssr_pipe, planar_pipe, composite_pipe;
    AttachDesc scene_colors[3], scene_depth, mirror_colors[3], mirror_depth, ao_color, blur_color, ssr_color,
               planar_color, composite_color;
    BufUse     reads[2];
    Target     ao_reads[2], blur_reads[2], ssr_reads[3], planar_reads[3], composite_reads[6];
    Push       push;
    /* settings */
    uint32_t   mode, view, samples, steps;   /* mode: reflections 0 none, 1 screen space, 2 planar */
    bool       use_ao, blur;
    float      radius, bias, reach, thickness;
} App;

/* ---- scene ------------------------------------------------------------------ */

static void add(App *a, DemoRange range, float x, float y, float z, float yaw, float r, float g, float b, bool mirror) {
    DemoObject *o;
    if (a->object_count >= MAX_OBJS) return;
    a->ranges[a->object_count] = range;
    o = &a->objects[a->object_count++];
    demo_object(o, x, y, z, yaw, 1.0f, r, g, b);
    o->color[3] = mirror ? 1.0f : 0.0f;
}

static void build_scene(App *a) {
    DemoMesh       *m      = &a->mesh;
    const DemoRange floor_ = demo_mesh_plane(m, 14.0f);
    const DemoRange box    = demo_mesh_box(m, 0.7f, 0.7f, 0.7f);
    const DemoRange slab   = demo_mesh_box(m, 1.6f, 0.25f, 0.9f);
    const DemoRange pillar = demo_mesh_box(m, 0.35f, 2.0f, 0.35f);
    const DemoRange wall   = demo_mesh_box(m, 3.0f, 1.2f, 0.2f);
    const DemoRange ball   = demo_mesh_sphere(m, 0.8f, 48u, 24u);
    const DemoRange ring   = demo_mesh_torus(m, 0.9f, 0.3f, 48u, 20u);
    add(a, floor_, 0.0f, 0.0f, 0.0f, 0.0f, 0.34f, 0.36f, 0.40f, true);      /* object 0: the mirror */
    add(a, box, -2.4f, 0.7f, 0.6f, 0.4f, 0.85f, 0.32f, 0.25f, false);
    add(a, box, -1.0f, 0.7f, 0.9f, 0.4f, 0.90f, 0.75f, 0.30f, false);       /* touching the first: a crease */
    add(a, slab, 1.8f, 0.25f, 1.6f, -0.3f, 0.80f, 0.80f, 0.78f, false);
    add(a, ball, 1.8f, 1.3f, 1.6f, 0.0f, 0.30f, 0.60f, 0.90f, false);       /* resting on the slab */
    add(a, ball, 0.2f, 2.3f, -1.2f, 0.0f, 0.90f, 0.40f, 0.70f, false);      /* floating: its underside is only in the mirror */
    add(a, ring, -0.3f, 0.3f, 3.2f, 0.0f, 0.35f, 0.80f, 0.45f, false);
    add(a, pillar, 3.6f, 2.0f, -1.5f, 0.2f, 0.75f, 0.70f, 0.60f, false);
    add(a, pillar, -3.8f, 2.0f, -2.2f, 0.7f, 0.55f, 0.50f, 0.80f, false);
    add(a, wall, 0.0f, 1.2f, -4.2f, 0.0f, 0.82f, 0.80f, 0.76f, false);
    add(a, box, 4.2f, 0.7f, 2.8f, 1.1f, 0.30f, 0.75f, 0.75f, false);
    a->draw_count = a->object_count;
}

/* ---- frame ------------------------------------------------------------------ */

static void draw(App *a, PassCtx *p, uint32_t first, uint32_t flags) {
    Push     push = a->push;
    uint32_t i;
    push.flags |= flags;
    for (i = first; i < a->draw_count; ++i) {
        push.vertex_offset = (uint32_t)a->vertices.offset + a->ranges[i].first * (uint32_t)sizeof(DemoVertex);
        push.object_offset = (uint32_t)a->object_data.offset + i * (uint32_t)sizeof(DemoObject);
        pass_push(p, &push, sizeof push);
        pass_draw(p, a->ranges[i].count, 1u, 0u, 0u);
    }
}
static void fullscreen(App *a, PassCtx *p, bool mirrored) {
    Push push = a->push;
    if (mirrored) { push.albedo = a->mirror.albedo_index; push.normal = a->mirror.normal_index; push.depth = a->mirror.depth_index; }
    pass_push(p, &push, sizeof push);
    pass_draw_fullscreen_triangle(p, a->demo.width, a->demo.height);
}
/* One frame: what changed since the last one, then every pass, in the order app_init listed them. */
static void app_frame(App *a) {
    Demo       *d = &a->demo;
    Push       *p = &a->push;
    const float aspect = (float)d->width / (float)d->height;
    const float tan_y  = tanf(a->orbit.fov_deg * 0.017453292f * 0.5f);
    float       view[16], eye[3], len;
    uint32_t    i;

    if (!d->headless) demo_orbit_input(d, &a->orbit);
    demo_orbit_camera(&a->orbit, aspect, view, NULL, p->vp, eye);
    for (i = 0u; i < 3u; ++i) {
        p->eye[i]     = eye[i];
        p->right[i]   = view[i * 4u];
        p->up[i]      = view[i * 4u + 1u];
        p->forward[i] = -view[i * 4u + 2u];
    }
    p->eye[3] = a->radius; p->right[3] = tan_y * aspect; p->up[3] = tan_y; p->forward[3] = a->bias;
    p->size[0] = (float)d->width; p->size[1] = (float)d->height; p->size[2] = a->reach; p->size[3] = a->thickness;
    p->light[0] = 0.45f; p->light[1] = 0.80f; p->light[2] = 0.35f;
    len = sqrtf(p->light[0] * p->light[0] + p->light[1] * p->light[1] + p->light[2] * p->light[2]);
    for (i = 0u; i < 3u; ++i) p->light[i] /= len;
    p->light[3] = 1.0f;
    p->flags   = (a->use_ao ? 0u : FLAG_NO_AO) | (a->blur ? 0u : FLAG_NO_BLUR);
    p->samples = a->samples; p->steps = a->steps;
    p->albedo = a->scene.albedo_index; p->normal = a->scene.normal_index; p->depth = a->scene.depth_index;
    p->ao = a->ao_index; p->ao_raw = a->ao_raw_index; p->ssr = a->ssr_index; p->planar = a->planar_index;
    p->sampler = d->linear_clamp_index;
    p->mode = a->mode; p->view = a->view;
    d->present.tonemap = a->view ? DEMO_TONEMAP_NONE : DEMO_TONEMAP_ACES_FITTED;

    if (!demo_frame_begin(d)) return;
    draw(a, demo_pass(d, "GBuffer"), 0u, 0u);
    draw(a, demo_pass(d, "Mirror"), 1u, FLAG_MIRROR);   /* everything but the floor, reflected in it */
    fullscreen(a, demo_pass(d, "AO"), false);
    fullscreen(a, demo_pass(d, "Blur"), false);
    fullscreen(a, demo_pass(d, "SSR"), false);
    fullscreen(a, demo_pass(d, "Planar"), true);
    fullscreen(a, demo_pass(d, "Composite"), false);
    demo_present(d);
    demo_frame_end(d);
}

static void gbuffer(Demo *d, GBuffer *g, const char *name) {
    char label[48];
    snprintf(label, sizeof label, "%s_albedo", name);
    g->albedo = demo_target(d, label, FMT_RGBA16F, d->width, d->height, false, &g->albedo_index);
    snprintf(label, sizeof label, "%s_normal", name);
    g->normal = demo_target(d, label, FMT_RGBA16F, d->width, d->height, false, &g->normal_index);
    snprintf(label, sizeof label, "%s_depth", name);
    g->depth = demo_target(d, label, FMT_R32F, d->width, d->height, false, &g->depth_index);
    snprintf(label, sizeof label, "%s_z", name);
    g->z = demo_target(d, label, FMT_D32F, d->width, d->height, false, NULL);
}

static PassDesc *post(PassDesc *p, const char *name, Pipe pipe, AttachDesc *color, Target *reads, uint32_t read_count) {
    memset(p, 0, sizeof *p);
    strcpy(p->name, name);
    p->kind  = PASS_GRAPHICS;
    p->pipe  = pipe;
    p->state = draw_state_opaque(SC_1, CULL_NONE);
    p->colors = color; p->color_count = 1u;
    p->tex_reads = reads; p->tex_read_count = read_count;
    return p;
}

static void app_init(App *a) {
    static const char *const k_fs[5] = {"ao", "blur", "ssr", "planar", "composite"};
    Demo     *d = &a->demo;
    PassDesc  passes[10];
    PassDesc *p;
    Pipe     *pipes[5];
    ShaderId  vs, scene_fs;
    PipeKey   key;
    uint32_t  count = 0u, i;
    char      path[96];

    build_scene(a);
    a->vertices    = demo_alloc(d, a->mesh.vertices, (uint64_t)a->mesh.count * sizeof(DemoVertex));
    a->object_data = demo_alloc(d, a->objects, (uint64_t)a->object_count * sizeof(DemoObject));
    a->reads[0] = use_read(a->vertices.buffer, a->vertices.offset, a->vertices.size);
    a->reads[1] = use_read(a->object_data.buffer, a->object_data.offset, a->object_data.size);
    a->reads[0].stages = a->reads[1].stages = SH_VERT;

    gbuffer(d, &a->scene, "ss_scene");
    gbuffer(d, &a->mirror, "ss_mirror");
    a->ao_raw = demo_target(d, "ss_ao_raw", FMT_RGBA16F, d->width, d->height, false, &a->ao_raw_index);
    a->ao     = demo_target(d, "ss_ao", FMT_RGBA16F, d->width, d->height, false, &a->ao_index);
    a->ssr    = demo_target(d, "ss_ssr", FMT_RGBA16F, d->width, d->height, false, &a->ssr_index);
    a->planar = demo_target(d, "ss_planar", FMT_RGBA16F, d->width, d->height, false, &a->planar_index);
    a->hdr    = demo_target(d, "ss_hdr", FMT_RGBA16F, d->width, d->height, false, NULL);

    vs       = demo_shader(d, SHADERS "main.vert.spv", STAGE_VERTEX);
    scene_fs = demo_shader(d, SHADERS "scene.frag.spv", STAGE_FRAGMENT);
    memset(&key, 0, sizeof key);
    key.vs = vs; key.fs = scene_fs;
    key.color[0] = FMT_RGBA16F; key.color[1] = FMT_RGBA16F; key.color[2] = FMT_R32F; key.color_count = 3u;
    key.depth = FMT_D32F;
    key.blend_fallback = BLEND_OPAQUE;
    a->scene_pipe = pipe_intern(d->gpu, &key);
    pipes[0] = &a->ao_pipe; pipes[1] = &a->blur_pipe; pipes[2] = &a->ssr_pipe; pipes[3] = &a->planar_pipe;
    pipes[4] = &a->composite_pipe;
    for (i = 0u; i < 5u; ++i) {
        ShaderId fs;
        snprintf(path, sizeof path, SHADERS "%s.frag.spv", k_fs[i]);
        fs = demo_shader(d, path, STAGE_FRAGMENT);
        *pipes[i] = pipe_intern_graphics(d->gpu, d->fullscreen_vs, fs, FMT_RGBA16F, FMT_NONE, BLEND_OPAQUE);
    }

    for (i = 0u; i < 2u; ++i) {
        GBuffer    *g      = i ? &a->mirror : &a->scene;
        AttachDesc *colors = i ? a->mirror_colors : a->scene_colors;
        colors[0] = attach_clear(g->albedo, 0.0f, 0.0f, 0.0f, 0.0f);
        colors[1] = attach_clear(g->normal, 0.0f, 0.0f, 0.0f, 0.0f);
        colors[2] = attach_clear(g->depth, 0.0f, 0.0f, 0.0f, 0.0f);
        *(i ? &a->mirror_depth : &a->scene_depth) = attach_depth_clear(g->z, 1.0f);
        p = &passes[count++];
        memset(p, 0, sizeof *p);
        strcpy(p->name, i ? "Mirror" : "GBuffer");
        p->kind  = PASS_GRAPHICS;
        p->pipe  = a->scene_pipe;
        p->state = draw_state_opaque(SC_1, i ? CULL_FRONT : CULL_BACK);   /* mirroring turns the triangles over */
        p->colors = colors; p->color_count = 3u;
        p->depth  = i ? &a->mirror_depth : &a->scene_depth;
        p->buf_reads = a->reads; p->buf_read_count = 2u;
    }
    a->ao_color        = attach_color(a->ao_raw, ATT_WRITE);
    a->blur_color      = attach_color(a->ao, ATT_WRITE);
    a->ssr_color       = attach_color(a->ssr, ATT_WRITE);
    a->planar_color    = attach_color(a->planar, ATT_WRITE);
    a->composite_color = attach_color(a->hdr, ATT_WRITE);
    a->ao_reads[0] = a->scene.normal; a->ao_reads[1] = a->scene.depth;
    a->blur_reads[0] = a->ao_raw; a->blur_reads[1] = a->scene.depth;
    a->ssr_reads[0] = a->scene.albedo; a->ssr_reads[1] = a->scene.normal; a->ssr_reads[2] = a->scene.depth;
    a->planar_reads[0] = a->mirror.albedo; a->planar_reads[1] = a->mirror.normal; a->planar_reads[2] = a->mirror.depth;
    a->composite_reads[0] = a->scene.albedo; a->composite_reads[1] = a->scene.normal; a->composite_reads[2] = a->scene.depth;
    a->composite_reads[3] = a->ao; a->composite_reads[4] = a->ssr; a->composite_reads[5] = a->planar;
    post(&passes[count++], "AO", a->ao_pipe, &a->ao_color, a->ao_reads, 2u);
    post(&passes[count++], "Blur", a->blur_pipe, &a->blur_color, a->blur_reads, 2u);
    post(&passes[count++], "SSR", a->ssr_pipe, &a->ssr_color, a->ssr_reads, 3u);
    post(&passes[count++], "Planar", a->planar_pipe, &a->planar_color, a->planar_reads, 3u);
    p = post(&passes[count++], "Composite", a->composite_pipe, &a->composite_color, a->composite_reads, 6u);
    (void)p;
    passes[count++] = demo_present_pass(d, a->hdr);
    d->program = demo_program(d, passes, count);
}

static void app_shutdown(App *a) { demo_mesh_free(&a->mesh); }

static void app_ui(App *a, struct nk_context *ctx) {
    static const char *const k_modes[3] = {"none", "screen space", "planar (reference)"};
    static const char *const k_views[3] = {"picture", "occlusion", "reflection"};
    Demo *d = &a->demo;
    if (nk_begin(ctx, "Screen space", nk_rect(10, 10, 330, 330),
                 NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_TITLE)) {
        float samples = (float)a->samples, steps = (float)a->steps;
        demo_ui_text(ctx, "%.1f ms", (double)d->dt * 1000.0);
        demo_ui_choice(ctx, "View", k_views, 3u, &a->view);
        demo_ui_bool(ctx, "Ambient occlusion", &a->use_ao);
        demo_ui_bool(ctx, "Blur it", &a->blur);
        demo_ui_float(ctx, "Samples", &samples, 1.0f, 64.0f);
        demo_ui_float(ctx, "Radius, m", &a->radius, 0.1f, 2.0f);
        demo_ui_choice(ctx, "Reflections", k_modes, 3u, &a->mode);
        demo_ui_float(ctx, "Steps", &steps, 8.0f, 256.0f);
        demo_ui_float(ctx, "Thickness, m", &a->thickness, 0.05f, 3.0f);
        a->samples = (uint32_t)samples; a->steps = (uint32_t)steps;
    }
    nk_end(ctx);
}

#include "check.inc"

int main(int argc, char **argv) {
    static App app;
    static const DemoOption options[] = {
        {"--mode", .u32 = &app.mode, .lo = 0, .hi = 2, .help = "reflections: 0 none, 1 screen space, 2 planar (the reference)"},
        {"--view", .u32 = &app.view, .lo = 0, .hi = 2, .help = "0 picture, 1 occlusion, 2 reflection"},
        {"--no-ao", .off = &app.use_ao, .help = "no ambient occlusion"},
        {"--no-blur", .off = &app.blur, .help = "show the occlusion as sampled"},
        {"--samples", .u32 = &app.samples, .lo = 1, .hi = 1024, .help = "occlusion samples per pixel"},
        {"--radius", .f32 = &app.radius, .lo = 0.001, .hi = 100, .help = "occlusion radius in metres"},
        {"--steps", .u32 = &app.steps, .lo = 1, .hi = 2048, .help = "steps of a reflection ray"},
        {"--thickness", .f32 = &app.thickness, .lo = 0.001, .hi = 100, .help = "how thick things are assumed to be, metres"},
        {0}};
    demo_defaults(&app.demo, "screenspace", "Screen space - ambient occlusion and reflections from the depth buffer");
    app.orbit = (DemoOrbit){.target = {0.3f, 0.9f, 0.5f}, .distance = 9.0f, .yaw = -0.7f, .pitch = 0.16f,
                            .fov_deg = 55.0f, .znear = 0.1f, .zfar = 100.0f, .min_distance = 2.0f, .max_distance = 40.0f};
    app.mode = 1u; app.samples = 16u; app.steps = 96u;
    app.use_ao = true; app.blur = true;
    app.radius = 0.6f; app.bias = 0.03f; app.reach = 12.0f; app.thickness = 0.6f;
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
