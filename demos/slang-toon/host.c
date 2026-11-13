/* host.c — reference host for the bindless Slang toon shaders (ABI v2).
 *
 * Renders the demos/toon character scene (ground, figure, ball, crate) through
 * the shaders under demos/slang-toon and demos/slang-toon-inspired
 * (spirv/bindless), with the passes the upstream URP shader has:
 *
 *   ShadowMap  vertShadow  + fragDepth          -> D32F shadow map
 *   Backdrop   demos/toon sky                    -> colour
 *   Forward    vertMain    + frag<Model>         -> colour + depth, CULL_BACK
 *   Outline    vertOutline + frag<Model>Outline  -> colour, CULL_FRONT, depth read
 *
 * Everything the shaders read lives in ONE storage-buffer slice: FrameData,
 * one ObjectData and one MaterialData(+MaterialStyle) per object, two
 * MaterialMaps, InspiredData, one PointLight, then the vertices. Each draw
 * pushes 48 bytes of byte offsets. See bindless/BINDLESS.md for the layout.
 *
 *   ./build/toon-cmake/slang_toon_host                       windowed
 *   ./build/toon-cmake/slang_toon_host --headless out.ppm    one frame to a PPM
 *   ./build/toon-cmake/slang_toon_host --help                every switch
 *
 * Run from the repository root: the shader paths are relative. The windowed
 * path uses demos/common/platform.c (GLFW).
 */
#include "mu_math.h"
#include "platform.h"
#include "scene.h"
#include "vk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define WIN_W 1280u
#define WIN_H 720u
#define SHADOW_SIZE 2048u
#define NO_TEXTURE 0xFFFFFFFFu
#define CHECKER_SIZE 512u

/* ---- shader ABI (must match bindless/toon.slang) ------------------------- */

typedef struct { float m[16]; } Mat4; /* row-major, as the shader declares */
typedef struct {
    Mat4 objectToWorld, worldToView, viewToClip, normalToWorld, worldToShadow;
    float cameraPosition[4], mainDirectionShadow[4], mainColor[4], indirectColor[4];
    float fogColor[4], fogParams[4], shadowParams[4], outlineCamera[4];
} FrameData;
typedef struct { Mat4 objectToWorld, normalToWorld; } ObjectData;
typedef struct {
    float baseColor[4], baseUV[4], emissionColor[4], emissionChannel[4];
    float occlusionChannel[4], occlusionParams[4], indirectMin[4], celParams[4];
    float shadowColor[4], alphaParams[4], outlineColorWidth[4], outlineParams[4];
    float bandParams[4], midColor[4], specularParams[4], specularColor[4];
    float rimParams[4], rimColor[4];
} MaterialData;
typedef struct { float shadowHSV[4], styleParams[4]; } MaterialStyle;
typedef struct { MaterialData base; MaterialStyle style; } MaterialRecord;
typedef struct {
    uint32_t baseMap, emissionMap, occlusionMap, outlineMask, shadowMap, faceMap;
    uint32_t normalMap, materialMap, matcapMap, noiseMap, sceneLinearDepth, hairShadowMask;
    uint32_t linearSampler, shadowSampler, shadowWidth, shadowHeight;
} MaterialMaps;
typedef struct { float positionInvRange[4], colorIntensity[4]; } PointLight;
typedef struct {
    uint32_t frameOffset, materialOffset, inspiredOffset, mapsOffset;
    uint32_t pointLightOffset, pointLightCount, vertexOffset, flags;
    uint32_t objectOffset, reserved0, reserved1, reserved2;
} BindlessPush;

#define BINDLESS_HAS_INSPIRED           (1u << 0)
#define TOON_FLAG_OBJECT_DATA           (1u << 1)
#define TOON_FLAG_MATERIAL_STYLE        (1u << 2)
#define TOON_FLAG_SMOOTH_OUTLINE_NORMAL (1u << 3)
#define TOON_FLAG_OUTLINE_PIXEL_WIDTH   (1u << 4)
#define TOON_FLAG_SHADOW_FLIP_V         (1u << 5)

#define INSPIRED_BYTES 224u

_Static_assert(sizeof(FrameData) == 448, "FrameData must match the shader");
_Static_assert(sizeof(ObjectData) == 128, "ObjectData must match the shader");
_Static_assert(sizeof(MaterialData) == 288, "MaterialData must match the shader");
_Static_assert(sizeof(MaterialRecord) == 320, "MaterialStyle follows MaterialData at +288");
_Static_assert(sizeof(MaterialMaps) == 64, "MaterialMaps must match the shader");
_Static_assert(sizeof(PointLight) == 32, "PointLight must match the shader");
_Static_assert(sizeof(BindlessPush) == 48, "BindlessPush must match the shader");

/* demos/toon/shaders/sky.slang (the backdrop is borrowed from that demo). */
typedef struct { float horizon[4], zenith[4], sun[4], params[4]; } SkyPush;
_Static_assert(sizeof(SkyPush) == 64, "SkyPush must match demos/toon/shaders/sky.slang");

/* ---- host ---------------------------------------------------------------- */

typedef enum Model { MODEL_TUTORIAL, MODEL_EXTENDED, MODEL_INSPIRED } Model;

typedef struct Options {
    Model    model;
    bool     shadows, outline, smooth_normals, pixel_outline, style, point_light;
    bool     shadow_flip_v;   /* debug: turn off to see why the flag exists */
    float    outline_width;   /* < 0: pick the default for the outline mode */
    float    distance, yaw, pitch, fov_deg;
    uint32_t width, height;
} Options;

