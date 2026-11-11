/* deferred — a thousand point lights, clustered.
 *
 *   GBuffer (albedo RGBA8 sRGB, normal R32_UINT, material RGBA8, D32F)
 *     -> ClusterLights (compute: light lists per frustum cluster)
 *     -> Lighting (full screen, RGBA16F)
 *     -> Present
 *
 * A second frame program, Forward -> Present, shades the same geometry
 * straight from the vertex attributes with a loop over every light. It is
 * the reference --check compares against, and what `--forward` shows.
 *
 * First demo with several colour attachments (PipeKey.color_count), an
 * integer render target, position rebuilt from the depth buffer, and light
 * lists in the storage buffer read back with readback_request_buffer().
 */
#include "demo.h"
#include "mesh.h"

#include "mu_math.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SHADERS    "demos/deferred/shaders/"
#define MAX_OBJS   96u
#define MAX_LIGHTS 4096u
#define GRID_X     16u
#define GRID_Y     9u
#define GRID_Z     24u
#define CLUSTERS   (GRID_X * GRID_Y * GRID_Z)
#define CAPACITY   511u                       /* light indices per cluster   */
#define RECORD     ((CAPACITY + 1u) * 4u)     /* bytes: count + the indices  */
#define CLUSTER_NEAR 0.5f
#define AREA       21.0f                      /* lights live in +-AREA       */

typedef struct FrameData {
    float    view_proj[16], inv_view_proj[16], view[16];
    float    eye[4], forward[4], proj[4], size[4];
    uint32_t grid[4], data[4], gbuffer[4];
    float    moon[4];
} FrameData;
_Static_assert(sizeof(FrameData) == 320, "FrameData must match deferred_common.slang");

typedef struct Light { float pos_radius[4], color[4]; } Light;
_Static_assert(sizeof(Light) == 32, "Light must match LIGHT_SIZE in deferred_common.slang");

typedef struct LightSeed { float x, z, height, radius, orbit, speed, phase; } LightSeed;

typedef struct DrawPush { uint32_t vertex_offset, object_offset, frame_offset, view, brute, pad[3]; } DrawPush;
typedef struct ClusterPush { uint32_t frame_offset, pad[3]; } ClusterPush;

typedef struct App {
    Demo       demo; /* first: see demo.h */
    DemoOrbit  orbit;
    DemoMesh   mesh;
    DemoObject objects[MAX_OBJS];
    DemoRange  ranges[MAX_OBJS];
    uint32_t   object_count;
    BufSlice   vertices, object_data, frame_data, light_data, cluster_data;
    Target     albedo, normal, material, depth, hdr;
    uint32_t   albedo_index, normal_index, material_index, depth_index;
    Pipe       gbuffer_pipe, cluster_pipe, lighting_pipe, forward_pipe;
    AttachDesc gbuffer_color[3], gbuffer_depth, lighting_color, forward_color, forward_depth;
    Target     lighting_reads[4];
    BufUse     geometry_reads[3], cluster_reads[2], cluster_write, lighting_buf_reads[3], forward_reads[4];
    FrameProgram *deferred_program, *forward_program;
    FrameData  frame;
    LightSeed  seeds[MAX_LIGHTS];
    Light      lights[MAX_LIGHTS];
    /* parameters */
    uint32_t light_count, view;
    float    intensity, ambient, moon;
    bool     brute, forward, standard_depth;
} App;

static const char *const k_views[] = {"lit", "albedo", "normal", "roughness / metalness", "depth",
                                      "lights per cluster", "clusters"};
#define VIEW_COUNT 7u

/* ---- deterministic pseudo-random numbers ---------------------------------- */

static uint32_t pcg(uint32_t v) {
    uint32_t s = v * 747796405u + 2891336453u;
    uint32_t w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
    return (w >> 22u) ^ w;
}
static float rnd(uint32_t index, uint32_t channel) {
    return (float)(pcg(index * 16u + channel) >> 8) * (1.0f / 16777216.0f);
}

/* A saturated colour from a hue in [0, 1). */
static void hue(float h, float *rgb) {
    uint32_t i;
    for (i = 0u; i < 3u; ++i) {
        float t = h + (float)i / 3.0f;
        t -= floorf(t);
        t = fabsf(t * 6.0f - 3.0f) - 1.0f;
        rgb[i] = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    }
}

/* ---- per frame ------------------------------------------------------------ */

/* Inverse of a 4x4 in double precision (Gauss-Jordan, partial pivoting): the
 * lighting pass un-projects every pixel through this matrix. */
