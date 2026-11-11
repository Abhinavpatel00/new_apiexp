/* clustered — Granite-style clustered forward shading.
 *
 *   PrePass (depth only)
 *     -> BinLights (compute: one bit per light per screen tile)
 *     -> Forward (RGBA16F; lights of the pixel's tile AND depth bin)
 *     -> Present
 *   and once, when shadows are switched on: Bake (compute: the scene's
 *   distance function into a 3D texture, which Forward walks towards each
 *   light; shaders/sdf_shadow.slang)
 *
 * The same scene and lights as demos/deferred, plus spot lights, but no
 * G-buffer and no per-cluster light lists. The structure is the "bindless
 * clusterer" of Granite (Arntzen 2020) after Drobot's z-binning: lights
 * sorted by depth on the CPU, a [min, max] light index per depth bin, a bit
 * mask per screen tile, and a bit-scan loop in the fragment shader.
 */
#include "demo.h"
#include "mesh.h"

#include "mu_math.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SHADERS    "demos/clustered/shaders/"
#define MAX_OBJS   96u
#define MAX_LIGHTS 4096u
#define MAX_WORDS  (MAX_LIGHTS / 32u)
#define TILE       32u                       /* pixels                        */
#define ZBINS      1024u
#define AREA       21.0f
/* The distance volume the shadows are read from: 36 x 4 x 36 m around the
 * objects, 12.5 cm voxels. SDF_W/H/D must match SDF_DIMS in sdf_shadow.slang. */
#define SDF_W      288u
#define SDF_H      32u
#define SDF_D      288u
#define SDF_VOXEL  0.125f
#define SDF_X0     (-18.0f)
#define SDF_Y0     (-0.25f)
#define SDF_Z0     (-18.0f)
#define MOON_LIGHT 0xffffffffu
/* Fog: a view-aligned volume, fog.slang. */
#define FROXELS_X  160u
#define FROXELS_Y  90u
#define FROXELS_Z  64u
#define FOG_FAR    60.0f

typedef struct FrameData {
    float    view_proj[16], view[16];
    float    eye[4], forward[4], proj[4], size[4];
    uint32_t grid[4], data[4], data2[4];
    float    moon[4];
    float    sdf_box[4];
    uint32_t sdf_data[4], sdf_ctl[4];
    float    sdf_params[4];
    float    right[4], up[4];
    float    fog[4], fog_params[4];
    uint32_t fog_data[4], fog_scene[4], fog_ref[4];
} FrameData;
_Static_assert(sizeof(FrameData) == 432, "FrameData must match clustered_common.slang");

typedef struct Light { float pos_radius[4], color_inner[4], axis_outer[4]; } Light;
_Static_assert(sizeof(Light) == 48, "Light must match LIGHT_SIZE in clustered_common.slang");

/* A shape of the scene as sdf_shadow.slang's sceneDistance() reads it. */
typedef struct Shape { float centre_bound[4], turn_kind[4], size[4]; } Shape;
_Static_assert(sizeof(Shape) == 48, "Shape must match SHAPE_SIZE in sdf_shadow.slang");
enum { SHAPE_BOX, SHAPE_SPHERE, SHAPE_RING };

typedef struct LightSeed { float x, z, height, radius, orbit, speed, phase, cone, tilt, turn; bool spot; } LightSeed;
typedef struct Sorted { float depth; uint32_t index; } Sorted;

typedef struct DrawPush { uint32_t vertex_offset, object_offset, frame_offset, view, brute, pad[3]; } DrawPush;
typedef struct BinPush { uint32_t frame_offset, pad[3]; } BinPush;

typedef struct App {
    Demo       demo; /* first: see demo.h */
    DemoOrbit  orbit;
    DemoMesh   mesh;
    DemoObject objects[MAX_OBJS];
    DemoRange  ranges[MAX_OBJS];
    uint32_t   object_count;
    BufSlice   vertices, object_data, frame_data, light_data, cull_data, type_data, mask_data, zbin_data, shape_data;
    Target     depth, hdr, sdf_volume, fog_injected, fog_integrated, fogged;
    Pipe       prepass_pipe, bin_pipe, forward_pipe, bake_pipe, inject_pipe, integrate_pipe, fog_pipe;
    uint32_t   hdr_index, depth_index, injected_index, integrated_index;
    BufUse     inject_reads[5], integrate_read, fog_reads[5];
    Target     fog_tex_reads[4];
    AttachDesc fog_color;
    uint32_t   fog_mode, fog_output, fog_steps, fog_stride;   /* mode: 0 off, 1 froxels, 2 marched per pixel */
    float      fog_density, fog_height, fog_ambient, fog_g, fog_albedo;
    bool       fog_brute;
    Shape      shapes[MAX_OBJS];
    uint32_t   shape_count, sdf_index, check_light;
    bool       sdf_shadows, sdf_baked;
    bool       fog_on, fog_reference;   /* --fog, --fog-reference */
    float      lamp_radius, moon_k, sdf_bias;
    BufUse     bake_reads[2];
    AttachDesc prepass_depth, forward_color, forward_depth;
    BufUse     geometry_reads[3], bin_reads[2], bin_write, forward_reads[8];
    FrameData  frame;
    LightSeed  seeds[MAX_LIGHTS];
    Light      lights[MAX_LIGHTS];
    float      cull[MAX_LIGHTS][4];
    Sorted     order[MAX_LIGHTS];
    uint32_t   types[MAX_WORDS];
    uint32_t   zbins[ZBINS][2];
    uint32_t   tiles_x, tiles_y;
    /* parameters */
    uint32_t light_count, view;
    float    intensity, ambient, moon, spot_share;
    bool     brute;
} App;

static const char *const k_views[] = {"lit", "lights tested per pixel", "lights in the tile's mask",
                                      "lights in the depth bin's range", "tested but out of reach"};
#define VIEW_COUNT 5u

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

static int by_depth(const void *x, const void *y) {
    const Sorted *a = (const Sorted *)x, *b = (const Sorted *)y;
    if (a->depth != b->depth) return a->depth < b->depth ? -1 : 1;
    return a->index < b->index ? -1 : a->index > b->index ? 1 : 0;
}

/* One light at time t: its shading record, and the sphere that bounds
 * everything it can light, in view space. */