typedef struct Host {
    Gpu      *gpu;
    Platform *platform;
    Options   opt;
    ToonScene scene;
    BufSlice  slice;          /* the one storage-buffer slice              */
    uint8_t  *blob;           /* CPU copy; FrameData is re-uploaded on orbit */
    uint32_t  off_frame, off_objects, off_materials, off_maps, off_inspired;
    uint32_t  off_lights, off_vertices;
    Pipe      sky_pipe, shadow_pipe, forward_pipe, outline_pipe;
    Target    color, depth, shadow;
    FrameProgram *program;
    Texture   checker;
    Sampler   linear_clamp, linear_repeat, nearest;
    uint32_t  width, height;
    uint32_t  flags;
    SkyPush   sky;
    float     light[3];
    bool      windowed;
} Host;

static void store_row_major(float dst[16], const float src[16]) {
    /* The shader's matrices are row_major; mu_math matrices are column-major. */
    mu_mat4_transpose(src, dst);
}

/* Orthographic projection with z in [0,1] (mu_mat4_ortho is the GL [-1,1] one). */
static void ortho_zo(float *r, float half, float znear, float zfar) {
    mu_mat4_identity(r);
    r[0]  = 1.0f / half;
    r[5]  = 1.0f / half;
    r[10] = -1.0f / (zfar - znear);
    r[14] = -znear / (zfar - znear);
}

/* ---- scene data ---------------------------------------------------------- */

static void set4(float *d, float x, float y, float z, float w) {
    d[0] = x; d[1] = y; d[2] = z; d[3] = w;
}

/* Camera, light and shadow matrices. Called once, and per frame when the
 * window orbits. */
static void fill_frame(Host *h, float yaw) {
    FrameData *fd = (FrameData *)(h->blob + h->off_frame);
    const Options *o = &h->opt;
    const float target[3] = {0.0f, 1.15f, 0.0f};
    const float up[3] = {0.0f, 1.0f, 0.0f};
    const float eye[3] = {target[0] + o->distance * sinf(yaw) * cosf(o->pitch),
                          target[1] + o->distance * sinf(o->pitch),
                          target[2] + o->distance * cosf(yaw) * cosf(o->pitch)};
    const float aspect = (float)h->width / (float)h->height;
    const float shadow_center[3] = {0.0f, 1.0f, 0.0f};
    float shadow_eye[3];
    mu_mat4 view, proj, light_view, light_proj, world_to_shadow, view_proj;
    mu_vec4 point, clip;

    memset(fd, 0, sizeof *fd);
    mu_mat4_look_at(view, eye, target, up);
    mu_mat4_perspective_zo(proj, mu_radians(o->fov_deg), aspect, 0.1f, 100.0f);
    /* objectToWorld / normalToWorld are per object (ObjectData); the copies in
     * FrameData are only read by hosts that do not set TOON_FLAG_OBJECT_DATA. */
    mu_mat4_identity(fd->objectToWorld.m);
    mu_mat4_identity(fd->normalToWorld.m);
    store_row_major(fd->worldToView.m, view);
    store_row_major(fd->viewToClip.m, proj);

    shadow_eye[0] = shadow_center[0] + h->light[0] * 14.0f;
    shadow_eye[1] = shadow_center[1] + h->light[1] * 14.0f;
    shadow_eye[2] = shadow_center[2] + h->light[2] * 14.0f;
    mu_mat4_look_at(light_view, shadow_eye, shadow_center, up);
    ortho_zo(light_proj, 4.5f, 0.5f, 30.0f);
    mu_mat4_mul(light_proj, light_view, world_to_shadow);
    store_row_major(fd->worldToShadow.m, world_to_shadow);

    set4(fd->cameraPosition, eye[0], eye[1], eye[2], 1.0f);
    set4(fd->mainDirectionShadow, h->light[0], h->light[1], h->light[2],
         o->shadows ? 1.0f : 0.0f);
    set4(fd->mainColor, 1.0f, 0.97f, 0.92f, 0.0f);
    set4(fd->indirectColor, 0.20f, 0.21f, 0.30f, 0.0f);
    set4(fd->fogColor, 0.74f, 0.68f, 0.80f, 0.0f);
    set4(fd->fogParams, 14.0f, 60.0f, 1.0f, 0.0f);
    /* x caster depth bias (world), y receiver depth bias (normalized shadow z),
     * z receiver offset (world), w caster normal bias (world). One shadow texel
     * is 9 / 2048 = 0.0044 world units. */
    set4(fd->shadowParams, 0.010f, 0.0006f, 0.0f, 0.020f);
    /* w = render-target height: what makes pixel-width outlines possible. */
    set4(fd->outlineCamera, o->fov_deg, 5.0f, 0.0f, (float)h->height);

    /* Backdrop: the sun sits where the light direction projects to. */
    mu_mat4_mul(proj, view, view_proj);
    mu_vec4_make(point, eye[0] + h->light[0] * 40.0f, eye[1] + h->light[1] * 40.0f,
                 eye[2] + h->light[2] * 40.0f, 1.0f);
    mu_mat4_mulv(clip, view_proj, point);
    set4(h->sky.horizon, 0.74f, 0.68f, 0.80f, 1.8f);
    set4(h->sky.zenith, 0.20f, 0.26f, 0.46f, 0.55f);
    set4(h->sky.sun, 0.5f + clip[0] / clip[3] * 0.5f, 0.5f + clip[1] / clip[3] * 0.5f,
         0.055f, clip[3] > 0.0f ? 1.0f : 0.0f);
    set4(h->sky.params, 0.0f, 0.0f, 0.0f, 0.0f);
}