static void invert(const float *m, float *out) {
    double   a[4][8];
    uint32_t r, c, k;
    for (r = 0u; r < 4u; ++r)
        for (c = 0u; c < 4u; ++c) {
            a[r][c]      = (double)m[c * 4u + r];
            a[r][c + 4u] = r == c ? 1.0 : 0.0;
        }
    for (k = 0u; k < 4u; ++k) {
        uint32_t best = k;
        double   pivot;
        for (r = k + 1u; r < 4u; ++r)
            if (fabs(a[r][k]) > fabs(a[best][k])) best = r;
        for (c = 0u; c < 8u; ++c) { const double t = a[k][c]; a[k][c] = a[best][c]; a[best][c] = t; }
        pivot = a[k][k];
        for (c = 0u; c < 8u; ++c) a[k][c] /= pivot;
        for (r = 0u; r < 4u; ++r) {
            const double f = a[r][k];
            if (r == k) continue;
            for (c = 0u; c < 8u; ++c) a[r][c] -= f * a[k][c];
        }
    }
    for (r = 0u; r < 4u; ++r)
        for (c = 0u; c < 4u; ++c) out[c * 4u + r] = (float)a[r][c + 4u];
}

/* ---- passes --------------------------------------------------------------- */

static void draw_objects(App *a, PassCtx *p) {
    DrawPush push;
    uint32_t i;
    memset(&push, 0, sizeof push);
    push.frame_offset = (uint32_t)a->frame_data.offset;
    push.view         = a->view;
    push.brute        = 1u;
    for (i = 0u; i < a->object_count; ++i) {
        push.vertex_offset = (uint32_t)a->vertices.offset + a->ranges[i].first * (uint32_t)sizeof(DemoVertex);
        push.object_offset = (uint32_t)a->object_data.offset + i * (uint32_t)sizeof(DemoObject);
        pass_push(p, &push, sizeof push);
        pass_draw(p, a->ranges[i].count, 1u, 0u, 0u);
    }
}

/* One frame: what changed since the last one, then every pass, in the order app_init listed them. */
static void app_frame(App *a) {
    Demo      *d = &a->demo;
    FrameData *f = &a->frame;
    float      eye[3], proj[16];
    const float aspect = (float)d->width / (float)d->height;
    const float tan_y  = tanf(a->orbit.fov_deg * 0.017453292f * 0.5f);
    uint32_t    i;
    PassCtx *pass;

    demo_orbit_input(d, &a->orbit);
    demo_orbit_camera(&a->orbit, aspect, f->view, proj, NULL, eye);
    /* Reversed depth: 1 at the near plane, 0 at the far plane. A float has
     * most of its precision near 0, and 1/z puts most of the scene there -
     * with the usual 0..1 depth the far half of this scene shares the last
     * few thousand float values below 1. The lighting pass rebuilds every
     * position from this buffer, so its precision is the precision of the
     * image. (--standard-depth shows the difference; README, "Depth".) */
    if (!a->standard_depth) {
        const float n = a->orbit.znear, fz = a->orbit.zfar;
        proj[10] = n / (fz - n);
        proj[14] = fz * n / (fz - n);
    }
    mu_mat4_mul(proj, f->view, f->view_proj);
    invert(f->view_proj, f->inv_view_proj);
    for (i = 0u; i < 3u; ++i) {
        f->eye[i]     = eye[i];
        f->forward[i] = -f->view[i * 4u + 2u];
    }
    f->eye[3]     = 1.0f;
    f->forward[3] = a->ambient;
    f->proj[0] = tan_y * aspect; f->proj[1] = tan_y;
    f->proj[2] = CLUSTER_NEAR;   f->proj[3] = a->orbit.zfar;
    f->size[0] = (float)d->width; f->size[1] = (float)d->height;
    f->size[2] = a->standard_depth ? 1.0f : 0.0f; /* the depth of the sky */
    f->grid[0] = GRID_X; f->grid[1] = GRID_Y; f->grid[2] = GRID_Z; f->grid[3] = CAPACITY;
    f->data[0] = (uint32_t)a->light_data.offset;
    f->data[1] = a->light_count;
    f->data[2] = (uint32_t)a->cluster_data.offset;
    f->gbuffer[0] = a->albedo_index; f->gbuffer[1] = a->normal_index; f->gbuffer[2] = a->depth_index;
    f->gbuffer[3] = a->material_index;
    f->moon[0] = 0.35f; f->moon[1] = 0.80f; f->moon[2] = 0.48f; /* unit length to 3 digits */
    f->moon[3] = a->moon;

    /* Each light circles its seed position and bobs a little. */
    for (i = 0u; i < a->light_count; ++i) {
        const LightSeed *s = &a->seeds[i];
        const float      t = d->time * s->speed + s->phase;
        float            rgb[3];
        Light           *l = &a->lights[i];
        hue(rnd(i, 7u), rgb);
        l->pos_radius[0] = s->x + cosf(t) * s->orbit;
        l->pos_radius[1] = s->height + 0.15f * sinf(t * 1.7f);
        l->pos_radius[2] = s->z + sinf(t) * s->orbit;
        l->pos_radius[3] = s->radius;
        /* A little white in every colour, so no light is invisible on a
         * surface of the complementary colour. */
        l->color[0] = (0.15f + 0.85f * rgb[0]) * a->intensity;
        l->color[1] = (0.15f + 0.85f * rgb[1]) * a->intensity;
        l->color[2] = (0.15f + 0.85f * rgb[2]) * a->intensity;
        l->color[3] = 0.0f;
    }
    demo_upload(d, a->frame_data, f, sizeof *f);
    if (a->light_count)
        demo_upload(d, (BufSlice){a->light_data.buffer, a->light_data.offset, (uint64_t)a->light_count * sizeof(Light)},
                    a->lights, (size_t)a->light_count * sizeof(Light));
    d->program = a->forward ? a->forward_program : a->deferred_program;
    /* The debug views are display colours, not light: no tone mapping. */
    d->present.tonemap = (a->view && !a->forward) ? DEMO_TONEMAP_NONE : DEMO_TONEMAP_ACES_FITTED;

    if (!demo_frame_begin(d)) return;
    if (d->program == a->forward_program) {
        draw_objects(a, demo_pass(d, "Forward"));
    } else {
        draw_objects(a, demo_pass(d, "GBuffer"));
        pass = demo_pass(d, "ClusterLights");
        {
            ClusterPush push;
            memset(&push, 0, sizeof push);
            push.frame_offset = (uint32_t)a->frame_data.offset;
            pass_push(pass, &push, sizeof push);
            pass_dispatch(pass, (CLUSTERS + 63u) / 64u, 1u, 1u);
        }
        pass = demo_pass(d, "Lighting");
        {
            DrawPush push;
            memset(&push, 0, sizeof push);
            push.frame_offset = (uint32_t)a->frame_data.offset;
            push.view         = a->view;
            push.brute        = a->brute ? 1u : 0u;
            pass_push(pass, &push, sizeof push);
            pass_draw_fullscreen_triangle(pass, d->width, d->height);
        }
    }
    demo_present(d);
    demo_frame_end(d);
}

