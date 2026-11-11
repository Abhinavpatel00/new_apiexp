/* main.c — toon shading demo for the mu_gfx Vulkan API.
 *
 * A port of ColinLeung-NiloCat/UnityURPToonLitShaderExample
 * (SimpleURPToonLitOutlineExample.shader, MIT) from Unity URP HLSL to Slang on
 * vk.h. The shader's job is unchanged: one cel-shaded directional light, a flat
 * indirect term, and a second "inverted hull" pass that draws the outline.
 * What changed is everything around it — render graph, push constants,
 * bindless vertex fetch — so demos/toon/README.md carries the mapping table
 * between the URP properties and this file's knobs.
 *
 * Layout mirrors demos/galaxy/main.c: main.c owns scene state and recording and
 * never includes GLFW or names a key code; demos/common/platform.c owns the
 * window and mu_input.h.
 */

#include "mu_math.h"
#include "vk.h"

#include "nk_vk.h"
#include "platform.h"
#include "scene.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- input vocabulary ---------------------------------------------------- */

typedef enum Action {
    ACT_QUIT,
    ACT_PAUSE,
    ACT_ORBIT_LEFT,
    ACT_ORBIT_RIGHT,
    ACT_PITCH_UP,
    ACT_PITCH_DOWN,
    ACT_ZOOM_IN,
    ACT_ZOOM_OUT,
    ACT_LIGHT_LEFT,
    ACT_LIGHT_RIGHT,
    ACT_OUTLINE_TOGGLE,
    ACT_RESET,
    ACT_LOOK_DRAG,
    ACT_COUNT
} Action;

static const char *const k_action_names[ACT_COUNT] = {
    "quit", "pause", "orbit left", "orbit right", "pitch up", "pitch down",
    "zoom in", "zoom out", "light left", "light right", "toggle outline",
    "reset", "look",
};

static const char *const k_default_bindings[ACT_COUNT] = {
    "Escape", "P", "A", "D", "Up", "Down",
    "W", "S", "Q", "E", "O",
    "R", "MouseRight",
};

/* ---- fixed scene configuration ------------------------------------------ */

#define WIDTH       1280u
#define HEIGHT      720u
#define SHADER_DIR  "demos/toon/shaders/"
/* The Nuklear overlay shader is shared by every demo (demos/common/shaders/nk.slang). */
#define NK_SHADER_DIR "demos/common/shaders/"
#define FOV_DEGREES 60.0f
#define NEAR_PLANE  0.1f
#define FAR_PLANE   120.0f
#define CAM_TARGET_Y 1.15f
#define HEAD_INDEX   2u /* the object _IsFace applies to */

/* ---- push constants ------------------------------------------------------
 * ToonPush is the lit pass' per-draw payload: exactly 256 bytes, which is
 * vk.h's whole push-constant range. The vertex fetch offset is the only part
 * that changes per object, but the model matrix does too, so each object is one
 * push + one non-indexed draw.
 * ------------------------------------------------------------------------ */

typedef struct ALIGNAS(16) ToonPush {
    mu_mat4  view_proj;
    mu_mat4  model;
    mu_vec4  camera;       /* xyz eye, w fov degrees                       */
    mu_vec4  light_dir;    /* xyz towards light, w intensity               */
    mu_vec4  light_color;  /* rgb colour, w _DirectLightMultiplier         */
    mu_vec4  base_color;   /* rgb albedo, a alpha                          */
    mu_vec4  shadow_color; /* rgb _ShadowMapColor, w _IndirectLightMultiplier */
    mu_vec4  ambient_min;  /* rgb _IndirectLightMinColor, w _IsFace        */
    mu_vec4  cel;          /* x _CelShadeMidPoint, y _CelShadeSoftness, z rim */
    uint32_t meta[4];      /* x vertex byte offset, y TOON_OBJ_* flags      */
} ToonPush;

typedef struct ALIGNAS(16) OutlinePush {
    mu_mat4 view_proj;
    mu_mat4 model;
    mu_vec4  params; /* x width, y z-offset, z near, w far */
    mu_vec4  camera; /* xyz eye, w fov degrees             */
    mu_vec4  color;  /* rgb outline colour, a alpha        */
    mu_vec4  meta;   /* x vertex byte offset, y width scale */
} OutlinePush;

typedef struct ALIGNAS(16) SkyPush {
    mu_vec4 horizon;
    mu_vec4 zenith;
    mu_vec4 sun;
    mu_vec4 params;
} SkyPush;

typedef struct ALIGNAS(16) ResolvePush {
    uint32_t texture_index;
    uint32_t sampler_index;
    float    exposure;
    float    pad;
} ResolvePush;

_Static_assert(sizeof(ToonPush) == 256u, "ToonPush must be exactly the 256-byte push range");
_Static_assert(sizeof(OutlinePush) == 192u, "OutlinePush std430 layout");
_Static_assert(sizeof(SkyPush) == 64u, "SkyPush std430 layout");
_Static_assert(sizeof(ResolvePush) == 16u, "ResolvePush std430 layout");

typedef struct Camera {
    float yaw, pitch, distance;
} Camera;