static void fill_material(const Host *h, MaterialRecord *record, const ToonObject *object,
                          uint32_t index) {
    MaterialData  *md = &record->base;
    MaterialStyle *ms = &record->style;
    const Options *o = &h->opt;
    const bool ground = index == 0u;
    const bool eye = object->color[0] < 0.2f && object->color[1] < 0.2f;
    const bool checker = (object->flags & TOON_OBJ_USE_CHECKER) != 0u;
    /* The checker texture stores 0.774 / 1.0; x1.06 gives demos/toon's
     * 0.82 / 1.06 pair. */
    const float tint = checker ? 1.06f : 1.0f;
    float width = o->outline_width;

    memset(record, 0, sizeof *record);
    set4(md->baseColor, object->color[0] * tint, object->color[1] * tint,
         object->color[2] * tint, object->color[3]);
    set4(md->baseUV, 1.0f, 1.0f, 0.0f, 0.0f);
    set4(md->emissionChannel, 1.0f, 1.0f, 1.0f, 0.0f);
    set4(md->occlusionChannel, 1.0f, 0.0f, 0.0f, 0.0f);
    set4(md->occlusionParams, 0.0f, 1.0f, 0.0f, 1.0f);
    set4(md->indirectMin, 0.10f, 0.11f, 0.20f, 0.0f);
    /* x midpoint, y softness, z isFace, w receive-shadow amount. The softness is
     * deliberately small: a crisp band is where band anti-aliasing matters. */
    set4(md->celParams, 0.05f, 0.005f, 0.0f, 0.85f);
    set4(md->shadowColor, 0.40f, 0.35f, 0.56f, 0.0f);
    set4(md->alphaParams, 0.0f, 0.5f, 0.0f, 0.0f);

    /* Width: pixels in pixel mode, upstream's unit otherwise. 9.0 is what the
     * tutorial formula needs at this camera for about the same line. */
    if (width < 0.0f) width = (o->pixel_outline && o->model != MODEL_TUTORIAL) ? 2.0f : 9.0f;
    set4(md->outlineColorWidth, 0.22f, 0.17f, 0.28f, width * object->outline_scale);
    set4(md->outlineParams, 0.0001f, 0.0f, 1.0f, 0.0f);

    set4(md->bandParams, 0.55f, 0.005f, 0.0f, 0.0f);
    set4(md->midColor, 0.74f, 0.66f, 0.80f, 0.0f);
    set4(md->specularParams, eye ? 60.0f : 28.0f, eye ? 0.30f : 0.42f, 0.01f,
         ground ? 0.0f : (eye ? 0.9f : 0.28f));
    set4(md->specularColor, 1.0f, 1.0f, 1.0f, 0.0f);
    /* The ground is seen at grazing angles everywhere, so a view-dependent rim
     * would cover it. */
    set4(md->rimParams, 2.5f, 0.50f, 0.01f, ground ? 0.0f : 0.30f);
    set4(md->rimColor, 1.0f, 0.86f, 0.70f, 0.0f);

    /* Style block; only read when TOON_FLAG_MATERIAL_STYLE is pushed. */
    /* Shadows drift slightly towards red/violet and gain a little saturation
     * instead of going grey: the usual anime shadow-colour rule. */
    set4(ms->shadowHSV, -0.015f, 1.12f, 1.0f, 0.0f);
    set4(ms->styleParams, 1.25f, 0.18f, 1.0f, 0.0f);
}

static bool build_scene(Host *h) {
    const Options *o = &h->opt;
    uint32_t i, shadow_index = NO_TEXTURE, checker_index = NO_TEXTURE;
    uint64_t vertex_bytes, total;
    MaterialMaps *maps;
    float *ins;
    Upload *upload;

    if (!toon_scene_build(&h->scene)) return false;
    toon_scene_bake_outline_normals(&h->scene);

    vertex_bytes = (uint64_t)h->scene.vertex_count * sizeof(ToonVertex);
    total = sizeof(FrameData) + (uint64_t)h->scene.object_count *
            (sizeof(ObjectData) + sizeof(MaterialRecord)) + 2u * sizeof(MaterialMaps) +
            INSPIRED_BYTES + sizeof(PointLight) + vertex_bytes;
    total = (total + 15u) & ~(uint64_t)15u;

    h->slice = arena_alloc(h->gpu, ARENA_DEVICE, total, 64u);
    if (!HANDLE_VALID(h->slice.buffer)) return false;
    h->blob = (uint8_t *)calloc(1u, (size_t)total);
    if (!h->blob) return false;

    /* Offsets inside the blob. The shader wants offsets into the bound storage
     * buffer, so record() adds h->slice.offset. Note off_vertices is not a
     * multiple of 48: the shader loads by byte offset, so it does not care. */
    h->off_frame     = 0u;
    h->off_objects   = h->off_frame + (uint32_t)sizeof(FrameData);
    h->off_materials = h->off_objects + h->scene.object_count * (uint32_t)sizeof(ObjectData);
    h->off_maps      = h->off_materials + h->scene.object_count * (uint32_t)sizeof(MaterialRecord);
    h->off_inspired  = h->off_maps + 2u * (uint32_t)sizeof(MaterialMaps);
    h->off_lights    = h->off_inspired + INSPIRED_BYTES;
    h->off_vertices  = h->off_lights + (uint32_t)sizeof(PointLight);

    fill_frame(h, o->yaw);

    for (i = 0u; i < h->scene.object_count; ++i) {
        const ToonObject *object = &h->scene.objects[i];
        ObjectData *od = (ObjectData *)(h->blob + h->off_objects + i * sizeof(ObjectData));
        /* Uniform scale + rotation: the inverse transpose is the same matrix up
         * to scale, and the shader normalizes and ignores the translation. */
        store_row_major(od->objectToWorld.m, object->model);
        store_row_major(od->normalToWorld.m, object->model);
        fill_material(h, (MaterialRecord *)(h->blob + h->off_materials +
                                             i * sizeof(MaterialRecord)), object, i);
    }

    if (o->shadows &&
        !texture_info(h->gpu, target_texture(h->gpu, h->shadow), &shadow_index))
        return false;
    if (!texture_info(h->gpu, h->checker, &checker_index)) return false;

    /* maps[0]: untextured; maps[1]: checker base map through a repeat sampler. */
    maps = (MaterialMaps *)(h->blob + h->off_maps);
    for (i = 0u; i < 2u; ++i) {
        MaterialMaps *mm = &maps[i];
        mm->baseMap = i == 1u ? checker_index : NO_TEXTURE;
        mm->emissionMap = mm->occlusionMap = mm->outlineMask = NO_TEXTURE;
        mm->faceMap = mm->normalMap = mm->materialMap = NO_TEXTURE;
        mm->matcapMap = mm->noiseMap = mm->sceneLinearDepth = mm->hairShadowMask = NO_TEXTURE;
        mm->shadowMap = shadow_index;
        mm->linearSampler = sampler_bindless_index(h->gpu, i == 1u ? h->linear_repeat
                                                                   : h->linear_clamp);
        mm->shadowSampler = sampler_bindless_index(h->gpu, h->nearest);
        mm->shadowWidth = mm->shadowHeight = SHADOW_SIZE;
    }

    /* InspiredData: the shader's neutralInspired() with the real target size. */
    ins = (float *)(h->blob + h->off_inspired);
    {
        const float neutral[14][4] = {
            {1, 0, 0, 0}, {0, 0, 1, 0}, {0, 0.03f, 0, 0}, {0, 1, 0.5f, 0},
            {0.35f, 0.1f, 0.03f, 1}, {1, 1, 1, 0}, {0, 0, 0, 0}, {0, 16, 0, 0.7f},
            {1, 1, 1, 0}, {(float)h->width, (float)h->height, 2, 0.02f}, {0, 0, 0, 0},
            {1, 0, 0.05f, 1}, {1, 0.3f, 0.05f, 0}, {1, 1, 1, 1}};
        memcpy(ins, neutral, sizeof neutral);
    }

    {
        PointLight *pl = (PointLight *)(h->blob + h->off_lights);
        set4(pl->positionInvRange, -1.9f, 0.9f, 1.5f, 1.0f / 5.0f);
        set4(pl->colorIntensity, 1.0f, 0.55f, 0.25f, 6.0f);
    }

    memcpy(h->blob + h->off_vertices, h->scene.vertices, (size_t)vertex_bytes);

    upload = upload_begin(h->gpu);
    if (!upload) return false;
    upload_buffer(upload, h->slice, DATA_SPAN(h->blob, (size_t)total));
    upload_end(h->gpu, upload);
    return true;
}