/* ---- scene ---------------------------------------------------------------- */

/* Roughness and metalness are stored in 8 bits by the G-buffer; author them
 * in 8 bits and the forward reference sees the same material. */
static float q8(float v) { return floorf(v * 255.0f + 0.5f) / 255.0f; }

static void add(App *a, DemoRange range, float x, float y, float z, float yaw, const float *rgb,
                float roughness, float metalness) {
    DemoObject *o;
    if (a->object_count >= MAX_OBJS) return;
    a->ranges[a->object_count] = range;
    o = &a->objects[a->object_count++];
    demo_object(o, x, y, z, yaw, 1.0f, rgb[0], rgb[1], rgb[2]);
    o->params[0] = q8(roughness);
    o->params[1] = q8(metalness);
}

static void build_scene(App *a) {
    DemoMesh       *m      = &a->mesh;
    const DemoRange ground = demo_mesh_plane(m, 40.0f);
    const DemoRange box    = demo_mesh_box(m, 0.7f, 0.7f, 0.7f);
    const DemoRange pillar = demo_mesh_box(m, 0.35f, 1.6f, 0.35f);
    const DemoRange sphere = demo_mesh_sphere(m, 0.8f, 40u, 20u);
    const DemoRange torus  = demo_mesh_torus(m, 0.85f, 0.28f, 48u, 16u);
    static const float grey[3] = {0.42f, 0.42f, 0.44f};
    const int   half = 4;
    int         ix, iz;
    add(a, ground, 0, 0, 0, 0, grey, 0.75f, 0.0f);
    for (iz = -half; iz < half; ++iz)
        for (ix = -half; ix < half; ++ix) {
            const uint32_t id   = (uint32_t)((iz + 8) * 16 + ix + 8);
            const float    x    = ((float)ix + 0.5f) * 4.4f + (rnd(id, 0u) - 0.5f) * 1.2f;
            const float    z    = ((float)iz + 0.5f) * 4.4f + (rnd(id, 1u) - 0.5f) * 1.2f;
            const float    yaw  = rnd(id, 2u) * 6.2831853f;
            const bool     metal = rnd(id, 3u) < 0.35f;
            const float    rough = 0.3f + 0.55f * rnd(id, 4u);
            float          rgb[3];
            hue(rnd(id, 5u), rgb);
            rgb[0] = 0.25f + 0.6f * rgb[0]; rgb[1] = 0.25f + 0.6f * rgb[1]; rgb[2] = 0.25f + 0.6f * rgb[2];
            if (metal) { rgb[0] = 0.5f + 0.5f * rgb[0]; rgb[1] = 0.5f + 0.5f * rgb[1]; rgb[2] = 0.5f + 0.5f * rgb[2]; }
            switch (pcg(id * 16u + 6u) % 4u) {
            case 0u: add(a, box, x, 0.7f, z, yaw, rgb, rough, metal ? 1.0f : 0.0f); break;
            case 1u: add(a, sphere, x, 0.8f, z, yaw, rgb, rough, metal ? 1.0f : 0.0f); break;
            case 2u: add(a, torus, x, 1.13f, z, yaw, rgb, rough, metal ? 1.0f : 0.0f); break;
            default: add(a, pillar, x, 1.6f, z, yaw, rgb, rough, metal ? 1.0f : 0.0f); break;
            }
        }
}

