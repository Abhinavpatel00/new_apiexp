/* gpu_driven — the GPU decides what to draw.
 *
 *   Reset -> CullA (compute) -> DrawA (indirect)
 *     -> depth pyramid (compute, one pass per level)
 *     -> CullB (compute) -> DrawB (indirect) -> Present
 *
 * 4 096 buildings around a plaza and one ground slab, all boxes. The CPU
 * uploads them once and after that sends only the camera. Each frame two
 * compute passes write two draw lists: what was visible last frame, then
 * what the depth of that reveals as newly visible. See shaders/cull.slang.
 */
#include "demo.h"
#include "mesh.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SHADERS    "demos/gpu_driven/shaders/"
#define CITY       64u
#define COUNT      (CITY * CITY + 1u)
#define SPACING    4.0f
#define MAX_LEVELS 12u
#define DEPTH_BIAS 2e-6f

typedef struct Instance { float centre[4], half[4], color[4]; } Instance;
_Static_assert(sizeof(Instance) == 48, "Instance must match gpu_common.slang");

typedef struct Push {
    float    vp[16];
    uint32_t instances, flags, cmd, list;
    uint32_t count, mode, phase, view;
    uint32_t vertices, levels, width, height;
    float    light[4];
    uint32_t pyramid[12];
} Push;
_Static_assert(sizeof(Push) == 176, "Push must match gpu_common.slang");

typedef struct ReducePush { uint32_t src, dst, src_w, src_h, dst_w, dst_h, pad0, pad1; } ReducePush;

static const char *const k_modes[] = {"frustum + occlusion", "frustum only", "no culling"};
static const char *const k_views[] = {"lit", "instance", "added in phase B"};

typedef struct App {
    Demo       demo; /* first: see demo.h */
    DemoOrbit  orbit;
    Instance  *scene;
    BufSlice   instances, flags, cmd[2], list[2], vertices;
    Target     hdr, depth, pyramid[MAX_LEVELS];
    uint32_t   depth_index, pyramid_index[MAX_LEVELS], level_w[MAX_LEVELS], level_h[MAX_LEVELS], levels;
    Pipe       reset_pipe, cull_pipe, reduce_pipe, draw_pipe;
    AttachDesc color_a, depth_a, color_b, depth_b;
    BufUse     reset_writes[2], cull_reads[1], cull_a_writes[3], cull_b_writes[3], draw_reads[2][4];
    Target     cull_b_reads[MAX_LEVELS];
    Push       push;
    uint32_t   mode, view;
    bool       still, overview;
    uint32_t   step;
} App;

static uint32_t hash32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return x;
}
static float hash01(uint32_t x) { return (float)(hash32(x) >> 8) / 16777216.0f; }

/* A plaza in the middle; low blocks around it; one in sixteen is a tower. */
static void build_scene(Instance *s) {
    uint32_t x, z, n = 0u;
    s[n++] = (Instance){{0.0f, -0.5f, 0.0f, 0.0f}, {CITY * SPACING * 0.6f, 0.5f, CITY * SPACING * 0.6f, 0.0f},
                        {0.16f, 0.17f, 0.17f, 0.0f}};
    for (z = 0u; z < CITY; ++z)
        for (x = 0u; x < CITY; ++x) {
            const uint32_t k  = z * CITY + x;
            const float    cx = ((float)x - CITY * 0.5f + 0.5f) * SPACING, cz = ((float)z - CITY * 0.5f + 0.5f) * SPACING;
            const bool     plaza = cx * cx + cz * cz < 14.0f * 14.0f;
            const bool     tower = (hash32(k * 7u + 3u) & 15u) == 0u;
            const float    h = plaza ? 0.15f : (tower ? 18.0f + 26.0f * hash01(k * 7u + 4u) : 3.0f + 9.0f * hash01(k * 7u + 5u));
            const float    w = plaza ? 1.9f : 1.1f + 0.6f * hash01(k * 7u + 6u), dd = plaza ? 1.9f : 1.1f + 0.6f * hash01(k * 7u + 1u);
            const float    tint = 0.30f + 0.45f * hash01(k * 7u + 2u), warm = hash01(k * 7u + 8u);
            Instance *i = &s[n++];
            i->centre[0] = cx; i->centre[1] = h * 0.5f; i->centre[2] = cz; i->centre[3] = 0.0f;
            i->half[0] = w; i->half[1] = h * 0.5f; i->half[2] = dd; i->half[3] = 0.0f;
            i->color[0] = tint * (tower ? 0.55f : 0.80f + 0.35f * warm); i->color[1] = tint * (tower ? 0.75f : 0.80f);
            i->color[2] = tint * (tower ? 1.10f : 1.05f - 0.45f * warm); i->color[3] = 0.0f;
        }
}

