/* lines — toon outlines two ways: the inverted hull and edges found in the picture.
 *
 *   Scene (colour, normal + object number, depth) -> Hull -> Edges -> Present
 *
 * See shaders/lines.slang. This is the comparison the roadmap's "toon v2" asks for,
 * on its own; the toon demos themselves are not merged.
 */
#include "demo.h"
#include "mesh.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SHADERS  "demos/lines/shaders/"
#define MAX_OBJS 32u
enum { MODE_HULL = 1u, MODE_EDGES = 2u };

typedef struct Push {
    float    vp[16];
    float    eye[4], right[4], up[4], forward[4], size[4], light[4];
    uint32_t vertex_offset, object_offset, object_id, mode;
    uint32_t color, normal, depth, naive;
} Push;
_Static_assert(sizeof(Push) == 192, "Push must match lines.slang");

typedef struct App {
    Demo       demo; /* first: see demo.h */
    DemoOrbit  orbit;
    DemoRange  ranges[MAX_OBJS];
    uint32_t   object_count, first, last;      /* objects [first, last) are drawn */
    BufSlice   vertices, object_data;
    Target     color, normal, linear_depth, z, hdr;
    uint32_t   color_index, normal_index, depth_index;
    Push       push;
    uint32_t   mode;                           /* bit 0 hull, bit 1 edges */
    bool       naive;                          /* --check only */
    float      width, crease;                  /* pixels; degrees */
} App;

static void draw(App *a, PassCtx *p, uint32_t first) {
    Push     push = a->push;
    uint32_t i;
    for (i = first > a->first ? first : a->first; i < a->last; ++i) {
        push.vertex_offset = (uint32_t)a->vertices.offset + a->ranges[i].first * (uint32_t)sizeof(DemoVertex);
        push.object_offset = (uint32_t)a->object_data.offset + i * (uint32_t)sizeof(DemoObject);
        push.object_id     = i + 1u;
        pass_push(p, &push, sizeof push);
        pass_draw(p, a->ranges[i].count, 1u, 0u, 0u);
    }
}
/* One frame: what changed since the last one, then every pass, in the order app_init listed them. */
static void app_frame(App *a) {
    PassCtx *pass;
    Demo       *d = &a->demo;
    Push       *p = &a->push;
    const float aspect = (float)d->width / (float)d->height;
    const float tan_y  = tanf(a->orbit.fov_deg * 0.017453292f * 0.5f);
    float       view[16], eye[3];
    uint32_t    i;
    if (!d->headless) demo_orbit_input(d, &a->orbit);
    demo_orbit_camera(&a->orbit, aspect, view, NULL, p->vp, eye);
    for (i = 0u; i < 3u; ++i) {
        p->eye[i] = eye[i]; p->right[i] = view[i * 4u]; p->up[i] = view[i * 4u + 1u]; p->forward[i] = -view[i * 4u + 2u];
    }
    p->right[3] = tan_y * aspect; p->up[3] = tan_y;
    p->size[0] = (float)d->width; p->size[1] = (float)d->height; p->size[2] = a->width;
    p->size[3] = cosf(a->crease * 0.017453292f);
    p->light[0] = 0.48f; p->light[1] = 0.72f; p->light[2] = 0.50f;
    p->mode = a->mode; p->naive = a->naive ? 1u : 0u;
    p->color = a->color_index; p->normal = a->normal_index; p->depth = a->depth_index;
    d->present.tonemap = DEMO_TONEMAP_NONE;

    if (!demo_frame_begin(d)) return;
    draw(a, demo_pass(d, "Scene"), 0u);
    pass = demo_pass(d, "Hull");
    if (a->mode & MODE_HULL) draw(a, pass, 1u);
    pass = demo_pass(d, "Edges");
    pass_push(pass, &a->push, sizeof a->push);
    pass_draw_fullscreen_triangle(pass, d->width, d->height);
    demo_present(d);
    demo_frame_end(d);
}