/* ---- recording ----------------------------------------------------------- */

static void push_object(const Host *h, PassCtx *p, uint32_t index) {
    const ToonObject *object = &h->scene.objects[index];
    const uint32_t base = (uint32_t)h->slice.offset;
    BindlessPush push;

    memset(&push, 0, sizeof push);
    push.frameOffset      = base + h->off_frame;
    push.materialOffset   = base + h->off_materials + index * (uint32_t)sizeof(MaterialRecord);
    push.inspiredOffset   = base + h->off_inspired;
    push.mapsOffset       = base + h->off_maps +
                            ((object->flags & TOON_OBJ_USE_CHECKER) ? (uint32_t)sizeof(MaterialMaps) : 0u);
    push.pointLightOffset = base + h->off_lights;
    push.pointLightCount  = h->opt.point_light ? 1u : 0u;
    push.vertexOffset     = base + h->off_vertices + object->first_vertex * (uint32_t)sizeof(ToonVertex);
    push.flags            = h->flags;
    push.objectOffset     = base + h->off_objects + index * (uint32_t)sizeof(ObjectData);
    pass_push(p, &push, sizeof push);
    pass_draw(p, object->vertex_count, 1u, 0u, 0u);
}

static void record_shadow(void *user, PassCtx *p) {
    const Host *h = (const Host *)user;
    uint32_t i;
    /* The ground only receives: it is the lowest thing in the scene. */
    for (i = 1u; i < h->scene.object_count; ++i) push_object(h, p, i);
}

static void record_sky(void *user, PassCtx *p) {
    const Host *h = (const Host *)user;
    pass_push(p, &h->sky, sizeof h->sky);
    pass_draw_fullscreen_triangle(p, h->width, h->height);
}

static void record_forward(void *user, PassCtx *p) {
    const Host *h = (const Host *)user;
    uint32_t i;
    for (i = 0u; i < h->scene.object_count; ++i) push_object(h, p, i);
}

static void record_outline(void *user, PassCtx *p) {
    const Host *h = (const Host *)user;
    uint32_t i;
    for (i = 0u; i < h->scene.object_count; ++i)
        if (!(h->scene.objects[i].flags & TOON_OBJ_NO_OUTLINE)) push_object(h, p, i);
}

/* ---- init ---------------------------------------------------------------- */

/* 8x8 checker with a full mip chain. The ground magnifies it and the crate
 * and ball minify it several times over, so without mips the cell edges alias
 * into wobbly lines on the small objects. */
static bool make_checker(Host *h) {
    static uint8_t pixels[CHECKER_SIZE * CHECKER_SIZE * 4u];
    TextureDesc td;
    Upload *upload;
    uint32_t level, levels = 0u, size, x, y;

    for (size = CHECKER_SIZE; size > 0u; size >>= 1u) ++levels;
    memset(&td, 0, sizeof td);
    td.name = "checker";
    td.width = td.height = CHECKER_SIZE;
    td.depth = td.layers = 1u;
    td.mips = levels;
    td.format = FMT_RGBA8_UNORM;
    td.usage = TEX_SAMPLED | TEX_XFER_DST;
    td.cls = RES_PERSISTENT;
    h->checker = texture_create(h->gpu, &td);
    if (!HANDLE_VALID(h->checker)) return false;

    upload = upload_begin(h->gpu);
    if (!upload) return false;
    for (level = 0u; level < levels; ++level) {
        const uint32_t dim = CHECKER_SIZE >> level;
        const uint32_t cell = dim / 8u; /* 0 once a texel covers several cells */
        ImageRegion region;
        for (y = 0u; y < dim; ++y)
            for (x = 0u; x < dim; ++x) {
                /* demos/toon: mod(floor(u*8) + floor(v*8), 2) picks 1.06 over
                 * 0.82; stored as 1.0 / 0.774 and scaled by baseColor. */
                const uint8_t v = cell == 0u ? 226u
                                : ((((x / cell) + (y / cell)) & 1u) ? 255u : 197u);
                uint8_t *px = &pixels[(y * dim + x) * 4u];
                px[0] = px[1] = px[2] = v;
                px[3] = 255u;
            }
        memset(&region, 0, sizeof region);
        region.mip = level;
        region.width = region.height = dim;
        region.depth = 1u;
        /* upload_texture copies into staging, so the scratch buffer is reused. */
        upload_texture(upload, h->checker, &region, DATA_SPAN(pixels, (size_t)dim * dim * 4u));
    }
    upload_end(h->gpu, upload);
    return true;
}