typedef struct Demo {
    Gpu          *gpu;
    FrameProgram *program;

    Target   color_target;
    Target   depth_target;
    Texture  color_texture;
    Sampler  sampler;
    Pipe     sky_pipe, toon_pipe, outline_pipe, resolve_pipe;
    uint32_t color_index, sampler_index;

    uint32_t width, height;

    ToonScene scene;
    BufSlice  vertex_slice;
    BufUse    vertex_use;

    /* Material + light knobs. Defaults are the tutorial shader's defaults
     * unless the comment says otherwise. */
    float cel_mid;        /* _CelShadeMidPoint  = -0.5 */
    float cel_soft;       /* _CelShadeSoftness  =  0.05 */
    float rim;            /* extension, 0 disables */
    float direct_mul;     /* _DirectLightMultiplier */
    float indirect_mul;   /* _IndirectLightMultiplier */
    float shadow_color[4];/* _ShadowMapColor */
    float ambient_min[4]; /* _IndirectLightMinColor */
    float outline_color[4];
    float outline_width;  /* _OutlineWidth    = 1 */
    float outline_zoffset;/* _OutlineZOffset  = 1e-4 */
    float exposure;
    float light_yaw, light_elev, light_intensity;

    bool show_outline;
    bool head_flat; /* port of _IsFace: the head ignores the cel band */
    bool auto_orbit;
    bool paused;
    bool validate; /* --validate: Vulkan validation + sync layers */

    Camera   camera;
    float    time;

    Platform *platform;
    uint64_t  frame;
    double    last_report;

    NkVk  *nkvk;
    BufUse ui_buf_use;
} Demo;

static void camera_matrices(const Demo *d, mu_mat4 view_proj, mu_vec4 eye) {
    mu_vec3 pos, target, world_up;
    mu_mat4 view, proj, vp;

    target[0] = 0.0f;
    target[1] = CAM_TARGET_Y;
    target[2] = 0.0f;
    mu_vec3_make(pos,
                 target[0] + d->camera.distance * cosf(d->camera.pitch) * sinf(d->camera.yaw),
                 target[1] + d->camera.distance * sinf(d->camera.pitch),
                 target[2] + d->camera.distance * cosf(d->camera.pitch) * cosf(d->camera.yaw));
    mu_vec3_make(world_up, 0.0f, 1.0f, 0.0f);
    mu_mat4_look_at(view, pos, target, world_up);
    mu_mat4_perspective_zo(proj, mu_radians(FOV_DEGREES), (float)d->width / (float)d->height,
                           NEAR_PLANE, FAR_PLANE);
    mu_mat4_mul(proj, view, vp);
    memcpy(view_proj, vp, sizeof vp);
    mu_vec4_make(eye, pos[0], pos[1], pos[2], FOV_DEGREES);
}

static void light_direction(const Demo *d, mu_vec3 out) {
    const float ce = cosf(d->light_elev);
    out[0] = ce * sinf(d->light_yaw);
    out[1] = sinf(d->light_elev);
    out[2] = ce * cosf(d->light_yaw);
}

/* ---- recording ---------------------------------------------------------- */

static void record_sky(void *user, PassCtx *p) {
    Demo    *d = (Demo *)user;
    SkyPush  push;
    mu_vec4  eye;
    mu_mat4  view_proj;
    mu_vec3  light;
    mu_vec4  clip;

    camera_matrices(d, view_proj, eye);
    light_direction(d, light);

    /* Project a point 40 units along the light direction: that is where the sun
     * sits in the backdrop. Screen space, so no skybox and no cubemap. */
    {
        mu_vec4 point, out;
        mu_vec4_make(point, eye[0] + light[0] * 40.0f,
                     eye[1] + light[1] * 40.0f,
                     eye[2] + light[2] * 40.0f, 1.0f);
        mu_mat4_mulv(out, view_proj, point);
        clip[0] = out[0] / out[3];
        clip[1] = out[1] / out[3];
        clip[2] = out[3] > 0.0f ? 1.0f : 0.0f; /* in front of the camera */
    }

    mu_vec4_make(push.horizon, 0.74f, 0.68f, 0.80f, 1.8f);
    mu_vec4_make(push.zenith, 0.20f, 0.26f, 0.46f, 0.55f);
    mu_vec4_make(push.sun, 0.5f + clip[0] * 0.5f, 0.5f + clip[1] * 0.5f,
                 0.055f, clip[2]);
    mu_vec4_zero(push.params);

    if (pass_scope_begin(p, "backdrop", SH_FRAG)) {
        pass_push(p, &push, sizeof push);
        pass_draw_fullscreen_triangle(p, d->width, d->height);
        (void)pass_scope_end(p);
    }
}

static void draw_toon_object(Demo *d, PassCtx *p, const ToonObject *object,
                             const mu_mat4 view_proj, const mu_vec4 eye,
                             const mu_vec4 light) {
    /* _IsFace: the tutorial lerps the cel band away for face meshes, because
     * NoL banding across a face reads badly. Here it is a toggle on the head. */
    const float is_face = (d->head_flat && object == &d->scene.objects[HEAD_INDEX]) ? 1.0f : 0.0f;
    ToonPush push;

    memset(&push, 0, sizeof push);
    memcpy(push.view_proj, view_proj, sizeof push.view_proj);
    memcpy(push.model, object->model, sizeof push.model);
    mu_vec4_copy(push.camera, eye);
    mu_vec4_copy(push.light_dir, light);
    mu_vec4_make(push.light_color, 1.0f, 0.97f, 0.92f, d->direct_mul);
    mu_vec4_copy(push.base_color, object->color);
    mu_vec4_make(push.shadow_color, d->shadow_color[0], d->shadow_color[1],
                 d->shadow_color[2], d->indirect_mul);
    mu_vec4_make(push.ambient_min, d->ambient_min[0], d->ambient_min[1],
                 d->ambient_min[2], is_face);
    mu_vec4_make(push.cel, d->cel_mid, d->cel_soft, d->rim, 0.0f);
    push.meta[0] = (uint32_t)d->vertex_slice.offset +
                   object->first_vertex * (uint32_t)sizeof(ToonVertex);
    push.meta[1] = object->flags;

    pass_push(p, &push, sizeof push);
    pass_draw(p, object->vertex_count, 1u, 0u, 0u);
}

static void record_toon(void *user, PassCtx *p) {
    Demo   *d = (Demo *)user;
    mu_mat4 view_proj;
    mu_vec4 eye, light;
    mu_vec3 light_dir;
    uint32_t i;

    camera_matrices(d, view_proj, eye);
    light_direction(d, light_dir);
    mu_vec4_make(light, light_dir[0], light_dir[1], light_dir[2], d->light_intensity);

    if (pass_scope_begin(p, "toon forward", SH_VERT | SH_FRAG)) {
        for (i = 0; i < d->scene.object_count; ++i)
            draw_toon_object(d, p, &d->scene.objects[i], view_proj, eye, light);
        (void)pass_scope_end(p);
    }
}