static void seed_lights(App *a) {
    uint32_t i;
    for (i = 0u; i < MAX_LIGHTS; ++i) {
        LightSeed *s = &a->seeds[i];
        s->x      = (rnd(i, 0u) * 2.0f - 1.0f) * AREA;
        s->z      = (rnd(i, 1u) * 2.0f - 1.0f) * AREA;
        s->height = 0.35f + 1.9f * rnd(i, 2u);
        s->radius = 1.4f + 1.4f * rnd(i, 3u);
        s->orbit  = 0.3f + 1.2f * rnd(i, 4u);
        s->speed  = (0.3f + 0.9f * rnd(i, 5u)) * (pcg(i * 16u + 8u) & 1u ? 1.0f : -1.0f);
        s->phase  = rnd(i, 6u) * 6.2831853f;
    }
}

/* ---- setup ---------------------------------------------------------------- */

static void app_init(App *a) {
    Demo     *d = &a->demo;
    ShaderId  main_vs, gbuffer_fs, lighting_fs, forward_fs, cluster_cs;
    PipeKey   key;
    PassDesc  passes[6];
    uint32_t  count = 0u, i;
    const float   far_depth = a->standard_depth ? 1.0f : 0.0f;
    const uint8_t nearer    = a->standard_depth ? CMP_LESS : CMP_GREATER;

    build_scene(a);
    seed_lights(a);
    a->vertices     = demo_alloc(d, a->mesh.vertices, (uint64_t)a->mesh.count * sizeof(DemoVertex));
    a->object_data  = demo_alloc(d, a->objects, (uint64_t)a->object_count * sizeof(DemoObject));
    a->frame_data   = demo_alloc(d, NULL, sizeof(FrameData));
    a->light_data   = demo_alloc(d, NULL, (uint64_t)MAX_LIGHTS * sizeof(Light));
    a->cluster_data = demo_alloc(d, NULL, (uint64_t)CLUSTERS * RECORD);

    /* The albedo target is sRGB: its 8 bits are spaced evenly for the eye,
     * so the error it adds to the image has the same size for a dark albedo
     * as for a bright one. (What it costs in this scene: README.) */
    a->albedo   = demo_target(d, "gbuffer_albedo", FMT_RGBA8_SRGB, d->width, d->height, false, &a->albedo_index);
    a->normal   = demo_target(d, "gbuffer_normal", FMT_R32_UINT, d->width, d->height, false, &a->normal_index);
    a->material = demo_target(d, "gbuffer_material", FMT_RGBA8_UNORM, d->width, d->height, false, &a->material_index);
    a->depth    = demo_target(d, "gbuffer_depth", FMT_D32F, d->width, d->height, false, &a->depth_index);
    a->hdr      = demo_target(d, "deferred_hdr", FMT_RGBA16F, d->width, d->height, false, NULL);

    main_vs     = demo_shader(d, SHADERS "main.vert.spv", STAGE_VERTEX);
    gbuffer_fs  = demo_shader(d, SHADERS "gbuffer.frag.spv", STAGE_FRAGMENT);
    lighting_fs = demo_shader(d, SHADERS "lighting.frag.spv", STAGE_FRAGMENT);
    forward_fs  = demo_shader(d, SHADERS "forward.frag.spv", STAGE_FRAGMENT);
    cluster_cs  = demo_shader(d, SHADERS "cluster.comp.spv", STAGE_COMPUTE);

    /* Three colour attachments: pipe_intern_graphics() only knows one, so this
     * pipe is described by its full key. */
    memset(&key, 0, sizeof key);
    key.vs = main_vs;
    key.fs = gbuffer_fs;
    key.color[0]    = FMT_RGBA8_SRGB;
    key.color[1]    = FMT_R32_UINT;
    key.color[2]    = FMT_RGBA8_UNORM;
    key.color_count = 3u;
    key.depth       = FMT_D32F;
    key.blend_fallback   = BLEND_OPAQUE;
    key.blend_static_off = true;   /* an integer attachment cannot blend */
    a->gbuffer_pipe  = pipe_intern(d->gpu, &key);
    a->cluster_pipe  = pipe_intern_compute(d->gpu, cluster_cs, "main");
    a->lighting_pipe = pipe_intern_graphics(d->gpu, d->fullscreen_vs, lighting_fs, FMT_RGBA16F, FMT_NONE, BLEND_OPAQUE);
    a->forward_pipe  = pipe_intern_graphics(d->gpu, main_vs, forward_fs, FMT_RGBA16F, FMT_D32F, BLEND_OPAQUE);

    a->geometry_reads[0] = use_read(a->vertices.buffer, a->vertices.offset, a->vertices.size);
    a->geometry_reads[1] = use_read(a->object_data.buffer, a->object_data.offset, a->object_data.size);
    a->geometry_reads[2] = use_read(a->frame_data.buffer, a->frame_data.offset, a->frame_data.size);
    for (i = 0u; i < 3u; ++i) {
        a->geometry_reads[i].stages = SH_VERT | SH_FRAG;
        a->forward_reads[i]         = a->geometry_reads[i];
    }
    a->forward_reads[3] = use_read(a->light_data.buffer, a->light_data.offset, a->light_data.size);
    a->forward_reads[3].stages = SH_FRAG;
    a->cluster_reads[0] = use_read(a->frame_data.buffer, a->frame_data.offset, a->frame_data.size);
    a->cluster_reads[1] = use_read(a->light_data.buffer, a->light_data.offset, a->light_data.size);
    a->cluster_reads[0].stages = a->cluster_reads[1].stages = SH_COMP;
    a->cluster_write = use_write(a->cluster_data.buffer, a->cluster_data.offset, a->cluster_data.size);
    a->cluster_write.stages = SH_COMP;
    a->lighting_buf_reads[0] = use_read(a->frame_data.buffer, a->frame_data.offset, a->frame_data.size);
    a->lighting_buf_reads[1] = use_read(a->light_data.buffer, a->light_data.offset, a->light_data.size);
    a->lighting_buf_reads[2] = use_read(a->cluster_data.buffer, a->cluster_data.offset, a->cluster_data.size);
    for (i = 0u; i < 3u; ++i) a->lighting_buf_reads[i].stages = SH_FRAG;
    a->lighting_reads[0] = a->albedo;
    a->lighting_reads[1] = a->normal;
    a->lighting_reads[2] = a->depth;
    a->lighting_reads[3] = a->material;

    /* ---- program 1: deferred ---------------------------------------------- */
    memset(passes, 0, sizeof passes);
    {
        PassDesc *p = &passes[count++];
        a->gbuffer_color[0] = attach_clear(a->albedo, 0.0f, 0.0f, 0.0f, 0.0f);
        a->gbuffer_color[1] = attach_clear(a->normal, 0.0f, 0.0f, 0.0f, 0.0f);
        a->gbuffer_color[2] = attach_clear(a->material, 0.0f, 0.0f, 0.0f, 0.0f);
        a->gbuffer_depth    = attach_depth_clear(a->depth, far_depth);
        strcpy(p->name, "GBuffer");
        p->kind  = PASS_GRAPHICS;
        p->pipe  = a->gbuffer_pipe;
        p->state = draw_state_opaque(SC_1, CULL_BACK);
        p->state.depth_cmp   = nearer;
        p->state.depth_write = 1u;
        p->colors         = a->gbuffer_color;
        p->color_count    = 3u;
        p->depth          = &a->gbuffer_depth;
        p->buf_reads      = a->geometry_reads;
        p->buf_read_count = 3u;
    }
    {
        PassDesc *p = &passes[count++];
        strcpy(p->name, "ClusterLights");
        p->kind            = PASS_COMPUTE;
        p->pipe            = a->cluster_pipe;
        p->state           = draw_state_opaque(SC_1, CULL_NONE);
        p->buf_reads       = a->cluster_reads;
        p->buf_read_count  = 2u;
        p->buf_writes      = &a->cluster_write;
        p->buf_write_count = 1u;
    }
    {
        PassDesc *p = &passes[count++];
        a->lighting_color = attach_color(a->hdr, ATT_WRITE);
        strcpy(p->name, "Lighting");
        p->kind  = PASS_GRAPHICS;
        p->pipe  = a->lighting_pipe;
        p->state = draw_state_opaque(SC_1, CULL_NONE);
        p->colors         = &a->lighting_color;
        p->color_count    = 1u;
        p->tex_reads      = a->lighting_reads;
        p->tex_read_count = 4u;
        p->buf_reads      = a->lighting_buf_reads;
        p->buf_read_count = 3u;
    }
    passes[count++] = demo_present_pass(d, a->hdr);
    a->deferred_program = demo_program(d, passes, count);

    /* ---- program 2: the forward reference --------------------------------- */
    memset(passes, 0, sizeof passes);
    count = 0u;
    {
        PassDesc *p = &passes[count++];
        /* The colour the lighting pass gives the sky is a gradient; the
         * reference clears to black and --check compares covered pixels. */
        a->forward_color = attach_clear(a->hdr, 0.0f, 0.0f, 0.0f, 0.0f);
        a->forward_depth = attach_depth_clear(a->depth, far_depth);
        strcpy(p->name, "Forward");
        p->kind  = PASS_GRAPHICS;
        p->pipe  = a->forward_pipe;
        p->state = draw_state_opaque(SC_1, CULL_BACK);
        p->state.depth_cmp   = nearer;
        p->state.depth_write = 1u;
        p->colors         = &a->forward_color;
        p->color_count    = 1u;
        p->depth          = &a->forward_depth;
        p->buf_reads      = a->forward_reads;
        p->buf_read_count = 4u;
    }
    passes[count++] = demo_present_pass(d, a->hdr);
    a->forward_program = demo_program(d, passes, count);
    d->program = a->deferred_program; /* app_frame() picks one of the two every frame */
}