static void make_light(const App *a, uint32_t i, float t0, const float *view, Light *l, float *cull) {
    const LightSeed *s = &a->seeds[i];
    const float      t = t0 * s->speed + s->phase;
    float            rgb[3], centre[3], radius;
    uint32_t         k;
    hue(rnd(i, 7u), rgb);
    l->pos_radius[0] = s->x + cosf(t) * s->orbit;
    l->pos_radius[1] = s->height + 0.15f * sinf(t * 1.7f);
    l->pos_radius[2] = s->z + sinf(t) * s->orbit;
    l->pos_radius[3] = s->radius;
    for (k = 0u; k < 3u; ++k) {
        l->color_inner[k] = (0.15f + 0.85f * rgb[k]) * a->intensity;
        centre[k]         = l->pos_radius[k];
    }
    radius = s->radius;
    if (s->spot) {
        /* A spot hangs higher, reaches further and points mostly down,
         * swinging around the vertical. */
        const float yaw = s->turn + t * 0.7f, co = cosf(s->cone), si = sinf(s->cone);
        float       reach;
        l->pos_radius[1] += 1.6f;
        l->pos_radius[3]  = s->radius * 2.2f;
        l->axis_outer[0]  = sinf(s->tilt) * cosf(yaw);
        l->axis_outer[1]  = -cosf(s->tilt);
        l->axis_outer[2]  = sinf(s->tilt) * sinf(yaw);
        l->axis_outer[3]  = co;
        l->color_inner[3] = cosf(s->cone * 0.7f);
        for (k = 0u; k < 3u; ++k) l->color_inner[k] *= 2.5f;
        /* Smallest sphere around a cone of length R and half-angle c: below
         * 45 degrees it passes through the apex and the rim, above it is the
         * rim's own circle. */
        reach = l->pos_radius[3];
        if (s->cone < 0.78539816f) { radius = reach / (2.0f * co); }
        else                       { radius = reach * si; }
        for (k = 0u; k < 3u; ++k)
            centre[k] = l->pos_radius[k] + l->axis_outer[k] * (s->cone < 0.78539816f ? radius : reach * co);
    } else {
        l->axis_outer[0] = l->axis_outer[2] = 0.0f;
        l->axis_outer[1] = -1.0f;
        l->axis_outer[3] = -1.0f;
        l->color_inner[3] = -1.0f;
    }
    for (k = 0u; k < 3u; ++k)
        cull[k] = view[k] * centre[0] + view[4u + k] * centre[1] + view[8u + k] * centre[2] + view[12u + k];
    cull[3] = radius;
}

/* ---- passes --------------------------------------------------------------- */

static void draw_objects(App *a, PassCtx *p) {
    DrawPush push;
    uint32_t i;
    memset(&push, 0, sizeof push);
    push.frame_offset = (uint32_t)a->frame_data.offset;
    push.view         = a->view;
    push.brute        = a->brute ? 1u : 0u;
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
    const float bins_per_metre = (float)ZBINS / a->orbit.zfar;
    const uint32_t n = a->light_count, words = (n + 31u) / 32u;
    static Light scratch[MAX_LIGHTS];
    static float scratch_cull[MAX_LIGHTS][4];
    uint32_t    i;
    PassCtx *pass;

    demo_orbit_input(d, &a->orbit);
    demo_orbit_camera(&a->orbit, aspect, f->view, proj, NULL, eye);
    {   /* reversed depth, as in demos/deferred */
        const float zn = a->orbit.znear, zf = a->orbit.zfar;
        proj[10] = zn / (zf - zn);
        proj[14] = zf * zn / (zf - zn);
    }
    mu_mat4_mul(proj, f->view, f->view_proj);
    for (i = 0u; i < 3u; ++i) {
        f->eye[i]     = eye[i];
        f->forward[i] = -f->view[i * 4u + 2u];
        f->right[i]   = f->view[i * 4u];
        f->up[i]      = f->view[i * 4u + 1u];
    }
    f->fog[0] = a->fog_density; f->fog[1] = a->fog_height; f->fog[2] = FOG_FAR; f->fog[3] = a->fog_ambient;
    f->fog_params[0] = a->fog_g; f->fog_params[1] = a->fog_albedo;
    f->fog_params[2] = a->orbit.znear; f->fog_params[3] = a->orbit.zfar;
    f->fog_data[0] = a->injected_index; f->fog_data[1] = a->integrated_index;
    f->fog_data[2] = a->fog_mode; f->fog_data[3] = a->fog_output;
    f->fog_scene[0] = a->hdr_index; f->fog_scene[1] = a->depth_index;
    f->fog_ref[0] = a->fog_steps; f->fog_ref[1] = a->fog_stride; f->fog_ref[2] = a->fog_brute ? 1u : 0u;
    f->forward[3] = a->ambient;
    f->proj[0] = tan_y * aspect; f->proj[1] = tan_y; f->proj[2] = bins_per_metre;
    f->size[0] = (float)d->width; f->size[1] = (float)d->height; f->size[2] = (float)TILE;
    f->grid[0] = a->tiles_x; f->grid[1] = a->tiles_y; f->grid[2] = words; f->grid[3] = ZBINS;
    f->data[0] = (uint32_t)a->light_data.offset;
    f->data[1] = n;
    f->data[2] = (uint32_t)a->mask_data.offset;
    f->data[3] = (uint32_t)a->zbin_data.offset;
    f->data2[0] = (uint32_t)a->cull_data.offset;
    f->data2[1] = (uint32_t)a->type_data.offset;
    f->moon[0] = 0.35f; f->moon[1] = 0.80f; f->moon[2] = 0.48f;
    f->moon[3] = a->moon;
    f->sdf_box[0] = SDF_X0; f->sdf_box[1] = SDF_Y0; f->sdf_box[2] = SDF_Z0; f->sdf_box[3] = SDF_VOXEL;
    f->sdf_data[0] = a->sdf_index;
    f->sdf_data[1] = d->linear_clamp_index;
    f->sdf_data[2] = (uint32_t)a->shape_data.offset;
    f->sdf_data[3] = a->shape_count;
    f->sdf_ctl[0] = a->sdf_shadows ? 1u : 0u;
    f->sdf_ctl[1] = a->check_light;
    f->sdf_params[0] = a->lamp_radius; f->sdf_params[1] = a->moon_k; f->sdf_params[2] = a->sdf_bias;

    /* Lights, then the same lights sorted by the view depth of their
     * bounding sphere: a depth bin then needs only "first and last light". */
    for (i = 0u; i < n; ++i) {
        make_light(a, i, d->time, f->view, &scratch[i], scratch_cull[i]);
        a->order[i].depth = -scratch_cull[i][2];
        a->order[i].index = i;
    }
    qsort(a->order, n, sizeof a->order[0], by_depth);
    memset(a->types, 0, sizeof a->types);
    for (i = 0u; i < ZBINS; ++i) { a->zbins[i][0] = 0xffffffffu; a->zbins[i][1] = 0u; }
    for (i = 0u; i < n; ++i) {
        const uint32_t src   = a->order[i].index;
        const float    depth = a->order[i].depth, r = scratch_cull[src][3];
        a->lights[i] = scratch[src];
        memcpy(a->cull[i], scratch_cull[src], sizeof a->cull[i]);
        if (a->seeds[src].spot) a->types[i >> 5] |= 1u << (i & 31u);
        if (depth + r > 0.0f) {
            /* Every bin the sphere overlaps. floorf of the same product the
             * shader forms, so a pixel can not fall between two bins. */
            const float lo = floorf((depth - r > 0.0f ? depth - r : 0.0f) * bins_per_metre) - 1.0f;
            const float hi = floorf((depth + r) * bins_per_metre) + 1.0f;
            uint32_t    b0 = lo < 0.0f ? 0u : (uint32_t)lo, b1 = hi >= (float)ZBINS ? ZBINS - 1u : (uint32_t)hi, b;
            for (b = b0; b <= b1 && b0 < ZBINS; ++b) {
                if (i < a->zbins[b][0]) a->zbins[b][0] = i;
                if (i > a->zbins[b][1]) a->zbins[b][1] = i;
            }
        }
    }
    demo_upload(d, a->frame_data, f, sizeof *f);
    demo_upload(d, a->zbin_data, a->zbins, sizeof a->zbins);
    demo_upload(d, a->type_data, a->types, sizeof a->types);
    if (n) {
        demo_upload(d, (BufSlice){a->light_data.buffer, a->light_data.offset, (uint64_t)n * sizeof(Light)},
                    a->lights, (size_t)n * sizeof(Light));
        demo_upload(d, (BufSlice){a->cull_data.buffer, a->cull_data.offset, (uint64_t)n * 16u}, a->cull, (size_t)n * 16u);
    }
    d->present.tonemap = a->view ? DEMO_TONEMAP_NONE : DEMO_TONEMAP_ACES_FITTED;

    if (!demo_frame_begin(d)) return;
    {
        /* Every compute and full-screen pass takes the same push constants. */
        const uint32_t bin_threads = a->tiles_x * a->tiles_y * ((a->light_count + 31u) / 32u);
        BinPush push;
        memset(&push, 0, sizeof push);
        push.frame_offset = (uint32_t)a->frame_data.offset;

        /* Once, the first frame shadows are on (the fog's moon shafts need it too).
         * The pass is always in the program. */
        pass = demo_pass(d, "BakeSdf");
        if ((a->sdf_shadows || a->fog_mode) && !a->sdf_baked) {
            pass_push(pass, &push, sizeof push);
            pass_dispatch(pass, SDF_W / 4u, SDF_H / 4u, SDF_D / 4u);
            a->sdf_baked = true;
        }

        draw_objects(a, demo_pass(d, "PrePass"));

        pass = demo_pass(d, "BinLights");
        if (bin_threads) {
            pass_push(pass, &push, sizeof push);
            pass_dispatch(pass, (bin_threads + 63u) / 64u, 1u, 1u);
        }

        draw_objects(a, demo_pass(d, "Forward"));

        pass = demo_pass(d, "FogInject");
        if (a->fog_mode == 1u) {
            pass_push(pass, &push, sizeof push);
            pass_dispatch(pass, FROXELS_X / 4u, (FROXELS_Y + 3u) / 4u, FROXELS_Z / 4u);
        }
        pass = demo_pass(d, "FogIntegrate");
        if (a->fog_mode == 1u) {
            pass_push(pass, &push, sizeof push);
            pass_dispatch(pass, FROXELS_X / 8u, (FROXELS_Y + 7u) / 8u, 1u);
        }

        /* Always runs: without fog it copies the picture. */
        pass = demo_pass(d, "Fog");
        pass_push(pass, &push, sizeof push);
        pass_draw_fullscreen_triangle(pass, d->width, d->height);
    }
    demo_present(d);
    demo_frame_end(d);
}

