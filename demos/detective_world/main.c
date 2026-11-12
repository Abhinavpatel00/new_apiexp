/* detective_world — procedural character and workshop using the lines renderer.
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
#define MAX_OBJS 192u
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
    uint32_t   seed;
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

/* Shared unit primitives, transformed per part. The face and room use only geometry. */
static void part(App *a, DemoObject *objects, DemoRange range,
                 float x, float y, float z, float sx, float sy, float sz,
                 float r, float g, float b) {
    uint32_t i = a->object_count++;
    if (i >= MAX_OBJS) demo_fatal(&a->demo, "too many parts");
    a->ranges[i] = range;
    demo_object(&objects[i], x, y, z, 0.0f, 1.0f, r, g, b);
    objects[i].model[0] *= sx;
    objects[i].model[5] *= sy;
    objects[i].model[10] *= sz;
}

static void app_init(App *a) {
    Demo *d = &a->demo;
    DemoMesh mesh = {0};
    DemoRange plane = demo_mesh_plane(&mesh, 12.0f);
    DemoRange box = demo_mesh_box(&mesh, 1.0f, 1.0f, 1.0f);
    DemoRange ball = demo_mesh_sphere(&mesh, 1.0f, 16u, 10u);
    DemoObject objects[MAX_OBJS];
    PassDesc passes[5];
    uint32_t i;
    static const float coats[4][3] = {{.24f,.35f,.38f},{.39f,.28f,.31f},{.30f,.34f,.25f},{.27f,.29f,.42f}};
    const float *coat = coats[a->seed % 4u];
#define PART(mesh,x,y,z,sx,sy,sz,r,g,b) part(a,objects,mesh,x,y,z,sx,sy,sz,r,g,b)
    /* floor is object zero: the outline pass deliberately excludes it */
    PART(plane,0,0,0,1,1,1,.48f,.56f,.52f);
    /* A cutaway workshop: back wall, trim, window and shelves. */
    PART(box,0,2.3f,-3.1f,4.8f,2.3f,.12f,.74f,.70f,.60f);
    PART(box,0,.14f,-2.94f,4.8f,.14f,.10f,.30f,.25f,.22f);
    PART(box,-3.1f,2.5f,-2.93f,1.05f,.85f,.08f,.30f,.23f,.19f);
    PART(box,-3.1f,2.5f,-2.82f,.90f,.70f,.04f,.51f,.72f,.73f);
    PART(box,-3.1f,2.5f,-2.76f,.04f,.72f,.03f,.29f,.25f,.22f);
    PART(box,-3.1f,2.5f,-2.75f,.92f,.04f,.03f,.29f,.25f,.22f);
    PART(box,2.75f,2.65f,-2.72f,1.30f,.09f,.35f,.34f,.24f,.18f);
    PART(box,2.75f,1.85f,-2.72f,1.30f,.09f,.35f,.34f,.24f,.18f);
    for (i=0;i<7;i++) {
        float x=1.75f+(float)i*.33f;
        PART(box,x,2.88f,-2.64f,.10f,.17f,.12f,.32f+.04f*(float)(i%3),.38f,.33f);
    }
    /* Workbench, legs, tools, paper and stylized clue. */
    PART(box,2.4f,1.05f,-1.5f,1.45f,.13f,.65f,.49f,.31f,.20f);
    PART(box,1.32f,.48f,-1.5f,.10f,.48f,.10f,.31f,.22f,.17f);
    PART(box,3.48f,.48f,-1.5f,.10f,.48f,.10f,.31f,.22f,.17f);
    PART(box,2.4f,1.205f,-1.35f,.38f,.018f,.25f,.90f,.85f,.69f);
    PART(box,3.14f,1.23f,-1.6f,.32f,.025f,.035f,.21f,.22f,.22f);
    PART(box,3.43f,1.23f,-1.6f,.09f,.06f,.07f,.55f,.35f,.19f);
    PART(ball,1.8f,.022f,-.45f,.44f,.015f,.27f,.48f,.09f,.10f);
    /* Character: shoes, trousers, coat, arms, hands and a spherical head. */
    PART(box,-.24f,.14f,.30f,.21f,.14f,.34f,.16f,.16f,.18f);
    PART(box,.24f,.14f,.30f,.21f,.14f,.34f,.16f,.16f,.18f);
    PART(box,-.22f,.53f,.20f,.17f,.38f,.19f,.21f,.24f,.25f);
    PART(box,.22f,.53f,.20f,.17f,.38f,.19f,.21f,.24f,.25f);
    PART(box,0,1.26f,.20f,.49f,.46f,.24f,coat[0],coat[1],coat[2]);
    PART(box,0,1.28f,.455f,.13f,.37f,.025f,.85f,.78f,.63f);
    PART(box,0,1.42f,.49f,.045f,.22f,.02f,.48f,.18f,.15f);
    PART(box,-.62f,1.24f,.20f,.13f,.40f,.16f,coat[0],coat[1],coat[2]);
    PART(box,.62f,1.24f,.20f,.13f,.40f,.16f,coat[0],coat[1],coat[2]);
    PART(ball,-.62f,.78f,.20f,.14f,.14f,.14f,.80f,.56f,.41f);
    PART(ball,.62f,.78f,.20f,.14f,.14f,.14f,.80f,.56f,.41f);
    PART(box,0,1.77f,.20f,.15f,.12f,.14f,.76f,.52f,.38f);
    PART(ball,0,2.17f,.20f,.45f,.46f,.43f,.86f,.65f,.49f);
    /* low-poly hair cap and simple mathematical face */
    PART(ball,0,2.45f,.15f,.46f,.20f,.43f,.25f,.17f,.13f);
    PART(ball,-.16f,2.21f,.588f,.075f,.095f,.045f,.95f,.93f,.84f);
    PART(ball,.16f,2.21f,.588f,.075f,.095f,.045f,.95f,.93f,.84f);
    PART(ball,-.16f,2.21f,.627f,.032f,.045f,.02f,.10f,.12f,.12f);
    PART(ball,.16f,2.21f,.627f,.032f,.045f,.02f,.10f,.12f,.12f);
    PART(box,-.16f,2.34f,.58f,.09f,.018f,.02f,.24f,.16f,.13f);
    PART(box,.16f,2.34f,.58f,.09f,.018f,.02f,.24f,.16f,.13f);
    PART(ball,0,2.12f,.62f,.055f,.05f,.05f,.75f,.48f,.36f);
    PART(box,0,2.02f,.59f,.105f,.013f,.02f,.35f,.18f,.17f);
#undef PART
    a->first = 0u; a->last = a->object_count;
    a->vertices = demo_alloc(d, mesh.vertices, (uint64_t)mesh.count * sizeof(DemoVertex));
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
    if (nk_begin(ctx, "Detective world", nk_rect(10, 10, 320, 200),
                 NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_TITLE)) {
        demo_ui_text(ctx, "%.1f ms", (double)d->dt * 1000.0);
        demo_ui_choice(ctx, "Lines", k_modes, 4u, &a->mode);
        demo_ui_float(ctx, "Width, pixels", &a->width, 1.0f, 8.0f);
        demo_ui_float(ctx, "Crease angle, degrees", &a->crease, 10.0f, 90.0f);
    }
    nk_end(ctx);
}