static void app_shutdown(App *a) {
    Demo *d = &a->demo;
    /* The framework destroys d->program; the other one is ours. */
    FrameProgram *other = d->program == a->deferred_program ? a->forward_program : a->deferred_program;
    if (other && other != d->program) frame_program_destroy(d->gpu, other);
    demo_mesh_free(&a->mesh);
}

static void app_ui(App *a, struct nk_context *ctx) {
    Demo *d = &a->demo;
    if (nk_begin(ctx, "Deferred", nk_rect(10, 10, 330, 380),
                 NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_TITLE)) {
        float lights = (float)a->light_count;
        demo_ui_text(ctx, "%.1f ms   %u lights   %u clusters", (double)d->dt * 1000.0, a->light_count, CLUSTERS);
        demo_ui_float(ctx, "Lights", &lights, 0.0f, (float)MAX_LIGHTS);
        a->light_count = (uint32_t)lights;
        demo_ui_choice(ctx, "View", k_views, VIEW_COUNT, &a->view);
        demo_ui_bool(ctx, "Forward reference (all lights)", &a->forward);
        demo_ui_bool(ctx, "Deferred without clusters", &a->brute);
        demo_ui_float(ctx, "Light intensity", &a->intensity, 0.0f, 12.0f);
        demo_ui_float(ctx, "Ambient", &a->ambient, 0.0f, 0.2f);
        demo_ui_float(ctx, "Moon", &a->moon, 0.0f, 2.0f);
    }
    nk_end(ctx);
}