/* ---- the same frustum test as project() in cull.slang ----------------------- */

static bool cpu_in_frustum(const float *vp, const Instance *b) {
    uint32_t outside = 63u, k;
    for (k = 0u; k < 8u; ++k) {
        const float px = b->centre[0] + ((k & 1u) ? b->half[0] : -b->half[0]);
        const float py = b->centre[1] + ((k & 2u) ? b->half[1] : -b->half[1]);
        const float pz = b->centre[2] + ((k & 4u) ? b->half[2] : -b->half[2]);
        const float cx = vp[0] * px + vp[4] * py + vp[8] * pz + vp[12];
        const float cy = vp[1] * px + vp[5] * py + vp[9] * pz + vp[13];
        const float cz = vp[2] * px + vp[6] * py + vp[10] * pz + vp[14];
        const float cw = vp[3] * px + vp[7] * py + vp[11] * pz + vp[15];
        uint32_t    m  = 0u;
        if (cx < -cw) m |= 1u;
        if (cx > cw) m |= 2u;
        if (cy < -cw) m |= 4u;
        if (cy > cw) m |= 8u;
        if (cz < 0.0f) m |= 16u;
        if (cz > cw) m |= 32u;
        outside &= m;
    }
    return outside == 0u;
}

/* ---- frame ------------------------------------------------------------------ */

static void set_camera(App *a) {
    Demo *d = &a->demo;
    Push *p = &a->push;
    uint32_t i;
    demo_orbit_camera(&a->orbit, (float)d->width / (float)d->height, NULL, NULL, p->vp, NULL);
    p->instances = (uint32_t)a->instances.offset;
    p->flags     = (uint32_t)a->flags.offset;
    p->count     = COUNT;
    p->mode      = a->mode;
    p->view      = a->view;
    p->vertices  = (uint32_t)a->vertices.offset;
    p->levels    = a->levels;
    p->width     = d->width;
    p->height    = d->height;
    p->light[0] = 0.45f; p->light[1] = 0.80f; p->light[2] = 0.40f; p->light[3] = DEPTH_BIAS;
    for (i = 0u; i < a->levels; ++i) p->pyramid[i] = a->pyramid_index[i];
}

static void phase_push(App *a, PassCtx *p, uint32_t phase) {
    Push push  = a->push;
    push.phase = phase;
    push.cmd   = (uint32_t)a->cmd[phase].offset;
    push.list  = (uint32_t)a->list[phase].offset;
    pass_push(p, &push, sizeof push);
}
/* One frame: what changed since the last one, then every pass, in the order app_init listed them. */
/* Level i is made from level i - 1; level 0 from the depth buffer. */
static void reduce(App *a, PassCtx *p, uint32_t i) {
    ReducePush push = {0};
    push.src   = i ? a->pyramid_index[i - 1u] : a->depth_index;
    push.dst   = a->pyramid_index[i];
    push.src_w = i ? a->level_w[i - 1u] : a->demo.width;
    push.src_h = i ? a->level_h[i - 1u] : a->demo.height;
    push.dst_w = a->level_w[i];
    push.dst_h = a->level_h[i];
    pass_push(p, &push, sizeof push);
    pass_dispatch(p, (push.dst_w + 7u) / 8u, (push.dst_h + 7u) / 8u, 1u);
}