static void record_outline(void *user, PassCtx *p) {
    Demo     *d = (Demo *)user;
    mu_mat4   view_proj;
    mu_vec4   eye;
    uint32_t  i;

    /* The pass stays in the program to keep one graph for the whole run; with
     * the toggle off it records nothing and costs an empty render pass. */
    if (!d->show_outline)
        return;

    camera_matrices(d, view_proj, eye);

    if (pass_scope_begin(p, "outline hull", SH_VERT | SH_FRAG)) {
        for (i = 0; i < d->scene.object_count; ++i) {
            const ToonObject *object = &d->scene.objects[i];
            OutlinePush       push;

            if (object->flags & TOON_OBJ_NO_OUTLINE)
                continue;
            memset(&push, 0, sizeof push);
            memcpy(push.view_proj, view_proj, sizeof push.view_proj);
            memcpy(push.model, object->model, sizeof push.model);
            mu_vec4_make(push.params, d->outline_width, d->outline_zoffset,
                         NEAR_PLANE, FAR_PLANE);
            mu_vec4_copy(push.camera, eye);
            mu_vec4_copy(push.color, d->outline_color);
            mu_vec4_make(push.meta,
                         (float)(d->vertex_slice.offset +
                                 (uint64_t)object->first_vertex * sizeof(ToonVertex)),
                         object->outline_scale, 0.0f, 0.0f);
            pass_push(p, &push, sizeof push);
            pass_draw(p, object->vertex_count, 1u, 0u, 0u);
        }
        (void)pass_scope_end(p);
    }
}

static void record_resolve(void *user, PassCtx *p) {
    Demo        *d = (Demo *)user;
    ResolvePush  push;
    push.texture_index = d->color_index;
    push.sampler_index = d->sampler_index;
    push.exposure      = d->exposure;
    push.pad           = 0.0f;
    if (pass_scope_begin(p, "resolve", SH_FRAG)) {
        pass_push(p, &push, sizeof push);
        pass_draw_fullscreen_triangle(p, d->width, d->height);
        (void)pass_scope_end(p);
    }
}

static void record_ui(void *user, PassCtx *p) {
    Demo *d = (Demo *)user;
    if (d->nkvk)
        nkvk_draw(d->nkvk, p);
}

/* ---- setup -------------------------------------------------------------- */

