/* stickman — the procedural miner from demos/stickman/stickman.c, drawn by the
 * lines renderer (demos/lines/shaders/lines.slang):
 *
 *   Scene (colour, normal + object number, depth) -> Hull -> Edges -> Present
 *
 * One object per body part; the pose array is uploaded every frame.
 */
#include "demo.h"
#include "mesh.h"
#include "stickman.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SHADERS "demos/lines/shaders/"
enum { MODE_HULL = 1u, MODE_EDGES = 2u };

typedef struct Push {
    float    vp[16];
    float    eye[4], right[4], up[4], forward[4], size[4], light[4];
    uint32_t vertex_offset, object_offset, object_id, mode;
    uint32_t color, normal, depth, naive;
} Push;
_Static_assert(sizeof(Push) == 192, "Push must match lines.slang");

typedef struct App {
    Demo      demo;
    Stickman  man;
    DemoOrbit orbit;
    BufSlice  vertices, object_data;
    Target    color, normal, linear_depth, z, hdr;
    uint32_t  color_index, normal_index, depth_index;
    Push      push;
    uint32_t  mode;                            /* bit 0 hull, bit 1 edges */
    bool      mining;
    float     time_scale, width, crease;       /* seconds; pixels; degrees */
} App;

static void draw(App *a, PassCtx *p, uint32_t first) {
    Push     push = a->push;
    uint32_t i;
    for (i = first; i < STICK_PART_COUNT; ++i) {
        push.vertex_offset = (uint32_t)a->vertices.offset + a->man.ranges[i].first * (uint32_t)sizeof(DemoVertex);
        push.object_offset = (uint32_t)a->object_data.offset + i * (uint32_t)sizeof(DemoObject);
        push.object_id     = i + 1u;
        pass_push(p, &push, sizeof push);
        pass_draw(p, a->man.ranges[i].count, 1u, 0u, 0u);
    }
}

static void app_frame(App *a) {
    PassCtx    *pass;
    Demo       *d = &a->demo;
    Push       *p = &a->push;
    const float aspect = (float)d->width / (float)d->height;
    const float tan_y  = tanf(a->orbit.fov_deg * 0.017453292f * 0.5f);
    const float time   = d->time * a->time_scale;
    float       view[16], eye[3];
    uint32_t    i;

    if (a->mining) stickman_pose_mine(&a->man, time);
    else           stickman_pose_walk(&a->man, time, 1.0f);
    demo_upload(d, a->object_data, a->man.pose, sizeof a->man.pose);

    if (!d->headless) demo_orbit_input(d, &a->orbit);
    demo_orbit_camera(&a->orbit, aspect, view, NULL, p->vp, eye);
    for (i = 0u; i < 3u; ++i) {
        p->eye[i] = eye[i]; p->right[i] = view[i * 4u]; p->up[i] = view[i * 4u + 1u]; p->forward[i] = -view[i * 4u + 2u];
    }
    p->right[3] = tan_y * aspect; p->up[3] = tan_y;
    p->size[0] = (float)d->width; p->size[1] = (float)d->height; p->size[2] = a->width;
    p->size[3] = cosf(a->crease * 0.017453292f);
    p->light[0] = 0.48f; p->light[1] = 0.72f; p->light[2] = 0.50f;
    p->mode = a->mode; p->naive = 0u;
    p->color = a->color_index; p->normal = a->normal_index; p->depth = a->depth_index;
    d->present.tonemap = DEMO_TONEMAP_NONE;

    if (!demo_frame_begin(d)) return;
    draw(a, demo_pass(d, "Scene"), 0u);
    pass = demo_pass(d, "Hull");
    if (a->mode & MODE_HULL) draw(a, pass, 0u);
    pass = demo_pass(d, "Edges");
    pass_push(pass, &a->push, sizeof a->push);
    pass_draw_fullscreen_triangle(pass, d->width, d->height);
    demo_present(d);
    demo_frame_end(d);
}