/* One frame's passes, with the camera as it is: --check calls this directly. */
static void render(App *a) {
    Demo    *d = &a->demo;
    PassCtx *pass;
    uint32_t phase, level;
    if (!demo_frame_begin(d)) return;
    pass = demo_pass(d, "Reset");
    {
        Push push = a->push;
        push.cmd  = (uint32_t)a->cmd[0].offset;
        push.list = (uint32_t)a->cmd[1].offset; /* compReset: the second command */
        pass_push(pass, &push, sizeof push);
        pass_dispatch(pass, 1u, 1u, 1u);
    }
    /* Phase 0 draws what was visible last frame; the depth pyramid is built from
     * that; phase 1 tests the rest against it and draws what turned up. */
    for (phase = 0u; phase < 2u; ++phase) {
        if (phase == 1u)
            for (level = 0u; level < a->levels; ++level) reduce(a, demo_pass(d, NULL), level);   /* "Pyramid0" .. */
        pass = demo_pass(d, phase ? "CullB" : "CullA");
        phase_push(a, pass, phase);
        pass_dispatch(pass, (COUNT + 63u) / 64u, 1u, 1u);
        pass = demo_pass(d, phase ? "DrawB" : "DrawA");
        phase_push(a, pass, phase);
        pass_draw_indirect(pass, a->cmd[phase], 1u);
    }
    demo_present(d);
    demo_frame_end(d);
}

static void app_frame(App *a) {
    Demo *d = &a->demo;
    if (!d->headless) demo_orbit_input(d, &a->orbit);
    if (!a->still) {
        ++a->step;
        a->orbit.yaw += 0.006f;
    }
    set_camera(a);
    render(a);
}

static void set_overview(App *a, bool overview) {
    a->overview = overview;
    if (overview)
        a->orbit = (DemoOrbit){.target = {0.0f, 0.0f, 0.0f}, .distance = 150.0f, .yaw = 0.6f, .pitch = 0.55f,
                               .fov_deg = 50.0f, .znear = 0.3f, .zfar = 600.0f, .min_distance = 2.0f, .max_distance = 400.0f};
    else /* standing in the plaza, looking across it */
        a->orbit = (DemoOrbit){.target = {0.0f, 3.5f, 0.0f}, .distance = 9.0f, .yaw = 0.6f, .pitch = 0.02f,
                               .fov_deg = 60.0f, .znear = 0.3f, .zfar = 600.0f, .min_distance = 2.0f, .max_distance = 400.0f};
}