/* ---- scene ---------------------------------------------------------------- */

/* The scene of demos/deferred, so the two demos can be compared. */
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

static void shape(App *a, int kind, float x, float y, float z, float yaw, float s0, float s1, float s2) {
    Shape *s = &a->shapes[a->shape_count++];
    memset(s, 0, sizeof *s);
    s->centre_bound[0] = x; s->centre_bound[1] = y; s->centre_bound[2] = z;
    s->centre_bound[3] = kind == SHAPE_BOX ? sqrtf(s0 * s0 + s1 * s1 + s2 * s2) : kind == SHAPE_SPHERE ? s0 : s0 + s1;
    s->turn_kind[0] = cosf(yaw); s->turn_kind[1] = sinf(yaw); s->turn_kind[2] = (float)kind;
    s->size[0] = s0; s->size[1] = s1; s->size[2] = s2;
}

/* sceneDistance() of sdf_shadow.slang, on the CPU. */
static float scene_distance(const App *a, float x, float y, float z) {
    float    d = y;
    uint32_t i;
    for (i = 0u; i < a->shape_count; ++i) {
        const Shape *s  = &a->shapes[i];
        const float  qx = x - s->centre_bound[0], qy = y - s->centre_bound[1], qz = z - s->centre_bound[2];
        const float  lx = s->turn_kind[0] * qx - s->turn_kind[1] * qz, lz = s->turn_kind[1] * qx + s->turn_kind[0] * qz;
        float        v;
        if ((int)s->turn_kind[2] == SHAPE_BOX) {
            const float ex = fabsf(lx) - s->size[0], ey = fabsf(qy) - s->size[1], ez = fabsf(lz) - s->size[2];
            const float ox = ex > 0.0f ? ex : 0.0f, oy = ey > 0.0f ? ey : 0.0f, oz = ez > 0.0f ? ez : 0.0f;
            const float in = ex > ey ? (ex > ez ? ex : ez) : (ey > ez ? ey : ez);
            v = sqrtf(ox * ox + oy * oy + oz * oz) + (in < 0.0f ? in : 0.0f);
        } else if ((int)s->turn_kind[2] == SHAPE_SPHERE) {
            v = sqrtf(lx * lx + qy * qy + lz * lz) - s->size[0];
        } else {
            const float ring = sqrtf(lx * lx + lz * lz) - s->size[0];
            v = sqrtf(ring * ring + qy * qy) - s->size[1];
        }
        if (v < d) d = v;
    }
    return d;
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
            /* The same shapes for the distance function; sizes as in the meshes above. */
            switch (pcg(id * 16u + 6u) % 4u) {
            case 0u: shape(a, SHAPE_BOX, x, 0.7f, z, yaw, 0.7f, 0.7f, 0.7f); break;
            case 1u: shape(a, SHAPE_SPHERE, x, 0.8f, z, yaw, 0.8f, 0.0f, 0.0f); break;
            case 2u: shape(a, SHAPE_RING, x, 1.13f, z, yaw, 0.85f, 0.28f, 0.0f); break;
            default: shape(a, SHAPE_BOX, x, 1.6f, z, yaw, 0.35f, 1.6f, 0.35f); break;
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
        s->spot   = rnd(i, 9u) < a->spot_share;
        s->cone   = 0.30f + 0.35f * rnd(i, 10u);   /* half-angle, 17..37 degrees */
        s->tilt   = 0.15f + 0.45f * rnd(i, 11u);
        s->turn   = rnd(i, 12u) * 6.2831853f;
    }
}

/* ---- setup ---------------------------------------------------------------- */