static bool init_gpu(Demo *d, bool windowed) {
    GpuDesc    desc;
    GpuInfo    info;
    GpuTables  tables;
    ShaderId   sky_vs, sky_fs, toon_vs, toon_fs, outline_vs, outline_fs, resolve_fs;
    TargetDesc target_desc;
    SamplerDesc sampler_desc;
    AttachDesc sky_color, toon_color, outline_color, resolve_color;
    AttachDesc depth_clear, depth_read;
    Target     reads[1];
    PassDesc   passes[5];
    Upload    *upload;

    memset(&desc, 0, sizeof desc);
    desc.width            = d->width;
    desc.height           = d->height;
    desc.frames_in_flight = 3u;
    desc.app_name         = "mu_gfx toon";
    desc.vsync            = true;
    desc.validation       = d->validate;
    d->gpu                = gpu_create(&desc);
    if (!d->gpu)
        return false;

    if (windowed) {
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        if (!platform_create_surface(d->platform, gpu_raw_instance(d->gpu), &surface))
            return false;
        if (!set_surface(d->gpu, surface)) {
            vkDestroySurfaceKHR(gpu_raw_instance(d->gpu), surface, NULL);
            fprintf(stderr, "[toon] set_surface: %s\n", gpu_last_error(d->gpu));
            return false;
        }
    }
    gpu_info(d->gpu, &info);
    d->width  = info.extent_w;
    d->height = info.extent_h;
    printf("[toon] device: %s (%ux%u)\n", info.device_name, d->width, d->height);

    /* ---- scene geometry: one arena slice, one upload --------------------- */
    if (!toon_scene_build(&d->scene)) {
        fprintf(stderr, "[toon] scene build failed\n");
        return false;
    }
    {
        const uint64_t bytes = (uint64_t)d->scene.vertex_count * sizeof(ToonVertex);
        d->vertex_slice      = arena_alloc(d->gpu, ARENA_DEVICE, bytes, 64u);
        if (!HANDLE_VALID(d->vertex_slice.buffer)) {
            fprintf(stderr, "[toon] arena_alloc: %s\n", gpu_last_error(d->gpu));
            return false;
        }
        upload = upload_begin(d->gpu);
        if (!upload) {
            fprintf(stderr, "[toon] upload_begin: %s\n", gpu_last_error(d->gpu));
            return false;
        }
        upload_buffer(upload, d->vertex_slice, DATA_SPAN(d->scene.vertices, bytes));
        upload_end(d->gpu, upload);

        gpu_tables(d->gpu, &tables);
        d->vertex_use = use_read(tables.storage_buffers.buffer,
                                 d->vertex_slice.offset, d->vertex_slice.size);
    }

    /* ---- targets -------------------------------------------------------- */
    memset(&target_desc, 0, sizeof target_desc);
    target_desc.name    = "toon_color";
    target_desc.width   = d->width;
    target_desc.height  = d->height;
    target_desc.format  = FMT_RGBA16F; /* linear; alpha is unused */
    target_desc.usage   = TGT_COLOR | TGT_SAMPLED;
    target_desc.samples = SC_1;
    target_desc.mips    = 1u;
    target_desc.cls     = RES_PERSISTENT;
    d->color_target     = target_create(d->gpu, &target_desc);
    if (!HANDLE_VALID(d->color_target)) {
        fprintf(stderr, "[toon] colour target: %s\n", gpu_last_error(d->gpu));
        return false;
    }
    d->color_texture = target_texture(d->gpu, d->color_target);
    if (!texture_info(d->gpu, d->color_texture, &d->color_index))
        return false;

    memset(&target_desc, 0, sizeof target_desc);
    target_desc.name    = "toon_depth";
    target_desc.width   = d->width;
    target_desc.height  = d->height;
    target_desc.format  = FMT_D32F;
    target_desc.usage   = TGT_DEPTH;
    target_desc.samples = SC_1;
    target_desc.mips    = 1u;
    target_desc.cls     = RES_PERSISTENT;
    d->depth_target     = target_create(d->gpu, &target_desc);
    if (!HANDLE_VALID(d->depth_target)) {
        fprintf(stderr, "[toon] depth target: %s\n", gpu_last_error(d->gpu));
        return false;
    }

    sampler_desc     = sampler_linear_clamp();
    d->sampler       = sampler_create(d->gpu, &sampler_desc);
    if (!HANDLE_VALID(d->sampler)) {
        fprintf(stderr, "[toon] sampler: %s\n", gpu_last_error(d->gpu));
        return false;
    }
    d->sampler_index = sampler_bindless_index(d->gpu, d->sampler);

    /* ---- shaders + pipelines -------------------------------------------- */
    if (!shader_load(d->gpu, SHADER_DIR "sky.vert.spv", STAGE_VERTEX, &sky_vs) ||
        !shader_load(d->gpu, SHADER_DIR "sky.frag.spv", STAGE_FRAGMENT, &sky_fs) ||
        !shader_load(d->gpu, SHADER_DIR "toon.vert.spv", STAGE_VERTEX, &toon_vs) ||
        !shader_load(d->gpu, SHADER_DIR "toon.frag.spv", STAGE_FRAGMENT, &toon_fs) ||
        !shader_load(d->gpu, SHADER_DIR "outline.vert.spv", STAGE_VERTEX, &outline_vs) ||
        !shader_load(d->gpu, SHADER_DIR "outline.frag.spv", STAGE_FRAGMENT, &outline_fs) ||
        !shader_load(d->gpu, SHADER_DIR "resolve.frag.spv", STAGE_FRAGMENT, &resolve_fs)) {
        fprintf(stderr, "[toon] shader load: %s\n", gpu_last_error(d->gpu));
        return false;
    }

    d->sky_pipe     = pipe_intern_graphics(d->gpu, sky_vs, sky_fs, FMT_RGBA16F, FMT_NONE, BLEND_OPAQUE);
    d->toon_pipe    = pipe_intern_graphics(d->gpu, toon_vs, toon_fs, FMT_RGBA16F, FMT_D32F, BLEND_OPAQUE);
    d->outline_pipe = pipe_intern_graphics(d->gpu, outline_vs, outline_fs, FMT_RGBA16F, FMT_D32F, BLEND_OPAQUE);
    d->resolve_pipe = pipe_intern_graphics(d->gpu, sky_vs, resolve_fs, info.swapchain_format, FMT_NONE, BLEND_OPAQUE);
    if (!HANDLE_VALID(d->sky_pipe) || !HANDLE_VALID(d->toon_pipe) ||
        !HANDLE_VALID(d->outline_pipe) || !HANDLE_VALID(d->resolve_pipe)) {
        fprintf(stderr, "[toon] pipeline: %s\n", gpu_last_error(d->gpu));
        return false;
    }

    /* ---- frame program -------------------------------------------------- */
    sky_color     = attach_clear(d->color_target, 0.0f, 0.0f, 0.0f, 1.0f);
    toon_color    = attach_color(d->color_target, ATT_WRITE_KEEP);
    outline_color = attach_color(d->color_target, ATT_WRITE_KEEP);
    resolve_color = attach_color(TARGET_SWAPCHAIN, ATT_WRITE);
    depth_clear   = attach_depth_clear(d->depth_target, 1.0f);
    depth_read    = attach_color(d->depth_target, ATT_DEPTH_READ);
    depth_read.clear[0] = 1.0f;
    reads[0]      = d->color_target;
    memset(passes, 0, sizeof passes);

    strcpy(passes[0].name, "Backdrop");
    passes[0].kind        = PASS_GRAPHICS;
    passes[0].pipe        = d->sky_pipe;
    passes[0].state       = draw_state_opaque(SC_1, CULL_NONE);
    passes[0].colors      = &sky_color;
    passes[0].color_count = 1u;
    passes[0].record      = record_sky;

    /* Forward: depth cleared here (ATT_DEPTH_CLEAR) because the backdrop does
     * not touch depth at all. */
    strcpy(passes[1].name, "ToonForward");
    passes[1].kind           = PASS_GRAPHICS;
    passes[1].pipe           = d->toon_pipe;
    passes[1].state          = draw_state_opaque(SC_1, CULL_BACK);
    passes[1].colors         = &toon_color;
    passes[1].color_count    = 1u;
    passes[1].depth          = &depth_clear;
    passes[1].buf_reads      = &d->vertex_use;
    passes[1].buf_read_count = 1u;
    passes[1].record         = record_toon;

    /* Outline: front faces culled, depth tested against the pass above but not
     * written (ATT_DEPTH_READ = load + read-only layout). */
    strcpy(passes[2].name, "Outline");
    passes[2].kind              = PASS_GRAPHICS;
    passes[2].pipe              = d->outline_pipe;
    passes[2].state             = draw_state_opaque(SC_1, CULL_FRONT);
    passes[2].state.depth_write = 0u;
    passes[2].colors            = &outline_color;
    passes[2].color_count       = 1u;
    passes[2].depth             = &depth_read;
    passes[2].buf_reads         = &d->vertex_use;
    passes[2].buf_read_count    = 1u;
    passes[2].record            = record_outline;

    strcpy(passes[3].name, "Resolve");
    passes[3].kind           = PASS_GRAPHICS;
    passes[3].pipe           = d->resolve_pipe;
    passes[3].state          = draw_state_opaque(SC_1, CULL_NONE);
    passes[3].colors         = &resolve_color;
    passes[3].color_count    = 1u;
    passes[3].tex_reads      = reads;
    passes[3].tex_read_count = 1u;
    passes[3].record         = record_resolve;

    if (windowed) {
        AttachDesc ui_color = attach_color(TARGET_SWAPCHAIN, ATT_WRITE_KEEP);
        d->nkvk = nkvk_create(d->gpu, NK_SHADER_DIR "nk.vert.spv", NK_SHADER_DIR "nk.frag.spv",
                              info.swapchain_format, d->width, d->height);
        if (!d->nkvk) {
            fprintf(stderr, "[toon] debug UI: %s\n", gpu_last_error(d->gpu));
            return false;
        }
        d->ui_buf_use = nkvk_buf_use(d->nkvk);
        strcpy(passes[4].name, "DebugUI");
        passes[4].kind           = PASS_GRAPHICS;
        passes[4].pipe           = nkvk_pipe(d->nkvk);
        passes[4].state          = draw_state_opaque(SC_1, CULL_NONE);
        passes[4].state.blend    = BLEND_ALPHA;
        passes[4].colors         = &ui_color;
        passes[4].color_count    = 1u;
        passes[4].buf_reads      = &d->ui_buf_use;
        passes[4].buf_read_count = 1u;
        passes[4].record         = record_ui;
        d->program = frame_program_build(d->gpu, passes, 5u);
    } else {
        d->program = frame_program_build(d->gpu, passes, 4u);
    }
    if (!d->program) {
        fprintf(stderr, "[toon] frame program: %s\n", gpu_last_error(d->gpu));
        return false;
    }
    return true;
}