/* ---- acceptance ----------------------------------------------------------- */



/* Render one frame; the HDR target as floats (4 per pixel). */
/* One frame, then the HDR target as floats. */
static float *render_hdr(App *a) {
    app_frame(a);
    return demo_read_floats(&a->demo, a->hdr, (size_t)a->demo.width * a->demo.height * 4u);
}

/* Render one frame; the 8-bit image a viewer would see (4 bytes per pixel). */
/* One frame, then the 8-bit output. */
static uint8_t *render_ldr(App *a) {
    const size_t bytes = (size_t)a->demo.width * a->demo.height * 4u;
    uint8_t     *px    = (uint8_t *)malloc(bytes);
    app_frame(a);
    demo_readback(&a->demo, a->demo.out, px, bytes);
    return px;
}

static void app_check(App *a) {
    Demo        *d      = &a->demo;
    const size_t pixels = (size_t)d->width * d->height;
    static const DemoOrbit k_cameras[2] = {
        {.target = {0.0f, 0.5f, 0.0f}, .distance = 20.0f, .yaw = 0.6f, .pitch = 0.5f,
         .fov_deg = 50.0f, .znear = 0.1f, .zfar = 100.0f},
        /* low and close: long lists in the distance, big pixels up close */
        {.target = {1.0f, 0.9f, -2.0f}, .distance = 5.0f, .yaw = 2.2f, .pitch = 0.06f,
         .fov_deg = 65.0f, .znear = 0.1f, .zfar = 100.0f}};
    static const char *const k_name[2] = {"overview", "low camera"};
    char     label[96];
    uint32_t cam;
    size_t   i;

    d->present.dither = false; /* the 8-bit comparison is about the lighting, not the dither */
    for (cam = 0u; cam < 2u; ++cam) {
        float    *clustered, *brute, *forward;
        uint8_t  *ldr_deferred, *ldr_forward;
        uint32_t *lists;
        double    max_cull = 0.0, max_hdr = 0.0, mean_hdr = 0.0, peak = 0.0;
        size_t    covered = 0u, differing = 0u, off_by_more = 0u;
        uint32_t  max_code = 0u;

        a->orbit = k_cameras[cam];
        a->view  = 0u;

        /* ---- 1. culling loses nothing ------------------------------------- */
        a->forward = false; a->brute = false;
        clustered = render_hdr(a);
        /* the lists that frame was lit with */
        lists = (uint32_t *)malloc((size_t)CLUSTERS * RECORD);
        demo_readback_buffer(d, a->cluster_data, lists, (size_t)CLUSTERS * RECORD);
        ldr_deferred = render_ldr(a);
        a->brute = true;
        brute = render_hdr(a);
        a->forward = true;
        forward = render_hdr(a);
        ldr_forward = render_ldr(a);
        for (i = 0u; i < pixels * 4u; ++i) {
            const double e = fabs((double)clustered[i] - (double)brute[i]);
            if (e > max_cull) max_cull = e;
            if ((i & 3u) != 3u && (double)brute[i] > peak) peak = brute[i];
        }
        snprintf(label, sizeof label, "%s: clustered vs all lights", k_name[cam]);
        demo_check(d, label, max_cull, "== 0 (HDR, every pixel)", max_cull == 0.0);
        snprintf(label, sizeof label, "%s: brightest HDR value", k_name[cam]);
        demo_check(d, label, peak, "> 0.5 (something is lit)", peak > 0.5);

        /* ---- 2. the lists themselves --------------------------------------- */
        {
            uint32_t c, longest = 0u, overflow = 0u, unsorted = 0u, bad = 0u, occupied = 0u;
            double   total = 0.0;
            for (c = 0u; c < CLUSTERS; ++c) {
                const uint32_t *rec = lists + (size_t)c * (CAPACITY + 1u);
                const uint32_t  n   = rec[0] < CAPACITY ? rec[0] : CAPACITY;
                uint32_t        k;
                if (rec[0] > CAPACITY) ++overflow;
                if (rec[0] > longest) longest = rec[0];
                if (rec[0]) ++occupied;
                total += (double)rec[0];
                for (k = 0u; k < n; ++k) {
                    if (rec[1u + k] >= a->light_count) ++bad;
                    if (k && rec[1u + k] <= rec[k]) ++unsorted;
                }
            }
            snprintf(label, sizeof label, "%s: clusters over capacity", k_name[cam]);
            demo_check(d, label, (double)overflow, "== 0", overflow == 0u);
            snprintf(label, sizeof label, "%s: longest list", k_name[cam]);
            demo_check(d, label, (double)longest, "<= 511, < all lights",
                             longest <= CAPACITY && longest < a->light_count);
            snprintf(label, sizeof label, "%s: mean list / all lights", k_name[cam]);
            demo_check(d, label, total / CLUSTERS / (double)a->light_count, "< 0.05 (culling culls)",
                             total / CLUSTERS / (double)a->light_count < 0.05);
            snprintf(label, sizeof label, "%s: clusters with lights", k_name[cam]);
            demo_check(d, label, (double)occupied, "> 100", occupied > 100u);
            snprintf(label, sizeof label, "%s: bad or unordered indices", k_name[cam]);
            demo_check(d, label, (double)(bad + unsorted), "== 0", bad + unsorted == 0u);
        }

        /* ---- 3. deferred equals forward ------------------------------------ */
        for (i = 0u; i < pixels; ++i) {
            uint32_t ch;
            bool     differs = false, more = false;
            if (forward[i * 4u + 3u] < 0.5f) continue; /* sky */
            ++covered;
            for (ch = 0u; ch < 3u; ++ch) {
                const double   e = fabs((double)clustered[i * 4u + ch] - (double)forward[i * 4u + ch]);
                const uint32_t x = ldr_deferred[i * 4u + ch], y = ldr_forward[i * 4u + ch];
                const uint32_t q = x > y ? x - y : y - x;
                if (e > max_hdr) max_hdr = e;
                mean_hdr += e;
                if (q > max_code) max_code = q;
                if (q) differs = true;
                if (q > 1u) more = true;
            }
            if (differs) ++differing;
            if (more) ++off_by_more;
        }
        snprintf(label, sizeof label, "%s: covered pixels", k_name[cam]);
        demo_check(d, label, (double)covered, "> half the image", covered > pixels / 2u);
        snprintf(label, sizeof label, "%s: deferred vs forward, 8 bit", k_name[cam]);
        demo_check(d, label, (double)max_code, "<= 1 code value", max_code <= 1u);
        snprintf(label, sizeof label, "%s:   pixels off by > 1", k_name[cam]);
        demo_check(d, label, (double)off_by_more, "== 0", off_by_more == 0u);
        snprintf(label, sizeof label, "%s:   pixels off by 1", k_name[cam]);
        demo_check(d, label, covered ? (double)differing / (double)covered : 0.0, "fraction, reported", true);
        snprintf(label, sizeof label, "%s:   HDR max difference", k_name[cam]);
        demo_check(d, label, max_hdr, "reported", true);
        snprintf(label, sizeof label, "%s:   HDR mean difference", k_name[cam]);
        demo_check(d, label, covered ? mean_hdr / (double)(covered * 3u) : 0.0, "reported", true);

        free(clustered); free(lists); free(brute); free(forward); free(ldr_deferred); free(ldr_forward);
    }

    /* ---- 4. what the G-buffer loses --------------------------------------- */
    {
        static const char *const k_pack[3] = {"normal error, 16+16 bit (G-buffer)", "normal error, 12+12 bit word",
                                              "normal error, mu_lib word"};
        static const uint32_t    k_pack_view[3] = {10u, 7u, 8u};
        float   *rebuilt, *exact;
        double   worst_pos = 0.0, sum_pos = 0.0;
        size_t   n = 0u;
        uint32_t v;
        a->orbit = k_cameras[0];
        a->brute = false;
        /* Normals: the forward program has the exact normal to compare with. */
        a->forward = true;
        for (v = 0u; v < 3u; ++v) {
            float *px;
            double worst = 0.0;
            a->view = k_pack_view[v];
            px = render_hdr(a);
            for (i = 0u; i < pixels; ++i)
                if (px[i * 4u + 3u] > 0.5f && (double)px[i * 4u] > worst) worst = px[i * 4u];
            free(px);
            if (v == 0u) demo_check(d, k_pack[v], worst, "< 0.0001 (length of n' - n)", worst < 0.0001);
            /* The other two are not pass/fail: they record why the G-buffer
             * is laid out the way it is. See README, "The G-buffer". */
            else demo_check(d, k_pack[v], worst, "reported; see README", true);
        }
        /* Positions: rebuilt from depth against interpolated by the rasterizer. */
        a->view = 9u;
        exact   = render_hdr(a);
        a->forward = false;
        rebuilt = render_hdr(a);
        for (i = 0u; i < pixels; ++i) {
            uint32_t ch;
            if (exact[i * 4u + 3u] < 0.5f) continue;
            for (ch = 0u; ch < 3u; ++ch) {
                double e = fabs((double)rebuilt[i * 4u + ch] - (double)exact[i * 4u + ch]);
                if (e > 0.5) e = 1.0 - e;   /* the values are modulo 1 */
                e /= 16.0;                  /* ... of sixteenths of a metre */
                if (e > worst_pos) worst_pos = e;
                sum_pos += e; ++n;
            }
        }
        free(exact); free(rebuilt);
        demo_check(d, "position from depth: max error, m", worst_pos, "< 0.0005", worst_pos < 0.0005);
        demo_check(d, "position from depth: mean error, m", n ? sum_pos / (double)n : 0.0, "reported", true);
        a->view = 0u;
    }
}