static void app_init(App *a) {
    Demo      *d = &a->demo;
    StickmanParams params = stickman_default_params();
    PassDesc   passes[4];

    params.col_body[0] = 0.10f; params.col_body[1] = 0.10f; params.col_body[2] = 0.10f; /* true stickman black */
    stickman_build(&a->man, params, true);

    a->vertices    = demo_alloc(d, a->man.mesh.vertices, (uint64_t)a->man.mesh.count * sizeof(DemoVertex));
    a->object_data = demo_alloc(d, a->man.pose, sizeof a->man.pose);

    a->color        = demo_target(d, "stick_color", FMT_RGBA16F, d->width, d->height, false, &a->color_index);
    a->normal       = demo_target(d, "stick_normal", FMT_RGBA16F, d->width, d->height, false, &a->normal_index);
    a->linear_depth = demo_target(d, "stick_depth", FMT_R32F, d->width, d->height, false, &a->depth_index);
    a->z            = demo_target(d, "stick_z", FMT_D32F, d->width, d->height, false, NULL);
    a->hdr          = demo_target(d, "stick_hdr", FMT_RGBA16F, d->width, d->height, false, NULL);

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
            .colors = (AttachDesc[]){attach_clear(a->color, 0.82f, 0.80f, 0.78f, 1.0f),
                                     attach_clear(a->normal, 0.0f, 0.0f, 0.0f, 0.0f),
                                     attach_clear(a->linear_depth, 0.0f, 0.0f, 0.0f, 0.0f)},
            .color_count = 3u, .depth = (AttachDesc[]){attach_depth_clear(a->z, 1.0f)},
            .buf_reads = geometry, .buf_read_count = 2u};
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
    if (nk_begin(ctx, "Stickman", nk_rect(10, 10, 320, 200),
                 NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_TITLE)) {
        demo_ui_text(ctx, "%.1f ms", (double)d->dt * 1000.0);
        demo_ui_bool(ctx, "Mining", &a->mining);
        demo_ui_float(ctx, "Time scale", &a->time_scale, 0.1f, 3.0f);
        demo_ui_choice(ctx, "Lines", k_modes, 4u, &a->mode);
        demo_ui_float(ctx, "Width, pixels", &a->width, 1.0f, 8.0f);
        demo_ui_float(ctx, "Crease angle, degrees", &a->crease, 10.0f, 90.0f);
    }
    nk_end(ctx);
}

/* The normal target carries the object number in alpha: count the parts that survived. */
static void app_check(App *a) {
    Demo    *d = &a->demo;
    size_t   pixels = (size_t)d->width * d->height;
    bool     seen[STICK_PART_COUNT + 1u] = {false};
    size_t   covered = 0u;
    uint32_t unique = 0u;
    float   *normal;
    app_frame(a);
    normal = demo_read_floats(d, a->normal, pixels * 4u);
    for (size_t i = 0u; i < pixels; ++i) {
        uint32_t id = (uint32_t)(normal[4u * i + 3u] + .5f);
        if (id > 0u && id <= STICK_PART_COUNT) { ++covered; seen[id] = true; }
    }
    for (uint32_t i = 1u; i <= STICK_PART_COUNT; ++i) unique += seen[i] ? 1u : 0u;
    demo_check(d, "scene coverage", (double)covered / (double)pixels, "> 0.02", covered > pixels / 50u);
    demo_check(d, "visible parts", (double)unique, ">= 9", unique >= 9u);
    demo_check(d, "built vertices", (double)a->man.mesh.count, "2000 < v < 4000",
               a->man.mesh.count > 2000u && a->man.mesh.count < 4000u);
    free(normal);
}

int main(int argc, char **argv) {
    static App app;
    static const DemoOption options[] = {
        {"--mine", .flag = &app.mining, .help = "mining swing instead of walking"},
        {"--speed", .f32 = &app.time_scale, .lo = 0.1, .hi = 3.0, .help = "animation time scale"},
        {"--mode", .u32 = &app.mode, .lo = 0, .hi = 3, .help = "0 no lines, 1 inverted hull, 2 edges, 3 both"},
        {"--width", .f32 = &app.width, .lo = 1, .hi = 16, .help = "line width in pixels"},
        {"--crease", .f32 = &app.crease, .lo = 1, .hi = 179, .help = "edges: line where the normal turns past this many degrees"},
        {0}};
    demo_defaults(&app.demo, "stickman", "Stickman - procedural miner");
    app.orbit = (DemoOrbit){.target = {0.0f, 1.15f, 0.0f}, .distance = 4.4f, .yaw = 0.85f, .pitch = 0.06f,
                            .fov_deg = 45.0f, .znear = 0.1f, .zfar = 40.0f, .min_distance = 1.5f, .max_distance = 12.0f};
    app.mode = MODE_HULL | MODE_EDGES; app.width = 3.0f; app.crease = 40.0f; app.time_scale = 1.0f;
    demo_open(&app.demo, options, argc, argv);
    app_init(&app);
    while (demo_running(&app.demo)) {
        if (app.demo.ui) app_ui(&app, app.demo.ui);
        app_frame(&app);
    }

    if (app.demo.check) app_check(&app);
    stickman_free(&app.man);
    return demo_close(&app.demo);
}