static void app_init(App *a) {
    Demo     *d = &a->demo;
    ShaderId  main_vs, main_fs, bin_cs, bake_cs, inject_cs, integrate_cs, fog_fs;
    PassDesc  passes[10];
    uint32_t  count = 0u, i;
    const BufSlice *all[8];

    if (a->fog_on) a->fog_mode = 1u;
    if (a->fog_reference) a->fog_mode = 2u;

    build_scene(a);
    seed_lights(a);
    a->tiles_x = (d->width + TILE - 1u) / TILE;
    a->tiles_y = (d->height + TILE - 1u) / TILE;
    a->vertices    = demo_alloc(d, a->mesh.vertices, (uint64_t)a->mesh.count * sizeof(DemoVertex));
    a->object_data = demo_alloc(d, a->objects, (uint64_t)a->object_count * sizeof(DemoObject));
    a->frame_data  = demo_alloc(d, NULL, sizeof(FrameData));
    a->light_data  = demo_alloc(d, NULL, (uint64_t)MAX_LIGHTS * sizeof(Light));
    a->cull_data   = demo_alloc(d, NULL, (uint64_t)MAX_LIGHTS * 16u);
    a->type_data   = demo_alloc(d, NULL, sizeof a->types);
    a->mask_data   = demo_alloc(d, NULL, (uint64_t)a->tiles_x * a->tiles_y * MAX_WORDS * 4u);
    a->zbin_data   = demo_alloc(d, NULL, sizeof a->zbins);
    a->shape_data  = demo_alloc(d, a->shapes, (uint64_t)a->shape_count * sizeof(Shape));
    all[0] = &a->vertices; all[1] = &a->object_data; all[2] = &a->frame_data; all[3] = &a->light_data;
    all[4] = &a->cull_data; all[5] = &a->type_data; all[6] = &a->mask_data; all[7] = &a->zbin_data;
    for (i = 0u; i < 8u; ++i)

    a->depth  = demo_target(d, "clustered_depth", FMT_D32F, d->width, d->height, false, &a->depth_index);
    a->hdr    = demo_target(d, "clustered_hdr", FMT_RGBA16F, d->width, d->height, false, &a->hdr_index);
    a->fogged = demo_target(d, "clustered_fogged", FMT_RGBA16F, d->width, d->height, false, NULL);
    a->fog_injected   = demo_volume(d, "fog_injected", FMT_RGBA16F, FROXELS_X, FROXELS_Y, FROXELS_Z, &a->injected_index);
    a->fog_integrated = demo_volume(d, "fog_integrated", FMT_RGBA16F, FROXELS_X, FROXELS_Y, FROXELS_Z, &a->integrated_index);
    a->sdf_volume = demo_volume(d, "clustered_sdf", FMT_RGBA16F, SDF_W, SDF_H, SDF_D, &a->sdf_index);

    main_vs = demo_shader(d, SHADERS "main.vert.spv", STAGE_VERTEX);
    main_fs = demo_shader(d, SHADERS "main.frag.spv", STAGE_FRAGMENT);
    bin_cs  = demo_shader(d, SHADERS "bin.comp.spv", STAGE_COMPUTE);
    bake_cs = demo_shader(d, SHADERS "bake.comp.spv", STAGE_COMPUTE);
    inject_cs    = demo_shader(d, SHADERS "fog_inject.comp.spv", STAGE_COMPUTE);
    integrate_cs = demo_shader(d, SHADERS "fog_integrate.comp.spv", STAGE_COMPUTE);
    fog_fs       = demo_shader(d, SHADERS "fog.frag.spv", STAGE_FRAGMENT);
    a->inject_pipe    = pipe_intern_compute(d->gpu, inject_cs, "main");
    a->integrate_pipe = pipe_intern_compute(d->gpu, integrate_cs, "main");
    a->fog_pipe       = pipe_intern_graphics(d->gpu, d->fullscreen_vs, fog_fs, FMT_RGBA16F, FMT_NONE, BLEND_OPAQUE);
    a->bake_pipe = pipe_intern_compute(d->gpu, bake_cs, "main");
    a->prepass_pipe = pipe_intern_depth_only(d->gpu, main_vs, SHADER_NONE, FMT_D32F);
    a->bin_pipe     = pipe_intern_compute(d->gpu, bin_cs, "main");
    a->forward_pipe = pipe_intern_graphics(d->gpu, main_vs, main_fs, FMT_RGBA16F, FMT_D32F, BLEND_OPAQUE);

    for (i = 0u; i < 3u; ++i) {
        a->geometry_reads[i] = use_read(all[i]->buffer, all[i]->offset, all[i]->size);
        a->geometry_reads[i].stages = SH_VERT;
    }
    /* Forward: geometry, frame, lights, types, masks, z bins (not the cull spheres). */
    for (i = 0u; i < 7u; ++i) {
        const BufSlice *s = all[i < 4u ? i : i + 1u];
        a->forward_reads[i] = use_read(s->buffer, s->offset, s->size);
        a->forward_reads[i].stages = SH_VERT | SH_FRAG;
    }
    a->forward_reads[7] = use_read(a->shape_data.buffer, a->shape_data.offset, a->shape_data.size);
    a->forward_reads[7].stages = SH_FRAG;
    a->bake_reads[0] = use_read(a->frame_data.buffer, a->frame_data.offset, a->frame_data.size);
    a->bake_reads[1] = use_read(a->shape_data.buffer, a->shape_data.offset, a->shape_data.size);
    a->bake_reads[0].stages = a->bake_reads[1].stages = SH_COMP;
    a->bin_reads[0] = use_read(a->frame_data.buffer, a->frame_data.offset, a->frame_data.size);
    a->bin_reads[1] = use_read(a->cull_data.buffer, a->cull_data.offset, a->cull_data.size);
    a->bin_reads[0].stages = a->bin_reads[1].stages = SH_COMP;
    a->bin_write = use_write(a->mask_data.buffer, a->mask_data.offset, a->mask_data.size);
    a->bin_write.stages = SH_COMP;

    {   /* Fog: frame, lights, types, masks, z bins. */
        static const uint32_t k_which[5] = {2u, 3u, 5u, 6u, 7u};
        for (i = 0u; i < 5u; ++i) {
            const BufSlice *s = all[k_which[i]];
            a->inject_reads[i] = a->fog_reads[i] = use_read(s->buffer, s->offset, s->size);
            a->inject_reads[i].stages = SH_COMP;
            a->fog_reads[i].stages    = SH_FRAG;
        }
        a->integrate_read = use_read(a->frame_data.buffer, a->frame_data.offset, a->frame_data.size);
        a->integrate_read.stages = SH_COMP;
    }

    memset(passes, 0, sizeof passes);
    {
        PassDesc *p = &passes[count++];
        strcpy(p->name, "BakeSdf");
        p->kind            = PASS_COMPUTE;
        p->pipe            = a->bake_pipe;
        p->state           = draw_state_opaque(SC_1, CULL_NONE);
        p->buf_reads       = a->bake_reads;
        p->buf_read_count  = 2u;
        p->tex_writes      = &a->sdf_volume;
        p->tex_write_count = 1u;
    }
    {
        /* Depth first: Forward then shades each pixel once, however many
         * surfaces cover it. Reversed depth: far = 0, nearer = greater. */
        PassDesc *p = &passes[count++];
        a->prepass_depth = attach_depth_clear(a->depth, 0.0f);
        strcpy(p->name, "PrePass");
        p->kind  = PASS_GRAPHICS;
        p->pipe  = a->prepass_pipe;
        p->state = draw_state_opaque(SC_1, CULL_BACK);
        p->state.depth_cmp   = CMP_GREATER;
        p->state.depth_write = 1u;
        p->depth          = &a->prepass_depth;
        p->buf_reads      = a->geometry_reads;
        p->buf_read_count = 3u;
    }
    {
        PassDesc *p = &passes[count++];
        strcpy(p->name, "BinLights");
        p->kind            = PASS_COMPUTE;
        p->pipe            = a->bin_pipe;
        p->state           = draw_state_opaque(SC_1, CULL_NONE);
        p->buf_reads       = a->bin_reads;
        p->buf_read_count  = 2u;
        p->buf_writes      = &a->bin_write;
        p->buf_write_count = 1u;
    }
    {
        PassDesc *p = &passes[count++];
        a->forward_color = attach_clear(a->hdr, 0.004f, 0.005f, 0.012f, 0.0f);
        a->forward_depth = attach_color(a->depth, ATT_DEPTH_WRITE);
        strcpy(p->name, "Forward");
        p->kind  = PASS_GRAPHICS;
        p->pipe  = a->forward_pipe;
        p->state = draw_state_opaque(SC_1, CULL_BACK);
        p->state.depth_cmp   = CMP_GEQUAL;   /* the surface the pre-pass kept */
        p->state.depth_write = 0u;
        p->colors         = &a->forward_color;
        p->color_count    = 1u;
        p->depth          = &a->forward_depth;
        p->buf_reads      = a->forward_reads;
        p->buf_read_count = 8u;
        p->tex_reads      = &a->sdf_volume;
        p->tex_read_count = 1u;
    }
    {
        PassDesc *p = &passes[count++];
        strcpy(p->name, "FogInject");
        p->kind            = PASS_COMPUTE;
        p->pipe            = a->inject_pipe;
        p->state           = draw_state_opaque(SC_1, CULL_NONE);
        p->buf_reads       = a->inject_reads;
        p->buf_read_count  = 5u;
        p->tex_reads       = &a->sdf_volume;
        p->tex_read_count  = 1u;
        p->tex_writes      = &a->fog_injected;
        p->tex_write_count = 1u;
    }
    {
        PassDesc *p = &passes[count++];
        strcpy(p->name, "FogIntegrate");
        p->kind            = PASS_COMPUTE;
        p->pipe            = a->integrate_pipe;
        p->state           = draw_state_opaque(SC_1, CULL_NONE);
        p->buf_reads       = &a->integrate_read;
        p->buf_read_count  = 1u;
        p->tex_reads       = &a->fog_injected;
        p->tex_read_count  = 1u;
        p->tex_writes      = &a->fog_integrated;
        p->tex_write_count = 1u;
    }
    {
        PassDesc *p = &passes[count++];
        a->fog_color        = attach_color(a->fogged, ATT_WRITE);
        a->fog_tex_reads[0] = a->hdr; a->fog_tex_reads[1] = a->depth;
        a->fog_tex_reads[2] = a->fog_integrated; a->fog_tex_reads[3] = a->sdf_volume;
        strcpy(p->name, "Fog");
        p->kind           = PASS_GRAPHICS;
        p->pipe           = a->fog_pipe;
        p->state          = draw_state_opaque(SC_1, CULL_NONE);
        p->colors         = &a->fog_color;
        p->color_count    = 1u;
        p->buf_reads      = a->fog_reads;
        p->buf_read_count = 5u;
        p->tex_reads      = a->fog_tex_reads;
        p->tex_read_count = 4u;
    }
    passes[count++] = demo_present_pass(d, a->fogged);
    d->program = demo_program(d, passes, count);
}