/* ---- per-frame animation ------------------------------------------------
 * Two objects move so the demo exercises a model matrix that changes every
 * frame (the figure stays put: its silhouette is the outline reference). In
 * headless mode --time picks the phase, so the same frame is reproducible.
 * ----------------------------------------------------------------------- */

#define BALL_INDEX  7u
#define CRATE_INDEX 8u

static void animate(Demo *d) {
    const float zaxis[3] = {0.0f, 0.0f, 1.0f};
    const float yaxis[3] = {0.0f, 1.0f, 0.0f};
    if (d->scene.object_count > BALL_INDEX) {
        const float t[3] = {-1.45f, 0.46f + 0.10f * sinf(d->time * 1.7f), 0.35f};
        toon_obj_set_transform(&d->scene.objects[BALL_INDEX], t, zaxis,
                               0.0f, 0.28f);
    }
    if (d->scene.object_count > CRATE_INDEX) {
        const float t[3] = {1.55f, 0.36f, -0.45f};
        toon_obj_set_transform(&d->scene.objects[CRATE_INDEX], t, yaxis,
                               0.42f + d->time * 0.25f, 0.36f);
    }
}

/* ---- simulation --------------------------------------------------------- */

#define ORBIT_RATE       0.9f
#define ZOOM_RATE        4.0f
#define PITCH_RATE       0.7f
#define LIGHT_RATE       1.4f
#define LOOK_SENSITIVITY 0.005f

static void update(Demo *d, const MuInputActions *in, const MuInputState *raw,
                   bool ui_visible) {
    const float dt = raw->dt;

    if (mu_input_action_pressed(in, ACT_PAUSE))
        d->paused = !d->paused;
    if (mu_input_action_pressed(in, ACT_OUTLINE_TOGGLE))
        d->show_outline = !d->show_outline;
    if (mu_input_action_pressed(in, ACT_RESET)) {
        d->cel_mid = 0.25f; d->cel_soft = 0.04f; d->rim = 0.15f;
        d->direct_mul = 1.1f; d->indirect_mul = 1.0f;
        d->outline_width = 1.0f; d->outline_zoffset = 0.0001f;
        d->exposure = 1.0f; d->light_yaw = 1.60f; d->light_elev = 0.70f;
        d->show_outline = true;
        d->head_flat    = false;
        d->camera.yaw = 0.55f; d->camera.pitch = 0.16f; d->camera.distance = 5.1f;
    }

    d->camera.yaw   += (float)mu_input_action_down(in, ACT_ORBIT_LEFT) * ORBIT_RATE * dt;
    d->camera.yaw   -= (float)mu_input_action_down(in, ACT_ORBIT_RIGHT) * ORBIT_RATE * dt;
    d->camera.pitch += (float)mu_input_action_down(in, ACT_PITCH_UP) * PITCH_RATE * dt;
    d->camera.pitch -= (float)mu_input_action_down(in, ACT_PITCH_DOWN) * PITCH_RATE * dt;
    d->camera.distance -= (float)mu_input_action_down(in, ACT_ZOOM_IN) * ZOOM_RATE * dt;
    d->camera.distance += (float)mu_input_action_down(in, ACT_ZOOM_OUT) * ZOOM_RATE * dt;
    d->light_yaw += (float)mu_input_action_down(in, ACT_LIGHT_LEFT) * LIGHT_RATE * dt;
    d->light_yaw -= (float)mu_input_action_down(in, ACT_LIGHT_RIGHT) * LIGHT_RATE * dt;

    if (mu_input_action_down(in, ACT_LOOK_DRAG)) {
        d->camera.yaw -= in->mouse_dx * LOOK_SENSITIVITY;
        d->camera.pitch += in->mouse_dy * LOOK_SENSITIVITY;
    }
    if (in->wheel_y != 0.0f && !ui_visible)
        d->camera.distance *= powf(0.88f, in->wheel_y);

    d->camera.pitch    = mu_clamp(d->camera.pitch, -0.35f, 1.20f);
    d->camera.distance = mu_clamp(d->camera.distance, 2.6f, 24.0f);

    if (!d->paused) {
        d->time += dt;
        if (d->auto_orbit)
            d->camera.yaw += 0.10f * dt;
    }
}