static const char *model_name(Model m) {
    return m == MODEL_TUTORIAL ? "tutorial" : m == MODEL_EXTENDED ? "extended" : "inspired";
}

static bool init_host(Host *h, bool windowed) {
    const Options *o = &h->opt;
    static const char *forward_fs[] = {
        "demos/slang-toon/spirv/bindless/fragTutorial.spv",
        "demos/slang-toon/spirv/bindless/fragExtended.spv",
        "demos/slang-toon-inspired/spirv/bindless/fragInspired.spv"};
    static const char *outline_fs[] = {
        "demos/slang-toon/spirv/bindless/fragTutorialOutline.spv",
        "demos/slang-toon/spirv/bindless/fragExtendedOutline.spv",
        "demos/slang-toon-inspired/spirv/bindless/fragInspiredOutline.spv"};
    GpuDesc gd;
    GpuInfo info;
    SamplerDesc sd;
    TargetDesc td;
    Format color_format;
    ShaderId vs_main, vs_outline, vs_shadow, fs_depth, fs_forward, fs_outline, vs_sky, fs_sky;
    AttachDesc sky_color, keep_color, depth_clear, depth_read, shadow_clear;
    PassDesc passes[4];
    Target reads[1];
    uint32_t pass_count = 0u;
    float len;

    h->windowed = windowed;
    h->width = o->width;
    h->height = o->height;

    /* TOON_FLAG_OBJECT_DATA is not optional here: nine objects share one
     * FrameData. The rest follows the command line. */
    h->flags = TOON_FLAG_OBJECT_DATA;
    if (o->model == MODEL_INSPIRED) h->flags |= BINDLESS_HAS_INSPIRED;
    if (o->style) h->flags |= TOON_FLAG_MATERIAL_STYLE;
    if (o->smooth_normals) h->flags |= TOON_FLAG_SMOOTH_OUTLINE_NORMAL;
    if (o->pixel_outline) h->flags |= TOON_FLAG_OUTLINE_PIXEL_WIDTH;
    if (o->shadow_flip_v) h->flags |= TOON_FLAG_SHADOW_FLIP_V;
    /* The tutorial stages are a strict port: no style, upstream outline width. */
    if (o->model == MODEL_TUTORIAL)
        h->flags &= ~(TOON_FLAG_MATERIAL_STYLE | TOON_FLAG_OUTLINE_PIXEL_WIDTH);

    h->light[0] = -0.62f; h->light[1] = 0.70f; h->light[2] = 0.36f;
    len = sqrtf(h->light[0] * h->light[0] + h->light[1] * h->light[1] + h->light[2] * h->light[2]);
    h->light[0] /= len; h->light[1] /= len; h->light[2] /= len;

    if (windowed) {
        h->platform = platform_create(o->width, o->height, "Slang Toon (bindless)");
        if (!h->platform) {
            fprintf(stderr, "platform_create failed (no display? use --headless)\n");
            return false;
        }
    }

    memset(&gd, 0, sizeof gd);
    gd.width = o->width;
    gd.height = o->height;
    gd.frames_in_flight = 3u;
    gd.app_name = "slang-toon-host";
    gd.vsync = windowed;
    h->gpu = gpu_create(&gd);
    if (!h->gpu) { fprintf(stderr, "gpu_create failed: %s\n", gpu_last_error(NULL)); return false; }

    if (windowed) {
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        if (!platform_create_surface(h->platform, gpu_raw_instance(h->gpu), &surface)) {
            fprintf(stderr, "platform_create_surface failed\n");
            return false;
        }
        if (!set_surface(h->gpu, surface)) {
            vkDestroySurfaceKHR(gpu_raw_instance(h->gpu), surface, NULL);
            fprintf(stderr, "set_surface: %s\n", gpu_last_error(h->gpu));
            return false;
        }
    }

    gpu_info(h->gpu, &info);
    if (windowed) { h->width = info.extent_w; h->height = info.extent_h; }

    if (!make_checker(h)) { fprintf(stderr, "texture: %s\n", gpu_last_error(h->gpu)); return false; }

    sd = sampler_linear_clamp();
    h->linear_clamp = sampler_create(h->gpu, &sd);
    sd.addr_u = sd.addr_v = sd.addr_w = ADDR_REPEAT;
    sd.mip_filter = FILT_LINEAR;
    sd.max_lod = 16.0f;
    sd.max_aniso = 8u; /* the ground is seen at a grazing angle */
    h->linear_repeat = sampler_create(h->gpu, &sd);
    sd = sampler_linear_clamp();
    sd.min_filter = sd.mag_filter = sd.mip_filter = FILT_NEAREST;
    h->nearest = sampler_create(h->gpu, &sd);
    if (!HANDLE_VALID(h->linear_clamp) || !HANDLE_VALID(h->linear_repeat) ||
        !HANDLE_VALID(h->nearest)) {
        fprintf(stderr, "sampler: %s\n", gpu_last_error(h->gpu));
        return false;
    }

    /* ---- targets -------------------------------------------------------- */
    memset(&td, 0, sizeof td);
    td.name = "depth";
    td.width = h->width; td.height = h->height;
    td.format = FMT_D32F;
    td.usage = TGT_DEPTH;
    td.samples = SC_1;
    td.mips = 1u;
    td.cls = RES_PERSISTENT;
    h->depth = target_create(h->gpu, &td);
    if (!HANDLE_VALID(h->depth)) { fprintf(stderr, "depth target: %s\n", gpu_last_error(h->gpu)); return false; }

    if (o->shadows) {
        /* A depth target that is also sampled: the forward pass reads it as a
         * plain float texture (nearest, manual 3x3 PCF in mainShadow()). */
        td.name = "shadow_map";
        td.width = td.height = SHADOW_SIZE;
        td.usage = TGT_DEPTH | TGT_SAMPLED;
        h->shadow = target_create(h->gpu, &td);
        if (!HANDLE_VALID(h->shadow)) { fprintf(stderr, "shadow target: %s\n", gpu_last_error(h->gpu)); return false; }
    }

    color_format = info.swapchain_format;
    if (!windowed) {
        /* sRGB, like the window's swapchain: the shaders output linear colour,
         * so the PPM is what the window shows. */
        memset(&td, 0, sizeof td);
        td.name = "slang_toon_out";
        td.width = h->width; td.height = h->height;
        td.format = FMT_RGBA8_SRGB;
        td.usage = TGT_COLOR | TGT_SAMPLED;
        td.samples = SC_1;
        td.mips = 1u;
        td.cls = RES_PERSISTENT;
        h->color = target_create(h->gpu, &td);
        if (!HANDLE_VALID(h->color)) { fprintf(stderr, "color target: %s\n", gpu_last_error(h->gpu)); return false; }
        color_format = FMT_RGBA8_SRGB;
    }

    if (!build_scene(h)) { fprintf(stderr, "scene: %s\n", gpu_last_error(h->gpu)); return false; }

    /* ---- shaders + pipelines -------------------------------------------- */
    if (!shader_load(h->gpu, "demos/slang-toon/spirv/bindless/vertMain.spv", STAGE_VERTEX, &vs_main) ||
        !shader_load(h->gpu, "demos/slang-toon/spirv/bindless/vertOutline.spv", STAGE_VERTEX, &vs_outline) ||
        !shader_load(h->gpu, "demos/slang-toon/spirv/bindless/vertShadow.spv", STAGE_VERTEX, &vs_shadow) ||
        !shader_load(h->gpu, "demos/slang-toon/spirv/bindless/fragDepth.spv", STAGE_FRAGMENT, &fs_depth) ||
        !shader_load(h->gpu, forward_fs[o->model], STAGE_FRAGMENT, &fs_forward) ||
        !shader_load(h->gpu, outline_fs[o->model], STAGE_FRAGMENT, &fs_outline) ||
        !shader_load(h->gpu, "demos/toon/shaders/sky.vert.spv", STAGE_VERTEX, &vs_sky) ||
        !shader_load(h->gpu, "demos/toon/shaders/sky.frag.spv", STAGE_FRAGMENT, &fs_sky)) {
        fprintf(stderr, "shader: %s\n", gpu_last_error(h->gpu));
        return false;
    }

    h->sky_pipe     = pipe_intern_graphics(h->gpu, vs_sky, fs_sky, color_format, FMT_NONE, BLEND_OPAQUE);
    h->forward_pipe = pipe_intern_graphics(h->gpu, vs_main, fs_forward, color_format, FMT_D32F, BLEND_OPAQUE);
    h->outline_pipe = pipe_intern_graphics(h->gpu, vs_outline, fs_outline, color_format, FMT_D32F, BLEND_OPAQUE);
    {
        /* Depth-only: no colour attachment at all, so the key is spelled out
         * (pipe_intern_graphics always declares one colour format). */
        PipeKey key;
        memset(&key, 0, sizeof key);
        key.vs = vs_shadow;
        key.fs = fs_depth;
        key.color_count = 0u;
        key.depth = FMT_D32F;
        h->shadow_pipe = pipe_intern(h->gpu, &key);
    }
    if (!HANDLE_VALID(h->sky_pipe) || !HANDLE_VALID(h->forward_pipe) ||
        !HANDLE_VALID(h->outline_pipe) || !HANDLE_VALID(h->shadow_pipe)) {
        fprintf(stderr, "pipe: %s\n", gpu_last_error(h->gpu));
        return false;
    }

    /* ---- frame program -------------------------------------------------- */
    sky_color    = attach_clear(windowed ? TARGET_SWAPCHAIN : h->color, 0.0f, 0.0f, 0.0f, 1.0f);
    keep_color   = attach_color(windowed ? TARGET_SWAPCHAIN : h->color, ATT_WRITE_KEEP);
    depth_clear  = attach_depth_clear(h->depth, 1.0f);
    depth_read   = attach_color(h->depth, ATT_DEPTH_READ);
    depth_read.clear[0] = 1.0f;
    reads[0]     = h->shadow;
    memset(passes, 0, sizeof passes);

    if (o->shadows) {
        PassDesc *pass = &passes[pass_count++];
        shadow_clear = attach_depth_clear(h->shadow, 1.0f);
        strcpy(pass->name, "ShadowMap");
        pass->kind   = PASS_GRAPHICS;
        pass->pipe   = h->shadow_pipe;
        /* No culling: the caster stage biases along the normal, and thin or
         * open geometry should still cast. */
        pass->state  = draw_state_depth_only(SC_1);
        pass->state.cull = CULL_NONE;
        pass->depth  = &shadow_clear;
        pass->record = record_shadow;
    }
    {
        PassDesc *pass = &passes[pass_count++];
        strcpy(pass->name, "Backdrop");
        pass->kind        = PASS_GRAPHICS;
        pass->pipe        = h->sky_pipe;
        pass->state       = draw_state_opaque(SC_1, CULL_NONE);
        pass->colors      = &sky_color;
        pass->color_count = 1u;
        pass->record      = record_sky;
    }
    {
        PassDesc *pass = &passes[pass_count++];
        strcpy(pass->name, "ToonForward");
        pass->kind        = PASS_GRAPHICS;
        pass->pipe        = h->forward_pipe;
        pass->state       = draw_state_opaque(SC_1, CULL_BACK);
        pass->colors      = &keep_color;
        pass->color_count = 1u;
        pass->depth       = &depth_clear;
        pass->record      = record_forward;
        if (o->shadows) { pass->tex_reads = reads; pass->tex_read_count = 1u; }
    }
    if (o->outline) {
        /* Inverted hull: front faces culled, tested against the forward depth,
         * not written (same arrangement as demos/toon). The outline stage
         * shades like the surface and multiplies by the outline colour, as
         * upstream does, so it samples the shadow map too. */
        PassDesc *pass = &passes[pass_count++];
        strcpy(pass->name, "Outline");
        pass->kind              = PASS_GRAPHICS;
        pass->pipe              = h->outline_pipe;
        pass->state             = draw_state_opaque(SC_1, CULL_FRONT);
        pass->state.depth_write = 0u;
        pass->colors            = &keep_color;
        pass->color_count       = 1u;
        pass->depth             = &depth_read;
        pass->record            = record_outline;
        if (o->shadows) { pass->tex_reads = reads; pass->tex_read_count = 1u; }
    }

    h->program = frame_program_build(h->gpu, passes, pass_count);
    if (!h->program) { fprintf(stderr, "program: %s\n", gpu_last_error(h->gpu)); return false; }

    printf("slang-toon host: %s (%ux%u, %s), model=%s, %u objects, %u vertices\n",
           info.device_name, h->width, h->height, windowed ? "windowed" : "headless",
           model_name(o->model), h->scene.object_count, h->scene.vertex_count);
    printf("features: shadows=%d outline=%d smooth-normals=%d pixel-outline=%d style=%d "
           "point-light=%d flags=0x%02x passes=%u\n",
           o->shadows, o->outline, (h->flags & TOON_FLAG_SMOOTH_OUTLINE_NORMAL) != 0u,
           (h->flags & TOON_FLAG_OUTLINE_PIXEL_WIDTH) != 0u,
           (h->flags & TOON_FLAG_MATERIAL_STYLE) != 0u, o->point_light, h->flags, pass_count);
    return true;
}