static void app_shutdown(App *a) { demo_mesh_free(&a->mesh); }

static void app_ui(App *a, struct nk_context *ctx) {
    Demo *d = &a->demo;
    static const char *const k_fog[3] = {"off", "froxels", "marched per pixel (slow)"};
    if (nk_begin(ctx, "Clustered", nk_rect(10, 10, 340, 500),
                 NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_TITLE)) {
        float lights = (float)a->light_count;
        demo_ui_text(ctx, "%.1f ms   %u lights", (double)d->dt * 1000.0, a->light_count);
        demo_ui_text(ctx, "%u x %u tiles, %u words each, %u z bins", a->tiles_x, a->tiles_y,
                     (a->light_count + 31u) / 32u, ZBINS);
        demo_ui_float(ctx, "Lights", &lights, 0.0f, (float)MAX_LIGHTS);
        a->light_count = (uint32_t)lights;
        demo_ui_choice(ctx, "View", k_views, VIEW_COUNT, &a->view);
        demo_ui_bool(ctx, "Every light per pixel", &a->brute);
        demo_ui_float(ctx, "Light intensity", &a->intensity, 0.0f, 12.0f);
        demo_ui_float(ctx, "Ambient", &a->ambient, 0.0f, 0.2f);
        demo_ui_float(ctx, "Moon", &a->moon, 0.0f, 2.0f);
        demo_ui_bool(ctx, "Shadows from the distance volume", &a->sdf_shadows);
        demo_ui_float(ctx, "Lamp radius, m", &a->lamp_radius, 0.02f, 0.5f);
        demo_ui_choice(ctx, "Fog", k_fog, 3u, &a->fog_mode);
        demo_ui_float(ctx, "Fog density, 1/m", &a->fog_density, 0.0f, 0.3f);
        demo_ui_float(ctx, "Fog height, m", &a->fog_height, 0.0f, 10.0f);
    }
    nk_end(ctx);
}

/* ---- acceptance ----------------------------------------------------------- */

/* Render one frame; the HDR target as floats (4 per pixel). */
/* One frame, then a half-float target the size of the picture as floats. */
static float *render_read(App *a, Target t) {
    app_frame(a);
    return demo_read_floats(&a->demo, t, (size_t)a->demo.width * a->demo.height * 4u);
}

static float *render_hdr(App *a) { return render_read(a, a->hdr); }

/* Render one frame; any RGBA16F target as floats. */
/* The fog: off it changes nothing; in a uniform, evenly lit fog both ways of
 * drawing it give the closed form; in the scene the froxels are compared with
 * the same medium marched per pixel with every light. */