/* ---- headless output ---------------------------------------------------- */

static float half_to_float(uint16_t h) {
    uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
    uint32_t exp  = (h >> 10) & 0x1fu;
    uint32_t mant = h & 0x03ffu;
    uint32_t bits;
    float    value;
    if (!exp) {
        if (!mant)
            bits = sign;
        else {
            int shift = 0;
            while ((mant & 0x0400u) == 0u) {
                mant <<= 1;
                ++shift;
            }
            mant &= 0x03ffu;
            bits = sign | ((uint32_t)(127 - 14 - shift) << 23) | (mant << 13);
        }
    } else if (exp == 31u)
        bits = sign | 0x7f800000u | (mant << 13);
    else
        bits = sign | ((exp + (127u - 15u)) << 23) | (mant << 13);
    memcpy(&value, &bits, sizeof value);
    return value;
}

static uint8_t encode_srgb(float linear) {
    float v;
    if (!(linear > 0.0f))
        return 0u;
    v = linear <= 0.0031308f ? 12.92f * linear : 1.055f * powf(linear, 1.0f / 2.4f) - 0.055f;
    if (v > 1.0f)
        v = 1.0f;
    return (uint8_t)(v * 255.0f + 0.5f);
}

/* Same transfer function the sRGB swapchain applies in windowed mode, minus the
 * attachment: the resolve pass clamps, this encodes. */
static bool write_ppm(const char *path, const uint16_t *pixels, uint32_t width,
                      uint32_t height, float exposure) {
    FILE    *f = fopen(path, "wb");
    uint32_t x, y;
    if (!f)
        return false;
    fprintf(f, "P6\n%u %u\n255\n", width, height);
    for (y = 0; y < height; ++y)
        for (x = 0; x < width; ++x) {
            const uint16_t *p = &pixels[((size_t)y * width + x) * 4u];
            uint8_t         rgb[3];
            uint32_t        c;
            for (c = 0u; c < 3u; ++c)
                rgb[c] = encode_srgb(half_to_float(p[c]) * exposure);
            if (fwrite(rgb, 1u, 3u, f) != 3u) {
                fclose(f);
                return false;
            }
        }
    fclose(f);
    return true;
}

static bool render_one_frame(Demo *d) {
    if (!frame_begin(d->gpu))
        return false;
    frame_run(d->gpu, d->program, d);
    frame_end(d->gpu);
    return true;
}

static bool render_headless(Demo *d, const char *path) {
    const size_t bytes  = (size_t)d->width * d->height * 8u;
    uint16_t    *pixels = (uint16_t *)malloc(bytes);
    Readback    *readback;

    if (!pixels)
        return false;
    if (!render_one_frame(d)) {
        free(pixels);
        return false;
    }
    readback = readback_request(d->gpu, d->color_texture, DATA_SPAN(pixels, bytes), 0u);
    if (!readback || !readback_ready(d->gpu, readback, true)) {
        fprintf(stderr, "[toon] readback failed: %s\n", gpu_last_error(d->gpu));
        if (readback)
            readback_release(d->gpu, readback);
        free(pixels);
        return false;
    }
    readback_release(d->gpu, readback);
    if (!write_ppm(path, pixels, d->width, d->height, d->exposure)) {
        fprintf(stderr, "[toon] could not write %s\n", path);
        free(pixels);
        return false;
    }
    free(pixels);
    printf("[toon] wrote %s (%ux%u, %u objects, %u vertices)\n", path, d->width,
           d->height, d->scene.object_count, d->scene.vertex_count);
    perf_dump(d->gpu, "demos/toon/toon_perf.txt");
    return true;
}

/* ---- debug UI ----------------------------------------------------------- */

static void color_row(struct nk_context *ctx, const char *label, float rgba[4]) {
    struct nk_colorf color;
    color.r = rgba[0];
    color.g = rgba[1];
    color.b = rgba[2];
    color.a = rgba[3];
    nk_layout_row_dynamic(ctx, 18, 1);
    nk_label(ctx, label, NK_TEXT_LEFT);
    nk_layout_row_dynamic(ctx, 92, 1);
    color = nk_color_picker(ctx, color, NK_RGB);
    rgba[0] = color.r;
    rgba[1] = color.g;
    rgba[2] = color.b;
}