static void app_init(App *a) {
    /* Six meshes, eight objects. Object 0 is the floor (never hulled); --check measures line width on object 1. */
    static const struct { uint32_t mesh; float x, y, z, yaw, r, g, b; } k_objects[] = {
        {0u, 0.0f, 0.0f, 0.0f, 0.0f, 0.78f, 0.80f, 0.74f},  {1u, 0.0f, 1.0f, 0.0f, 0.0f, 0.95f, 0.55f, 0.40f},
        {2u, -2.6f, 0.7f, 0.4f, 0.5f, 0.45f, 0.70f, 0.95f}, {2u, -2.2f, 1.75f, 0.5f, 1.1f, 0.95f, 0.85f, 0.45f},
        {3u, 2.4f, 1.6f, -1.0f, 0.3f, 0.60f, 0.85f, 0.60f}, {4u, 2.2f, 0.32f, 1.8f, 0.0f, 0.90f, 0.50f, 0.80f},
        {5u, -0.6f, 0.6f, 2.4f, 0.0f, 0.55f, 0.90f, 0.85f}, {5u, 1.0f, 0.6f, -2.6f, 0.0f, 0.95f, 0.75f, 0.55f}};
    Demo           *d = &a->demo;
    DemoMesh        mesh = {0};
    const DemoRange meshes[6] = {demo_mesh_plane(&mesh, 14.0f),                 demo_mesh_sphere(&mesh, 1.0f, 96u, 48u),
                                 demo_mesh_box(&mesh, 0.7f, 0.7f, 0.7f),        demo_mesh_box(&mesh, 0.35f, 1.6f, 0.35f),
                                 demo_mesh_torus(&mesh, 0.9f, 0.32f, 64u, 24u), demo_mesh_sphere(&mesh, 0.6f, 64u, 32u)};
    DemoObject      objects[MAX_OBJS];
    PassDesc        passes[5];
    uint32_t        i;

    a->object_count = (uint32_t)(sizeof k_objects / sizeof k_objects[0]);
    for (i = 0u; i < a->object_count; ++i) {
        a->ranges[i] = meshes[k_objects[i].mesh];
        demo_object(&objects[i], k_objects[i].x, k_objects[i].y, k_objects[i].z, k_objects[i].yaw, 1.0f,
                    k_objects[i].r, k_objects[i].g, k_objects[i].b);
    }
    a->first = 0u; a->last = a->object_count;
    a->vertices    = demo_alloc(d, mesh.vertices, (uint64_t)mesh.count * sizeof(DemoVertex));
    a->object_data = demo_alloc(d, objects, (uint64_t)a->object_count * sizeof(DemoObject));
    demo_mesh_free(&mesh);

    a->color        = demo_target(d, "lines_color", FMT_RGBA16F, d->width, d->height, false, &a->color_index);
    a->normal       = demo_target(d, "lines_normal", FMT_RGBA16F, d->width, d->height, false, &a->normal_index);
    a->linear_depth = demo_target(d, "lines_depth", FMT_R32F, d->width, d->height, false, &a->depth_index);
    a->z            = demo_target(d, "lines_z", FMT_D32F, d->width, d->height, false, NULL);
    a->hdr          = demo_target(d, "lines_hdr", FMT_RGBA16F, d->width, d->height, false, NULL);

    {
        const BufUse  geometry[2] = {{.slice = a->vertices, .access = ACC_READ, .stages = SH_VERT},
                                     {.slice = a->object_data, .access = ACC_READ, .stages = SH_VERT}};
        const PipeKey scene_key = {.vs = demo_shader(d, SHADERS "main.vert.spv", STAGE_VERTEX),
                                   .fs = demo_shader(d, SHADERS "scene.frag.spv", STAGE_FRAGMENT),
                                   .color = {FMT_RGBA16F, FMT_RGBA16F, FMT_R32F}, .color_count = 3u, .depth = FMT_D32F,
                                   .blend_fallback = BLEND_OPAQUE};
        passes[0] = (PassDesc){
            .name = "Scene", .kind = PASS_GRAPHICS, .state = draw_state_opaque(SC_1, CULL_BACK),
            .pipe = pipe_intern(d->gpu, &scene_key),
            .colors = (AttachDesc[]){attach_clear(a->color, 0.62f, 0.74f, 0.88f, 0.0f), attach_clear(a->normal, 0.0f, 0.0f, 0.0f, 0.0f),
                                     attach_clear(a->linear_depth, 0.0f, 0.0f, 0.0f, 0.0f)},
            .color_count = 3u, .depth = (AttachDesc[]){attach_depth_clear(a->z, 1.0f)},
            .buf_reads = geometry, .buf_read_count = 2u};
        /* Back faces of the swollen object, behind everything already drawn: only the rim survives. */
        passes[1] = (PassDesc){
            .name = "Hull", .kind = PASS_GRAPHICS, .state = draw_state_opaque(SC_1, CULL_FRONT),
            .pipe = pipe_intern_graphics(d->gpu, demo_shader(d, SHADERS "hull.vert.spv", STAGE_VERTEX),
                                         demo_shader(d, SHADERS "hull.frag.spv", STAGE_FRAGMENT), FMT_RGBA16F, FMT_D32F, BLEND_OPAQUE),
            .colors = (AttachDesc[]){attach_color(a->color, ATT_WRITE_KEEP)}, .color_count = 1u,
            .depth = (AttachDesc[]){attach_color(a->z, ATT_DEPTH_WRITE)},
            .buf_reads = geometry, .buf_read_count = 2u};
        passes[2] = (PassDesc){
            .name = "Edges", .kind = PASS_GRAPHICS, .state = draw_state_opaque(SC_1, CULL_NONE),
            .pipe = pipe_intern_graphics(d->gpu, d->fullscreen_vs, demo_shader(d, SHADERS "edges.frag.spv", STAGE_FRAGMENT),
                                         FMT_RGBA16F, FMT_NONE, BLEND_OPAQUE),
            .colors = (AttachDesc[]){attach_color(a->hdr, ATT_WRITE)}, .color_count = 1u,
            .tex_reads = (Target[]){a->color, a->normal, a->linear_depth}, .tex_read_count = 3u};
        passes[3] = demo_present_pass(d, a->hdr);
        d->program = demo_program(d, passes, 4u);
    }
}