static void check_fog(App *a, const DemoOrbit *cameras, const char *const *names) {
    Demo        *d      = &a->demo;
    const size_t pixels = (size_t)d->width * d->height;
    const App    saved  = *a;
    char     label[160];
    uint32_t cam, mode, pass;
    size_t   i;

    a->orbit = cameras[0]; a->view = 0u; a->brute = false; a->sdf_shadows = false;
    {
        float *fogged, *hdr;
        size_t differ = 0u;
        a->fog_mode = 0u;
        fogged = render_read(a, a->fogged);
        hdr    = render_read(a, a->hdr);
        for (i = 0u; i < pixels * 4u; ++i) differ += fogged[i] != hdr[i];
        demo_check(d, "fog off: values the fog pass changed", (double)differ, "== 0", differ == 0u);
        free(fogged); free(hdr);
    }
    /* Uniform fog, the same light everywhere, nothing else: T = exp(-density * path), L = albedo * light * (1 - T). */
    a->light_count = 0u; a->moon = 0.0f; a->fog_height = 0.0f; a->fog_ambient = 1.0f; a->fog_density = 0.05f;
    a->fog_output = 1u; a->fog_stride = 1u; a->fog_brute = false;
    for (cam = 0u; cam < 2u; ++cam)
        for (mode = 1u; mode <= 2u; ++mode) {
            float *v;
            double worst_t = 0.0, worst_l = 0.0, longest = 0.0, shortest = 1e9;
            a->orbit = cameras[cam]; a->fog_mode = mode;
            v = render_read(a, a->fogged);
            for (i = 0u; i < pixels; ++i) {
                const double path = v[i * 4u + 2u], t = exp(-0.05 * path), l = (double)a->fog_albedo * 0.55 * (1.0 - t);
                if (fabs(v[i * 4u + 1u] - t) > worst_t) worst_t = fabs(v[i * 4u + 1u] - t);
                if (fabs(v[i * 4u] - l) > worst_l) worst_l = fabs(v[i * 4u] - l);
                if (path > longest) longest = path;
                if (path < shortest) shortest = path;
            }
            free(v);
            if (mode == 1u) {
                snprintf(label, sizeof label, "%s, uniform fog: shortest path, m (reported)", names[cam]);
                demo_check(d, label, shortest, "", true);
                snprintf(label, sizeof label, "%s, uniform fog: longest path, m", names[cam]);
                demo_check(d, label, longest, "> 50", longest > 50.0);
            }
            snprintf(label, sizeof label, "%s, uniform fog, %s: transmittance vs exp(-density * path), worst", names[cam],
                     mode == 1u ? "froxels" : "marched");
            demo_check(d, label, worst_t, "< 0.002", worst_t < 0.002);
            snprintf(label, sizeof label, "%s, uniform fog, %s: light gathered vs the closed form, worst", names[cam],
                     mode == 1u ? "froxels" : "marched");
            demo_check(d, label, worst_l, "< 0.002", worst_l < 0.002);
        }
    /* The scene. Pass 0: the moon alone (shafts between the shapes); pass 1: the lamps as well. */
    a->fog_height = saved.fog_height; a->fog_ambient = saved.fog_ambient; a->fog_density = saved.fog_density;
    a->fog_output = 2u;
    for (cam = 0u; cam < 2u; ++cam)
        for (pass = 0u; pass < 2u; ++pass) {
            const char *what = pass ? "moon and lamps" : "moon alone";
            float  *froxels, *marched;
            double  sum_l = 0.0, worst_l = 0.0, mean_l = 0.0, sum_t = 0.0, worst_t = 0.0, mean_fog = 0.0;
            size_t  n = 0u, x, y;
            a->orbit = cameras[cam];
            a->moon = pass ? saved.moon : 1.0f;
            a->light_count = pass ? saved.light_count : 0u;
            a->fog_mode = 1u;
            froxels = render_read(a, a->fogged);
            a->fog_mode = 2u; a->fog_stride = 4u; a->fog_brute = false;
            marched = render_read(a, a->fogged);
            if (pass) {
                /* The lamps a point in mid-air gets from its tile and depth bin are all that reach it. */
                float *every;
                double worst = 0.0;
                a->fog_brute = true;
                every = render_read(a, a->fogged);
                for (i = 0u; i < pixels * 4u; ++i)
                    if (fabs((double)every[i] - marched[i]) > worst) worst = fabs((double)every[i] - marched[i]);
                free(every);
                snprintf(label, sizeof label, "%s, marched: lamps from the clusters vs every lamp, largest difference", names[cam]);
                demo_check(d, label, worst, "== 0", worst == 0.0);
            }
            for (y = 0u; y < d->height; y += 4u)
                for (x = 0u; x < d->width; x += 4u) {
                    const float *f = froxels + (y * d->width + x) * 4u, *m = marched + (y * d->width + x) * 4u;
                    uint32_t     c;
                    for (c = 0u; c < 3u; ++c) {
                        const double e = fabs((double)f[c] - m[c]);
                        sum_l += e / 3.0; mean_l += m[c] / 3.0;
                        if (e > worst_l) worst_l = e;
                    }
                    sum_t += fabs((double)f[3] - m[3]); mean_fog += 1.0 - m[3];
                    if (fabs((double)f[3] - m[3]) > worst_t) worst_t = fabs((double)f[3] - m[3]);
                    ++n;
                }
            free(froxels); free(marched);
            snprintf(label, sizeof label, "%s, %s: light the marched fog gathers, mean (reported)", names[cam], what);
            demo_check(d, label, mean_l / (double)n, "", true);
            snprintf(label, sizeof label, "%s, %s: froxels vs marched, mean difference", names[cam], what);
            demo_check(d, label, sum_l / (double)n, "reported", true);
            snprintf(label, sizeof label, "%s, %s:   as a share of the light gathered", names[cam], what);
            demo_check(d, label, sum_l / (mean_l > 0.0 ? mean_l : 1.0), pass ? "< 0.25 (guard)" : "< 0.03",
                             sum_l < (pass ? 0.25 : 0.03) * mean_l);
            snprintf(label, sizeof label, "%s, %s:   largest on a pixel (reported)", names[cam], what);
            demo_check(d, label, worst_l, "", true);
            snprintf(label, sizeof label, "%s, %s: share of the background hidden, mean (reported)", names[cam], what);
            demo_check(d, label, mean_fog / (double)n, "", true);
            snprintf(label, sizeof label, "%s, %s: transmittance, mean difference", names[cam], what);
            demo_check(d, label, sum_t / (double)n, "< 0.002", sum_t < 0.002 * (double)n);
            snprintf(label, sizeof label, "%s, %s:   largest on a pixel (reported)", names[cam], what);
            demo_check(d, label, worst_t, "", true);
        }
    {   /* everything back as it was; the volumes and buffers are per frame */
        const bool baked = a->sdf_baked;
        *a = saved;
        a->sdf_baked = baked;
    }
}

static uint32_t popcount32(uint32_t v) {
    uint32_t n = 0u;
    for (; v; v &= v - 1u) ++n;
    return n;
}