static void app_init(App *a) {
    Demo     *d = &a->demo;
    ShaderId  reset_cs, cull_cs, reduce_cs, vs, fs;
    DemoMesh  mesh = {0};
    DemoRange box;
    PassDesc  passes[MAX_LEVELS + 10u];
    uint32_t  count = 0u, i, w, h;
    char      name[32];

    set_overview(a, a->overview);
    a->scene = (Instance *)calloc(COUNT, sizeof *a->scene);
    build_scene(a->scene);
    box = demo_mesh_box(&mesh, 1.0f, 1.0f, 1.0f);
    if (box.count != 36u) demo_fatal(d, "the shaders draw 36 vertices per box");
    a->vertices  = demo_alloc(d, mesh.vertices, (uint64_t)mesh.count * sizeof(DemoVertex));
    a->instances = demo_alloc(d, a->scene, (uint64_t)COUNT * sizeof(Instance));
    a->flags     = demo_alloc(d, NULL, (uint64_t)COUNT * 4u);
    for (i = 0u; i < 2u; ++i) {
        a->cmd[i]  = demo_alloc(d, NULL, 16u);
        a->list[i] = demo_alloc(d, NULL, (uint64_t)COUNT * 4u);
    }
    demo_mesh_free(&mesh);

    a->hdr   = demo_target(d, "gpu_hdr", FMT_RGBA16F, d->width, d->height, false, NULL);
    a->depth = demo_target(d, "gpu_depth", FMT_D32F, d->width, d->height, false, &a->depth_index);
    for (w = d->width, h = d->height, a->levels = 0u; a->levels < MAX_LEVELS && (w > 1u || h > 1u); ++a->levels) {
        w = w > 1u ? w / 2u : 1u;
        h = h > 1u ? h / 2u : 1u;
        snprintf(name, sizeof name, "gpu_pyramid_%u", a->levels);
        a->level_w[a->levels] = w; a->level_h[a->levels] = h;
        a->pyramid[a->levels] = demo_target(d, name, FMT_R32F, w, h, true, &a->pyramid_index[a->levels]);
    }
    if (w > 1u || h > 1u) demo_fatal(d, "the window is too large for %u pyramid levels", MAX_LEVELS);

    reset_cs  = demo_shader(d, SHADERS "reset.comp.spv", STAGE_COMPUTE);
    cull_cs   = demo_shader(d, SHADERS "cull.comp.spv", STAGE_COMPUTE);
    reduce_cs = demo_shader(d, SHADERS "reduce.comp.spv", STAGE_COMPUTE);
    vs        = demo_shader(d, SHADERS "draw.vert.spv", STAGE_VERTEX);
    fs        = demo_shader(d, SHADERS "draw.frag.spv", STAGE_FRAGMENT);
    a->reset_pipe  = pipe_intern_compute(d->gpu, reset_cs, "main");
    a->cull_pipe   = pipe_intern_compute(d->gpu, cull_cs, "main");
    a->reduce_pipe = pipe_intern_compute(d->gpu, reduce_cs, "main");
    a->draw_pipe   = pipe_intern_graphics(d->gpu, vs, fs, FMT_RGBA16F, FMT_D32F, BLEND_OPAQUE);

    a->reset_writes[0] = use_write(a->cmd[0].buffer, a->cmd[0].offset, a->cmd[0].size);
    a->reset_writes[1] = use_write(a->cmd[1].buffer, a->cmd[1].offset, a->cmd[1].size);
    a->cull_reads[0]   = use_read(a->instances.buffer, a->instances.offset, a->instances.size);
    a->cull_reads[0].stages = SH_COMP;
    for (i = 0u; i < 2u; ++i) {
        BufUse *writes = i ? a->cull_b_writes : a->cull_a_writes;
        writes[0] = use_write(a->cmd[i].buffer, a->cmd[i].offset, a->cmd[i].size);
        writes[1] = use_write(a->list[i].buffer, a->list[i].offset, a->list[i].size);
        writes[2] = use_write(a->flags.buffer, a->flags.offset, a->flags.size); /* A only reads it */
        a->draw_reads[i][0] = use_read(a->instances.buffer, a->instances.offset, a->instances.size);
        a->draw_reads[i][1] = use_read(a->list[i].buffer, a->list[i].offset, a->list[i].size);
        a->draw_reads[i][2] = use_read(a->vertices.buffer, a->vertices.offset, a->vertices.size);
        a->draw_reads[i][0].stages = a->draw_reads[i][1].stages = a->draw_reads[i][2].stages = SH_VERT;
        a->draw_reads[i][3] = use_indirect(a->cmd[i].buffer, a->cmd[i].offset, a->cmd[i].size);
    }
    for (i = 0u; i < a->levels; ++i) a->cull_b_reads[i] = a->pyramid[i];

    memset(passes, 0, sizeof passes);
    {
        PassDesc *p = &passes[count++];
        strcpy(p->name, "Reset");
        p->kind = PASS_COMPUTE; p->pipe = a->reset_pipe; p->state = draw_state_opaque(SC_1, CULL_NONE);
        p->buf_writes = a->reset_writes; p->buf_write_count = 2u;
    }
    for (i = 0u; i < 2u; ++i) {
        PassDesc *p;
        if (i == 1u) {
            uint32_t l;
            for (l = 0u; l < a->levels; ++l) {
                p = &passes[count++];
                snprintf(p->name, sizeof p->name, "Pyramid%u", l);
                p->kind = PASS_COMPUTE; p->pipe = a->reduce_pipe; p->state = draw_state_opaque(SC_1, CULL_NONE);
                p->tex_reads  = l ? &a->pyramid[l - 1u] : &a->depth; p->tex_read_count = 1u;
                p->tex_writes = &a->pyramid[l];                      p->tex_write_count = 1u;
            }
        }
        p = &passes[count++];
        strcpy(p->name, i ? "CullB" : "CullA");
        p->kind = PASS_COMPUTE; p->pipe = a->cull_pipe; p->state = draw_state_opaque(SC_1, CULL_NONE);
        p->buf_reads  = a->cull_reads;                            p->buf_read_count = 1u;
        p->buf_writes = i ? a->cull_b_writes : a->cull_a_writes;  p->buf_write_count = 3u;
        if (i) { p->tex_reads = a->cull_b_reads; p->tex_read_count = a->levels; }

        p = &passes[count++];
        if (i == 0u) {
            a->color_a = attach_clear(a->hdr, 0.30f, 0.45f, 0.75f, 0.0f);
            a->depth_a = attach_depth_clear(a->depth, 1.0f);
        } else {
            a->color_b = attach_color(a->hdr, ATT_WRITE_KEEP);
            a->depth_b = attach_color(a->depth, ATT_DEPTH_WRITE);
        }
        strcpy(p->name, i ? "DrawB" : "DrawA");
        p->kind  = PASS_GRAPHICS; p->pipe = a->draw_pipe;
        p->state = draw_state_opaque(SC_1, CULL_BACK);
        p->state.depth_cmp = CMP_LESS; p->state.depth_write = 1u;
        p->colors = i ? &a->color_b : &a->color_a; p->color_count = 1u;
        p->depth  = i ? &a->depth_b : &a->depth_a;
        p->buf_reads = a->draw_reads[i]; p->buf_read_count = 4u;
    }
    passes[count++] = demo_present_pass(d, a->hdr);
    d->program = demo_program(d, passes, count);
    if (d->headless && !d->frames) d->frames = 30u;
}