int main(int argc, char **argv) {
    static App app;
    static const DemoOption options[] = {
        {"--lights", .u32 = &app.light_count, .lo = 0, .hi = MAX_LIGHTS, .help = "point lights"},
        {"--view", .u32 = &app.view, .lo = 0, .hi = VIEW_COUNT - 1, .help = "0 lit, 1 albedo, 2 normal, 3 roughness/metalness, 4 depth, 5 lights per cluster, 6 clusters"},
        {"--forward", .flag = &app.forward, .help = "the forward reference: no G-buffer, every light per pixel"},
        {"--brute", .flag = &app.brute, .help = "deferred, but every light per pixel (no clusters)"},
        {"--intensity", .f32 = &app.intensity, .lo = 0, .hi = 1000, .help = "light intensity"},
        {"--standard-depth", .flag = &app.standard_depth, .help = "depth 0 near .. 1 far instead of reversed (loses --check)"},
        {0}};
    demo_defaults(&app.demo, "deferred", "Deferred - clustered point lights");
    app.orbit = (DemoOrbit){.target = {0.0f, 0.5f, 0.0f}, .distance = 20.0f, .yaw = 0.6f, .pitch = 0.5f,
                            .fov_deg = 50.0f, .znear = 0.1f, .zfar = 100.0f,
                            .min_distance = 2.0f, .max_distance = 60.0f};
    app.light_count = 1024u;
    app.intensity   = 2.0f;
    app.ambient     = 0.02f;
    app.moon        = 0.25f;
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