static void app_check(App *a) {
    Demo        *d      = &a->demo;
    const size_t pixels = (size_t)d->width * d->height;
    static const DemoOrbit k_cameras[2] = {
        {.target = {0.0f, 0.5f, 0.0f}, .distance = 20.0f, .yaw = 0.6f, .pitch = 0.5f,
         .fov_deg = 50.0f, .znear = 0.1f, .zfar = 100.0f},
        {.target = {1.0f, 0.9f, -2.0f}, .distance = 5.0f, .yaw = 2.2f, .pitch = 0.06f,
         .fov_deg = 65.0f, .znear = 0.1f, .zfar = 100.0f}};
    static const char *const k_name[2] = {"overview", "low camera"};
    const uint32_t words = (a->light_count + 31u) / 32u, tiles = a->tiles_x * a->tiles_y;
    char     label[128];
    uint32_t cam;
    size_t   i;

    for (cam = 0u; cam < 2u; ++cam) {
        float    *clustered, *brute, *counts;
        uint32_t *masks = (uint32_t *)malloc((size_t)tiles * words * 4u);
        double    worst = 0.0, peak = 0.0, tested = 0.0, reached = 0.0, bits = 0.0;
        size_t    covered = 0u;
        uint32_t  missing = 0u, centres = 0u, t, l, widest = 0u, b;

        a->orbit = k_cameras[cam];
        a->view  = 0u;
        a->brute = false;
        clustered = render_hdr(a);
        /* the masks and the sorted lights that frame was shaded with */
        demo_readback_buffer(d, (BufSlice){a->mask_data.buffer, a->mask_data.offset, (uint64_t)tiles * words * 4u},
                             masks, (size_t)tiles * words * 4u);
        a->view = 5u;
        counts  = render_hdr(a);
        a->view = 0u;
        a->brute = true;
        brute = render_hdr(a);

        /* ---- 1. nothing is lost ------------------------------------------- */
        for (i = 0u; i < pixels * 4u; ++i) {
            const double e = fabs((double)clustered[i] - (double)brute[i]);
            if (e > worst) worst = e;
            if ((i & 3u) != 3u && (double)brute[i] > peak) peak = brute[i];
        }
        snprintf(label, sizeof label, "%s: clustered vs all lights", k_name[cam]);
        demo_check(d, label, worst, "== 0 (HDR, every pixel)", worst == 0.0);
        snprintf(label, sizeof label, "%s: brightest HDR value", k_name[cam]);
        demo_check(d, label, peak, "> 0.5 (something is lit)", peak > 0.5);

        /* ---- 2. how much is skipped ---------------------------------------- */
        for (i = 0u; i < pixels; ++i) {
            if (counts[i * 4u + 3u] < 0.5f) continue;
            ++covered;
            tested  += counts[i * 4u];
            reached += counts[i * 4u + 1u];
        }
        tested  /= covered ? (double)covered : 1.0;
        reached /= covered ? (double)covered : 1.0;
        snprintf(label, sizeof label, "%s: lights tested per pixel", k_name[cam]);
        demo_check(d, label, tested, "< 5 % of all lights", covered > pixels / 2u && tested < 0.05 * a->light_count);
        snprintf(label, sizeof label, "%s: lights that reach it", k_name[cam]);
        demo_check(d, label, reached, "reported", true);
        snprintf(label, sizeof label, "%s: reach / tested", k_name[cam]);
        demo_check(d, label, tested > 0.0 ? reached / tested : 0.0, "reported", true);

        /* ---- 3. the masks, against the CPU --------------------------------- */
        /* Independent of the shader's plane test: a light whose bounding
         * sphere's centre projects into a tile must have that tile's bit. */
        for (l = 0u; l < a->light_count; ++l) {
            const float *c = a->cull[l];
            float px, py;
            if (c[2] >= -0.05f) continue; /* centre behind or at the eye */
            px = (c[0] / (-c[2] * a->frame.proj[0]) * 0.5f + 0.5f) * (float)d->width;
            py = (0.5f - c[1] / (-c[2] * a->frame.proj[1]) * 0.5f) * (float)d->height;
            if (px < 0.0f || py < 0.0f || px >= (float)d->width || py >= (float)d->height) continue;
            t = (uint32_t)(px / TILE) + (uint32_t)(py / TILE) * a->tiles_x;
            ++centres;
            if (!(masks[(size_t)t * words + (l >> 5)] & (1u << (l & 31u)))) ++missing;
        }
        for (i = 0u; i < (size_t)tiles * words; ++i) bits += popcount32(masks[i]);
        for (b = 0u; b < ZBINS; ++b)
            if (a->zbins[b][0] <= a->zbins[b][1] && a->zbins[b][1] - a->zbins[b][0] + 1u > widest)
                widest = a->zbins[b][1] - a->zbins[b][0] + 1u;
        snprintf(label, sizeof label, "%s: light centres on screen", k_name[cam]);
        demo_check(d, label, (double)centres, "> 100", centres > 100u);
        snprintf(label, sizeof label, "%s:   missing from their tile", k_name[cam]);
        demo_check(d, label, (double)missing, "== 0", missing == 0u);
        snprintf(label, sizeof label, "%s: bits set in the masks", k_name[cam]);
        demo_check(d, label, bits / ((double)tiles * a->light_count), "fraction, 0 < x < 0.2",
                         bits > 0.0 && bits / ((double)tiles * a->light_count) < 0.2);
        snprintf(label, sizeof label, "%s: widest z-bin range, lights", k_name[cam]);
        demo_check(d, label, (double)widest, "reported", true);

        free(clustered); free(masks); free(counts); free(brute);
    }
    demo_check(d, "tile masks + z bins, bytes", (double)tiles * words * 4.0 + ZBINS * 8.0, "reported", true);

    /* ---- 4. shadows from the distance volume -------------------------------- */
    /* The reference is the scene's exact distance function, walked in 1 cm
     * steps along the same rays (view 7). View 6 is the volume: the same fine
     * walk for the smallest distance met, and the shading's own walk for the
     * shadow term. */
    {
        const bool     was = a->sdf_shadows;
        const double   voxel = SDF_VOXEL;
        for (cam = 0u; cam < 2u; ++cam) {
            float   *off, *on, *on_brute, *surface;
            double   worst = 0.0, off_surface = 0.0;
            size_t   brighter = 0u, darker = 0u, covered = 0u;
            uint32_t lights[4] = {MOON_LIGHT, 0u, 0u, 0u}, group, l, k;
            float    nearest[3] = {1e30f, 1e30f, 1e30f};

            a->orbit = k_cameras[cam];
            a->view  = 0u; a->brute = false;
            a->sdf_shadows = false;
            off = render_hdr(a);
            a->sdf_shadows = true;
            on = render_hdr(a);
            a->brute = true;
            on_brute = render_hdr(a);
            a->brute = false;
            a->view  = 8u;
            surface  = render_hdr(a);
            for (i = 0u; i < pixels; ++i) {
                bool less = false;
                for (k = 0u; k < 3u; ++k) {
                    const double e = fabs((double)on[i * 4u + k] - (double)on_brute[i * 4u + k]);
                    if (e > worst) worst = e;
                    if (on[i * 4u + k] > off[i * 4u + k]) ++brighter;
                    if (on[i * 4u + k] < off[i * 4u + k]) less = true;
                }
                if (surface[i * 4u + 3u] < 0.5f) continue;
                ++covered;
                darker += less;
                if (fabs((double)surface[i * 4u]) > off_surface) off_surface = fabs((double)surface[i * 4u]);
            }
            snprintf(label, sizeof label, "%s, shadows: clustered vs all lights", k_name[cam]);
            demo_check(d, label, worst, "== 0 (HDR, every pixel)", worst == 0.0);
            snprintf(label, sizeof label, "%s, shadows: values brighter than without", k_name[cam]);
            demo_check(d, label, (double)brighter, "== 0", brighter == 0u);
            snprintf(label, sizeof label, "%s, shadows: pixels darker than without (share)", k_name[cam]);
            demo_check(d, label, (double)darker / (double)(covered ? covered : 1u), "> 0.05", darker * 20u > covered);
            snprintf(label, sizeof label, "%s: distance function at rasterized surfaces, largest (m)", k_name[cam]);
            demo_check(d, label, off_surface, "< 0.01", off_surface < 0.01);
            free(off); free(on); free(on_brute); free(surface);

            /* The three lamps nearest the point the camera looks at that hang
             * in the open (30 cm clear), in the order the last frame sorted
             * them. A lamp inside an object lights nothing and tests nothing. */
            for (l = 0u; l < a->light_count; ++l) {
                const float *p = a->lights[l].pos_radius;
                const float dx = p[0] - a->orbit.target[0], dy = p[1] - a->orbit.target[1], dz = p[2] - a->orbit.target[2];
                float       d2 = dx * dx + dy * dy + dz * dz;
                uint32_t    id = l;
                if (scene_distance(a, p[0], p[1], p[2]) < 0.3f) continue;
                for (k = 0u; k < 3u; ++k)
                    if (d2 < nearest[k]) {
                        const float    td = nearest[k];
                        const uint32_t ti = lights[k + 1u];
                        nearest[k] = d2; lights[k + 1u] = id;
                        d2 = td; id = ti;
                    }
            }
            /* group 0: the moon. group 1: the three lamps together. */
            for (group = 0u; group < 2u; ++group) {
                size_t valid = 0u, signs = 0u, leak = 0u, false_shadow = 0u, in_shadow = 0u, partly = 0u;
                double field_sum = 0.0, field_worst = 0.0, term_sum = 0.0;
                const char *who = group ? "3 lamps" : "moon";
                for (l = group ? 1u : 0u; l < (group ? 4u : 1u); ++l) {
                    float *vol, *ref;
                    a->check_light = lights[l];
                    a->view = 6u;
                    vol = render_hdr(a);
                    a->view = 7u;
                    ref = render_hdr(a);
                    for (i = 0u; i < pixels; ++i) {
                        const float *v = &vol[i * 4u], *r = &ref[i * 4u];
                        double       e;
                        if (v[3] < 0.5f || v[2] < 0.5f || r[2] < 0.5f) continue;
                        ++valid;
                        e = fabs((double)v[0] - (double)r[0]) / voxel;
                        field_sum += e;
                        if (e > field_worst) field_worst = e;
                        term_sum += fabs((double)v[1] - (double)r[1]);
                        if ((v[0] < 0.0f) != (r[0] < 0.0f)) ++signs;
                        if (r[1] < 0.5f) ++in_shadow;
                        if (r[1] > 0.01f && r[1] < 0.99f) ++partly;
                        /* blocked by more than a voxel of solid, yet lit here */
                        if ((double)r[0] < -voxel && v[1] >= 0.5f) ++leak;
                        /* fully lit in the reference, yet dark here */
                        if (r[1] >= 0.99f && v[1] < 0.5f) ++false_shadow;
                    }
                    free(vol); free(ref);
                }
                snprintf(label, sizeof label, "%s, %s: pixels facing it and in reach", k_name[cam], who);
                demo_check(d, label, (double)valid, "> 2000", valid > 2000u);
                snprintf(label, sizeof label, "%s, %s:   in shadow in the reference (share)", k_name[cam], who);
                demo_check(d, label, (double)in_shadow / (double)(valid ? valid : 1u), "> 0.02", in_shadow * 50u > valid);
                snprintf(label, sizeof label, "%s, %s:   in a penumbra in the reference (share)", k_name[cam], who);
                demo_check(d, label, (double)partly / (double)(valid ? valid : 1u), "reported", true);
                snprintf(label, sizeof label, "%s, %s: blocked by > 1 voxel of solid but lit", k_name[cam], who);
                demo_check(d, label, (double)leak, "== 0", leak == 0u);
                snprintf(label, sizeof label, "%s, %s: fully lit in the reference but dark (share)", k_name[cam], who);
                demo_check(d, label, (double)false_shadow / (double)(valid ? valid : 1u), "< 0.001",
                                 false_shadow * 1000u < valid);
                snprintf(label, sizeof label, "%s, %s: blocked / not blocked differs (share)", k_name[cam], who);
                demo_check(d, label, (double)signs / (double)(valid ? valid : 1u), "reported", true);
                snprintf(label, sizeof label, "%s, %s: smallest distance on the ray, mean difference (voxels)", k_name[cam], who);
                demo_check(d, label, field_sum / (double)(valid ? valid : 1u), "< 0.1", field_sum < 0.1 * (double)valid);
                snprintf(label, sizeof label, "%s, %s:   largest (voxels)", k_name[cam], who);
                demo_check(d, label, field_worst, "reported", true);
                snprintf(label, sizeof label, "%s, %s: shadow term, mean difference", k_name[cam], who);
                demo_check(d, label, term_sum / (double)(valid ? valid : 1u), "< 0.02", term_sum < 0.02 * (double)valid);
            }
        }
        a->check_light = MOON_LIGHT;
        a->sdf_shadows = was;
        a->view        = 0u;
    }
    check_fog(a, k_cameras, k_name);
}