static void destroy_host(Host *h) {
    if (h->gpu) {
        if (h->program) frame_program_destroy(h->gpu, h->program);
        if (HANDLE_VALID(h->nearest)) sampler_destroy(h->gpu, h->nearest);
        if (HANDLE_VALID(h->linear_repeat)) sampler_destroy(h->gpu, h->linear_repeat);
        if (HANDLE_VALID(h->linear_clamp)) sampler_destroy(h->gpu, h->linear_clamp);
        if (HANDLE_VALID(h->checker)) texture_destroy(h->gpu, h->checker);
        if (HANDLE_VALID(h->shadow)) target_destroy(h->gpu, h->shadow);
        if (HANDLE_VALID(h->depth)) target_destroy(h->gpu, h->depth);
        if (HANDLE_VALID(h->color)) target_destroy(h->gpu, h->color);
        gpu_destroy(h->gpu);
    }
    if (h->platform) platform_destroy(h->platform);
    toon_scene_free(&h->scene);
    free(h->blob);
    memset(h, 0, sizeof *h);
}

static bool write_ppm(const char *path, const uint8_t *px, uint32_t w, uint32_t h) {
    FILE *f = fopen(path, "wb");
    uint32_t i;
    if (!f) return false;
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (i = 0u; i < w * h; ++i) fwrite(px + i * 4u, 1u, 3u, f);
    fclose(f);
    return true;
}