static void app_ui(App *a, struct nk_context *ctx) {
    Demo *d = &a->demo;
    if (nk_begin(ctx, "GPU driven", nk_rect(10, 10, 300, 220),
                 NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_TITLE)) {
        uint32_t i, in_frustum = 0u;
        for (i = 0u; i < COUNT; ++i) in_frustum += cpu_in_frustum(a->push.vp, &a->scene[i]);
        demo_ui_text(ctx, "%.1f ms   %u boxes, %u in the frustum (CPU count)", (double)d->dt * 1000.0, COUNT, in_frustum);
        demo_ui_choice(ctx, "Culling", k_modes, 3u, &a->mode);
        demo_ui_choice(ctx, "View", k_views, 3u, &a->view);
        demo_ui_bool(ctx, "Still", &a->still);
    }
    nk_end(ctx);
}

#include "check.inc"

static void app_shutdown(App *a) { free(a->scene); }

int main(int argc, char **argv) {
    static App app;
    static const DemoOption options[] = {
        {"--mode", .u32 = &app.mode, .lo = 0, .hi = 2, .help = "0 frustum + occlusion, 1 frustum only, 2 no culling"},
        {"--view", .u32 = &app.view, .lo = 0, .hi = 2, .help = "0 lit, 1 instance, 2 what phase B added"},
        {"--overview", .flag = &app.overview, .help = "camera above the city instead of in the plaza"},
        {"--still", .flag = &app.still, .help = "do not turn the camera"},
        {0}};
    demo_defaults(&app.demo, "gpu_driven", "GPU driven - compute culling, depth pyramid, indirect draws");
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