int main(int argc, char **argv) {
    static App app;
    static const DemoOption options[] = {
        {"--lights", .u32 = &app.light_count, .lo = 0, .hi = MAX_LIGHTS, .help = "lights"},
        {"--spots", .f32 = &app.spot_share, .lo = 0, .hi = 1, .help = "share of them that are spot lights"},
        {"--view", .u32 = &app.view, .lo = 0, .hi = VIEW_COUNT - 1, .help = "0 lit, 1 lights tested per pixel, 2 lights in the tile's mask, 3 lights in the depth bin's range, 4 tested but out of reach"},
        {"--brute", .flag = &app.brute, .help = "every light per pixel (no masks, no bins)"},
        {"--sdf-shadows", .flag = &app.sdf_shadows, .help = "a shadow ray per light through a distance volume of the scene"},
        {"--sdf-bias", .f32 = &app.sdf_bias, .lo = 0, .hi = 4, .help = "start shadow rays this many voxels off the surface"},
        {"--fog-height", .f32 = &app.fog_height, .lo = 0, .hi = 1000, .help = "density falls by e every H metres; 0 = uniform"},
        {"--fog-steps", .u32 = &app.fog_steps, .lo = 1, .hi = 8192, .help = "steps of the --fog-reference march"},
        {"--fog", .f32 = &app.fog_density, .flag = &app.fog_on, .lo = 0, .hi = 2,
         .help = "fog of this density per metre at the ground, lit through froxels (try 0.02)"},
        {"--fog-reference", .flag = &app.fog_reference, .help = "with --fog: march the fog per pixel instead (slow)"},
        {0}};
    demo_defaults(&app.demo, "clustered", "Clustered forward - tile bitmasks and z bins");
    app.orbit = (DemoOrbit){.target = {0.0f, 0.5f, 0.0f}, .distance = 20.0f, .yaw = 0.6f, .pitch = 0.5f,
                            .fov_deg = 50.0f, .znear = 0.1f, .zfar = 100.0f,
                            .min_distance = 2.0f, .max_distance = 60.0f};
    app.light_count = 1024u;
    app.intensity   = 2.0f;
    app.ambient     = 0.02f;
    app.moon        = 0.25f;
    app.spot_share  = 0.3f;
    app.lamp_radius = 0.12f;
    app.moon_k      = 24.0f;   /* a moon 2.4 degrees in radius: soft enough to see */
    app.sdf_bias    = 0.5f;
    app.check_light = MOON_LIGHT;
    app.fog_density = 0.02f;
    app.fog_height  = 2.5f;
    app.fog_ambient = 0.01f;
    app.fog_g       = 0.4f;
    app.fog_albedo  = 0.9f;
    app.fog_steps   = 128u;
    app.fog_stride  = 1u;
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