static void app_ui(App *a, struct nk_context *ctx) {
    static const char *const k_modes[4] = {"no lines", "inverted hull", "edges in the picture", "both"};
    Demo *d = &a->demo;
    if (nk_begin(ctx, "Lines", nk_rect(10, 10, 320, 200),
                 NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_TITLE)) {
        demo_ui_text(ctx, "%.1f ms", (double)d->dt * 1000.0);
        demo_ui_choice(ctx, "Lines", k_modes, 4u, &a->mode);
        demo_ui_float(ctx, "Width, pixels", &a->width, 1.0f, 8.0f);
        demo_ui_float(ctx, "Crease angle, degrees", &a->crease, 10.0f, 90.0f);
    }
    nk_end(ctx);
}

#include "check.inc"

int main(int argc, char **argv) {
    static App app;
    static const DemoOption options[] = {
        {"--mode", .u32 = &app.mode, .lo = 0, .hi = 3, .help = "0 no lines, 1 inverted hull, 2 edges found in the picture, 3 both"},
        {"--width", .f32 = &app.width, .lo = 1, .hi = 16, .help = "line width in pixels"},
        {"--crease", .f32 = &app.crease, .lo = 1, .hi = 179, .help = "edges: a line where the normal turns by more than this many degrees"},
        {0}};
    demo_defaults(&app.demo, "lines", "Lines - inverted hull and screen-space edges");
    app.orbit = (DemoOrbit){.target = {0.0f, 0.9f, 0.0f}, .distance = 10.0f, .yaw = 0.5f, .pitch = 0.28f,
                            .fov_deg = 45.0f, .znear = 0.1f, .zfar = 100.0f, .min_distance = 2.0f, .max_distance = 40.0f};
    app.mode = MODE_EDGES; app.width = 3.0f; app.crease = 40.0f;
    demo_open(&app.demo, options, argc, argv);
    app_init(&app);
    while (demo_running(&app.demo)) {
        if (app.demo.ui) app_ui(&app, app.demo.ui);
        app_frame(&app);
    }
    if (app.demo.check) app_check(&app);
    return demo_close(&app.demo);
}