static bool render_headless(Host *h, const char *out_path) {
    const size_t bytes = (size_t)h->width * h->height * 4u;
    uint8_t *pixels;
    Readback *rb;
    uint32_t i, center;
    bool ok;

    for (i = 0u; i < 2u; ++i) {
        if (!frame_begin(h->gpu)) { fprintf(stderr, "frame_begin: %s\n", gpu_last_error(h->gpu)); return false; }
        frame_run(h->gpu, h->program, h);
        frame_end(h->gpu);
    }
    pixels = (uint8_t *)calloc(1u, bytes);
    if (!pixels) return false;
    rb = readback_request(h->gpu, target_texture(h->gpu, h->color), DATA_SPAN(pixels, bytes));
    /* readback_ready(blocking) waits one second per call. A software rasterizer
     * needs longer than that for the shadow-mapped frame, so ask again. */
    ok = false;
    for (i = 0u; rb && !ok && i < 60u; ++i) ok = readback_ready(h->gpu, rb, true);
    if (!ok) {
        fprintf(stderr, "readback: %s\n", gpu_last_error(h->gpu));
        free(pixels);
        return false;
    }
    center = ((h->height / 2u) * h->width + h->width / 2u) * 4u;
    printf("center=%u %u %u %u\n", (unsigned)pixels[center], (unsigned)pixels[center + 1u],
           (unsigned)pixels[center + 2u], (unsigned)pixels[center + 3u]);
    ok = write_ppm(out_path, pixels, h->width, h->height);
    if (ok) printf("wrote %s\n", out_path);
    readback_release(h->gpu, rb);
    free(pixels);
    return ok;
}

static double now_seconds(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1.0e-9;
}