static void build_debug_ui(Demo *d, float dt) {
    struct nk_context *ctx;
    if (!d->nkvk || !nkvk_visible(d->nkvk))
        return;
    ctx = nkvk_context(d->nkvk);
    if (nk_begin(ctx, "Toon", nk_rect(10, 10, 300, 660),
                 NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE |
                     NK_WINDOW_TITLE)) {
        nk_layout_row_dynamic(ctx, 20, 1);
        nk_label(ctx, "F1 / ` toggles this panel", NK_TEXT_LEFT);

        nk_layout_row_dynamic(ctx, 22, 2);
        nk_label(ctx, "Cel midpoint", NK_TEXT_LEFT);
        nk_slider_float(ctx, -1.0f, &d->cel_mid, 1.0f, 0.01f);
        nk_layout_row_dynamic(ctx, 22, 2);
        nk_label(ctx, "Cel softness", NK_TEXT_LEFT);
        nk_slider_float(ctx, 0.001f, &d->cel_soft, 0.5f, 0.001f);
        nk_layout_row_dynamic(ctx, 22, 2);
        nk_label(ctx, "Direct light", NK_TEXT_LEFT);
        nk_slider_float(ctx, 0.0f, &d->direct_mul, 2.0f, 0.01f);
        nk_layout_row_dynamic(ctx, 22, 2);
        nk_label(ctx, "Indirect light", NK_TEXT_LEFT);
        nk_slider_float(ctx, 0.0f, &d->indirect_mul, 2.0f, 0.01f);
        nk_layout_row_dynamic(ctx, 22, 2);
        nk_label(ctx, "Rim", NK_TEXT_LEFT);
        nk_slider_float(ctx, 0.0f, &d->rim, 1.0f, 0.01f);

        nk_layout_row_dynamic(ctx, 22, 2);
        nk_label(ctx, "Outline width", NK_TEXT_LEFT);
        nk_slider_float(ctx, 0.0f, &d->outline_width, 4.0f, 0.01f);
        nk_layout_row_dynamic(ctx, 22, 2);
        nk_label(ctx, "Outline z-offset", NK_TEXT_LEFT);
        nk_slider_float(ctx, 0.0f, &d->outline_zoffset, 0.02f, 0.0001f);

        nk_layout_row_dynamic(ctx, 22, 2);
        nk_label(ctx, "Light yaw", NK_TEXT_LEFT);
        nk_slider_float(ctx, -3.2f, &d->light_yaw, 3.2f, 0.01f);
        nk_layout_row_dynamic(ctx, 22, 2);
        nk_label(ctx, "Light height", NK_TEXT_LEFT);
        nk_slider_float(ctx, -0.5f, &d->light_elev, 1.5f, 0.01f);
        nk_layout_row_dynamic(ctx, 22, 2);
        nk_label(ctx, "Light intensity", NK_TEXT_LEFT);
        nk_slider_float(ctx, 0.0f, &d->light_intensity, 3.0f, 0.01f);
        nk_layout_row_dynamic(ctx, 22, 2);
        nk_label(ctx, "Exposure", NK_TEXT_LEFT);
        nk_slider_float(ctx, 0.2f, &d->exposure, 2.0f, 0.01f);

        color_row(ctx, "Shadow colour (_ShadowMapColor)", d->shadow_color);
        color_row(ctx, "Ambient minimum", d->ambient_min);
        color_row(ctx, "Outline colour", d->outline_color);

        nk_layout_row_dynamic(ctx, 22, 1);
        {
            nk_bool value = d->show_outline ? nk_true : nk_false;
            nk_checkbox_label(ctx, "Outline pass", &value);
            d->show_outline = value ? true : false;
        }
        nk_layout_row_dynamic(ctx, 22, 1);
        {
            nk_bool value = d->head_flat ? nk_true : nk_false;
            nk_checkbox_label(ctx, "Head ignores cel band (_IsFace)", &value);
            d->head_flat = value ? true : false;
        }
        nk_layout_row_dynamic(ctx, 22, 1);
        {
            nk_bool value = d->auto_orbit ? nk_true : nk_false;
            nk_checkbox_label(ctx, "Auto orbit", &value);
            d->auto_orbit = value ? true : false;
        }
        nk_layout_row_dynamic(ctx, 22, 1);
        if (nk_button_label(ctx, "Reset (R)")) {
            d->cel_mid = 0.25f; d->cel_soft = 0.04f; d->rim = 0.15f;
            d->direct_mul = 1.1f; d->indirect_mul = 1.0f;
            d->outline_width = 1.0f; d->outline_zoffset = 0.0001f;
            d->exposure = 1.0f;
        }
        (void)dt;
    }
    nk_end(ctx);
}

/* ---- teardown, bindings, CLI -------------------------------------------- */

static void cleanup(Demo *d) {
    if (d->gpu) {
        if (d->nkvk) {
            nkvk_destroy(d->nkvk, d->gpu);
            d->nkvk = NULL;
        }
        if (d->program)
            frame_program_destroy(d->gpu, d->program);
        if (HANDLE_VALID(d->sampler))
            sampler_destroy(d->gpu, d->sampler);
        if (HANDLE_VALID(d->depth_target))
            target_destroy(d->gpu, d->depth_target);
        if (HANDLE_VALID(d->color_target))
            target_destroy(d->gpu, d->color_target);
        gpu_destroy(d->gpu);
        d->gpu = NULL;
    }
    if (d->platform) {
        platform_destroy(d->platform);
        d->platform = NULL;
    }
    toon_scene_free(&d->scene);
}

static bool setup_bindings(Platform *platform) {
    MuInputMap *map = platform_map(platform);
    uint32_t    action;
    for (action = 0; action < ACT_COUNT; ++action)
        if (!platform_bind_control_name(map, action, k_default_bindings[action]))
            return false;
    return true;
}

static void print_bindings(const MuInputMap *map, FILE *file) {
    char     scratch[32];
    uint32_t action;
    for (action = 0; action < ACT_COUNT; ++action) {
        fputs("  ", file);
        fprintf(file, "%-14s", k_action_names[action]);
        if (action < map->action_count && map->source_count[action] > 0u) {
            const MuInputSource *src = &map->sources[action][0];
            if (src->kind == MU_INPUT_SRC_CONTROL)
                fputs(platform_slot_label(src->code, scratch, sizeof scratch), file);
            else
                fprintf(file, "pad %u", (unsigned)src->code);
        } else {
            fputs("unbound", file);
        }
        fputc('\n', file);
    }
}

static void print_declared_bindings(FILE *file) {
    uint32_t action;
    for (action = 0; action < ACT_COUNT; ++action)
        fprintf(file, "  %-14s %s\n", k_action_names[action], k_default_bindings[action]);
}

static void usage(const char *argv0, FILE *file) {
    fprintf(file,
            "Usage: %s [--headless [output.ppm]] [--size W H] [--time seconds]\n"
            "       %s [--no-outline] [--outline width zoffset] [--no-orbit]\n"
            "       %s [--cel midpoint softness] [--light yaw elev]\n"
            "       %s [--validate]\n"
            "       %s [--bindings]\n\n"
            "Port of ColinLeung-NiloCat/UnityURPToonLitShaderExample to Vulkan.\n"
            "Default is an interactive window; --headless renders one frame\n"
            "offscreen and writes a PPM image.\n\nControls (positions on a US\n"
            "layout; labels are shown for your own layout in the window):\n",
            argv0, argv0, argv0, argv0, argv0);
    print_declared_bindings(file);
}