/* GPU readback confirms that geometry, including more than the floor, was drawn. */
static void app_check(App *a) {
    Demo *d = &a->demo;
    const size_t pixels = (size_t)d->width * d->height;
    bool seen[MAX_OBJS + 1u] = {false};
    size_t covered = 0u;
    uint32_t unique = 0u;
    float *normal;
    app_frame(a);
    normal = demo_read_floats(d, a->normal, pixels * 4u);
    for (size_t i = 0; i < pixels; ++i) {
        uint32_t id = (uint32_t)(normal[4u * i + 3u] + .5f);
        if (id > 0u && id <= a->object_count) { ++covered; seen[id] = true; }
    }
    for (uint32_t i = 1u; i <= a->object_count; ++i) unique += seen[i] ? 1u : 0u;
    demo_check(d, "scene coverage", (double)covered / (double)pixels, "> 0.20", covered > pixels / 5u);
    demo_check(d, "visible parts", (double)unique, ">= 15", unique >= 15u);
    demo_check(d, "generated parts", (double)a->object_count, ">= 35", a->object_count >= 35u);
    free(normal);
}

int main(int argc, char **argv) {
    static App app;
    static const DemoOption options[] = {
        {"--seed", .u32 = &app.seed, .lo = 0, .hi = 1000000, .help = "character coat variation"},
        {"--mode", .u32 = &app.mode, .lo = 0, .hi = 3, .help = "0 no lines, 1 inverted hull, 2 edges found in the picture, 3 both"},
        {"--width", .f32 = &app.width, .lo = 1, .hi = 16, .help = "line width in pixels"},
        {"--crease", .f32 = &app.crease, .lo = 1, .hi = 179, .help = "edges: a line where the normal turns by more than this many degrees"},
        {0}};
    demo_defaults(&app.demo, "detective_world", "Detective World - procedural character and workshop");
    app.orbit = (DemoOrbit){.target = {0.4f, 1.35f, -0.4f}, .distance = 8.5f, .yaw = 0.0f, .pitch = 0.08f,
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