static void usage(const char *argv0, FILE *f) {
    fprintf(f,
        "usage: %s [--headless [out.ppm]] [--out FILE] [--size W H]\n"
        "          [--model tutorial|extended|inspired] [--inspired]\n"
        "          [--camera DISTANCE YAW PITCH] [--fov DEGREES] [--no-orbit]\n"
        "          [--outline-width X] [--point-light]\n"
        "          [--v1] [--no-shadows] [--no-outline] [--no-smooth-normals]\n"
        "          [--no-pixel-outline] [--no-style] [--no-shadow-flip]\n\n"
        "Without --headless a window opens (Esc quits). --out only names the file:\n"
        "it does not select headless mode.\n\n"
        "  --v1                 the same scene with every ABI v2 feature off: no\n"
        "                       shadow map, split outline normals, upstream outline\n"
        "                       width, no style block (the 'before' image)\n"
        "  --outline-width X    pixels with pixel outlines, upstream units otherwise\n"
        "  --no-shadow-flip     debug: sample the shadow map without\n"
        "                       TOON_FLAG_SHADOW_FLIP_V (shadows land mirrored)\n",
        argv0);
}

int main(int argc, char **argv) {
    Host h;
    bool windowed = true, orbit = true;
    const char *out_path = "slang_toon.ppm";
    int i;

    memset(&h, 0, sizeof h);
    h.opt.model = MODEL_EXTENDED;
    h.opt.shadows = h.opt.outline = h.opt.smooth_normals = true;
    h.opt.pixel_outline = h.opt.style = h.opt.shadow_flip_v = true;
    h.opt.outline_width = -1.0f;
    h.opt.distance = 6.6f; h.opt.yaw = 0.42f; h.opt.pitch = 0.20f; h.opt.fov_deg = 50.0f;
    h.opt.width = WIN_W; h.opt.height = WIN_H;

    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--headless")) {
            windowed = false;
            if (i + 1 < argc && argv[i + 1][0] != '-') out_path = argv[++i];
        } else if (!strcmp(argv[i], "--out") && i + 1 < argc) {
            out_path = argv[++i];
        } else if (!strcmp(argv[i], "--size") && i + 2 < argc) {
            h.opt.width = (uint32_t)strtoul(argv[++i], NULL, 10);
            h.opt.height = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "--model") && i + 1 < argc) {
            const char *m = argv[++i];
            if (!strcmp(m, "tutorial")) h.opt.model = MODEL_TUTORIAL;
            else if (!strcmp(m, "extended")) h.opt.model = MODEL_EXTENDED;
            else if (!strcmp(m, "inspired")) h.opt.model = MODEL_INSPIRED;
            else { usage(argv[0], stderr); return 2; }
        } else if (!strcmp(argv[i], "--inspired")) {
            h.opt.model = MODEL_INSPIRED;
        } else if (!strcmp(argv[i], "--camera") && i + 3 < argc) {
            h.opt.distance = strtof(argv[++i], NULL);
            h.opt.yaw = strtof(argv[++i], NULL);
            h.opt.pitch = strtof(argv[++i], NULL);
        } else if (!strcmp(argv[i], "--fov") && i + 1 < argc) {
            h.opt.fov_deg = strtof(argv[++i], NULL);
        } else if (!strcmp(argv[i], "--outline-width") && i + 1 < argc) {
            h.opt.outline_width = strtof(argv[++i], NULL);
        } else if (!strcmp(argv[i], "--point-light")) {
            h.opt.point_light = true;
        } else if (!strcmp(argv[i], "--no-orbit")) {
            orbit = false;
        } else if (!strcmp(argv[i], "--v1")) {
            h.opt.shadows = h.opt.smooth_normals = h.opt.pixel_outline = h.opt.style = false;
        } else if (!strcmp(argv[i], "--no-shadows")) {
            h.opt.shadows = false;
        } else if (!strcmp(argv[i], "--no-outline")) {
            h.opt.outline = false;
        } else if (!strcmp(argv[i], "--no-smooth-normals")) {
            h.opt.smooth_normals = false;
        } else if (!strcmp(argv[i], "--no-pixel-outline")) {
            h.opt.pixel_outline = false;
        } else if (!strcmp(argv[i], "--no-style")) {
            h.opt.style = false;
        } else if (!strcmp(argv[i], "--no-shadow-flip")) {
            h.opt.shadow_flip_v = false;
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(argv[0], stdout);
            return 0;
        } else {
            usage(argv[0], stderr);
            return 2;
        }
    }
    if (h.opt.width == 0u || h.opt.height == 0u) { usage(argv[0], stderr); return 2; }

    if (!init_host(&h, windowed)) {
        destroy_host(&h);
        return 1;
    }

    if (!windowed) {
        const bool ok = render_headless(&h, out_path);
        destroy_host(&h);
        return ok ? 0 : 1;
    }

    printf("controls: Esc quits\n");
    {
        const double start = now_seconds();
        const BufSlice frame_slice = {h.slice.buffer, h.slice.offset + h.off_frame,
                                      sizeof(FrameData)};
        while (!platform_should_close(h.platform)) {
            platform_poll(h.platform);
            if (platform_key_pressed(h.platform, MU_KEY_ESCAPE)) break;
            if (orbit) {
                /* Per-frame data goes through the upload path, before
                 * frame_begin: only the 448-byte FrameData changes. */
                Upload *upload;
                fill_frame(&h, h.opt.yaw + (float)(now_seconds() - start) * 0.25f);
                upload = upload_begin(h.gpu);
                if (upload) {
                    upload_buffer(upload, frame_slice,
                                  DATA_SPAN(h.blob + h.off_frame, sizeof(FrameData)));
                    upload_end(h.gpu, upload);
                }
            }
            if (frame_begin(h.gpu)) {
                frame_run(h.gpu, h.program, &h);
                frame_end(h.gpu);
            }
        }
    }
    destroy_host(&h);
    return 0;
}