int main(int argc, char **argv) {
    Demo     d;
    bool     windowed     = true;
    const char *output    = "toon.ppm";
    uint32_t i;
    bool     show_usage   = false;
    bool     show_binding = false;
    bool     bad_args     = false;

    memset(&d, 0, sizeof d);
    d.width           = WIDTH;
    d.height          = HEIGHT;
    d.cel_mid         = 0.25f;
    d.cel_soft        = 0.04f;
    d.rim             = 0.15f;
    d.direct_mul      = 1.1f;
    d.indirect_mul    = 1.0f;
    d.outline_width   = 1.0f;
    d.outline_zoffset = 0.0001f;
    d.exposure        = 1.0f;
    d.light_yaw       = 1.60f;
    d.light_elev      = 0.70f;
    d.light_intensity = 1.0f;
    d.show_outline    = true;
    d.auto_orbit      = true;
    d.camera.yaw      = 0.55f;
    d.camera.pitch    = 0.16f;
    d.camera.distance = 5.1f;
    mu_vec4_make(d.shadow_color, 0.40f, 0.35f, 0.56f, 1.0f);
    mu_vec4_make(d.ambient_min, 0.10f, 0.11f, 0.20f, 1.0f);
    mu_vec4_make(d.outline_color, 0.10f, 0.08f, 0.13f, 1.0f);

    for (i = 1u; i < (uint32_t)argc; ++i) {
        if (!strcmp(argv[i], "--headless")) {
            windowed = false;
            if (i + 1u < (uint32_t)argc && argv[i + 1u][0] != '-')
                output = argv[++i];
        } else if (!strcmp(argv[i], "--size") && i + 2u < (uint32_t)argc) {
            d.width  = (uint32_t)strtoul(argv[++i], NULL, 10);
            d.height = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "--time") && i + 1u < (uint32_t)argc) {
            d.time = strtof(argv[++i], NULL);
        } else if (!strcmp(argv[i], "--cel") && i + 2u < (uint32_t)argc) {
            d.cel_mid  = strtof(argv[++i], NULL);
            d.cel_soft = strtof(argv[++i], NULL);
        } else if (!strcmp(argv[i], "--light") && i + 2u < (uint32_t)argc) {
            d.light_yaw  = strtof(argv[++i], NULL);
            d.light_elev = strtof(argv[++i], NULL);
        } else if (!strcmp(argv[i], "--outline") && i + 2u < (uint32_t)argc) {
            d.outline_width   = strtof(argv[++i], NULL);
            d.outline_zoffset = strtof(argv[++i], NULL);
        } else if (!strcmp(argv[i], "--no-outline")) {
            d.show_outline = false;
        } else if (!strcmp(argv[i], "--validate")) {
            d.validate = true;
        } else if (!strcmp(argv[i], "--no-orbit")) {
            d.auto_orbit = false;
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            show_usage = true;
        } else if (!strcmp(argv[i], "--bindings")) {
            show_binding = true;
        } else {
            bad_args = true;
        }
    }
    if (bad_args) {
        usage(argv[0], stderr);
        return 2;
    }
    if (show_usage) {
        usage(argv[0], stdout);
        return 0;
    }
    if (show_binding) {
        print_declared_bindings(stdout);
        return 0;
    }
    if (!d.width || !d.height || d.width > 4096u || d.height > 4096u) {
        fprintf(stderr, "[toon] invalid output extent\n");
        return 2;
    }
    if (d.cel_soft <= 0.0f) {
        fprintf(stderr, "[toon] --cel softness must be > 0\n");
        return 2;
    }

    if (windowed) {
        d.platform = platform_create(d.width, d.height, "Toon - URP toon lit port");
        if (!d.platform)
            return 1;
        if (!setup_bindings(d.platform))
            return 1;
        printf("[toon] bindings for your layout:\n");
        print_bindings(platform_map(d.platform), stdout);
    }

    if (!init_gpu(&d, windowed)) {
        cleanup(&d);
        return 1;
    }

    if (!windowed) {
        animate(&d);
        const bool ok = render_headless(&d, output);
        cleanup(&d);
        return ok ? 0 : 1;
    }

    printf("[toon] controls: A/D orbit, W/S zoom, up/down pitch, right-drag look, "
           "Q/E light yaw, O outline on/off, P pause, R reset, F1 panel, Esc quit\n");

    while (!platform_should_close(d.platform)) {
        const MuInputState   *raw;
        const MuInputActions *in;
        const bool            ui_visible = d.nkvk && nkvk_visible(d.nkvk);

        platform_poll(d.platform);
        raw = platform_state(d.platform);
        in  = platform_actions(d.platform);

        if (platform_key_pressed(d.platform, MU_KEY_ESCAPE) ||
            mu_input_action_pressed(in, ACT_QUIT))
            break;
        if (platform_key_pressed(d.platform, MU_KEY_F1) ||
            platform_key_pressed(d.platform, MU_KEY_GRAVE)) {
            if (d.nkvk)
                nkvk_toggle_visible(d.nkvk);
        }
        if (d.nkvk)
            nkvk_new_frame(d.nkvk, raw, raw->dt);

        update(&d, in, raw, ui_visible);
        if (d.nkvk) {
            build_debug_ui(&d, raw->dt);
            nkvk_prepare(d.nkvk, d.gpu, d.width, d.height);
        }
        animate(&d);
        if (frame_begin(d.gpu)) {
            frame_run(d.gpu, d.program, &d);
            frame_end(d.gpu);
        }
        ++d.frame;
        {
            const double now = (double)raw->time_ns * 1e-9;
            if (now - d.last_report > 2.0) {
                perf_dump(d.gpu, NULL);
                d.last_report = now;
            }
        }
    }
    cleanup(&d);
    return 0;
}
