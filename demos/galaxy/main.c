#include "mu_math.h"
#include "vk.h"

#include "platform.h"
#include "nk_vk.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The game's vocabulary. Ids index the runtime binding map; the controls they
 * resolve to are data set up in setup_bindings(). */
typedef enum Action {
    ACT_QUIT,
    ACT_PAUSE,
    ACT_RESUME,
    ACT_RESEED,
    ACT_SPRINT,
    ACT_ORBIT_LEFT,
    ACT_ORBIT_RIGHT,
    ACT_PITCH_UP,
    ACT_PITCH_DOWN,
    ACT_ZOOM_IN,
    ACT_ZOOM_OUT,
    ACT_SPEED_DOWN,
    ACT_SPEED_UP,
    ACT_BRIGHTNESS_DOWN,
    ACT_BRIGHTNESS_UP,
    ACT_LOOK_DRAG,
    ACT_COUNT
} Action;

static const char *const k_action_names[ACT_COUNT] = {
    "quit",       "pause",   "resume",   "reseed",    "sprint",   "orbit left", "orbit right", "pitch up",
    "pitch down", "zoom in", "zoom out", "slow down", "speed up", "dimmer",     "brighter",    "look",
};

/* Defaults are positions ("W" is the key at W's position), never key codes. */
static const char *const k_default_bindings[ACT_COUNT] = {
    "Escape", "P", "Space", "R",           "LeftShift",    "A",     "D",     "Up",
    "Down",   "W", "S",     "LeftBracket", "RightBracket", "Minus", "Equal", "MouseRight",
};

#define WIDTH         1280u
#define HEIGHT        720u
#define STAR_COUNT    120000u
#define DUST_COUNT    14000u
#define CORE_COUNT    1u
#define GALAXY_RADIUS 20.0f
#define SHADER_DIR    "demos/galaxy/shaders/"
/* The Nuklear overlay shader is shared by every demo (demos/common/shaders/nk.slang). */
#define NK_SHADER_DIR "demos/common/shaders/"

typedef struct ALIGNAS(16) SkyPush {
    mu_vec4 right;
    mu_vec4 up;
    mu_vec4 forward;
    mu_vec4 params; /* aspect, tan-half-fov, time, sky intensity */
} SkyPush;

typedef struct ALIGNAS(16) GalaxyPush {
    mu_mat4  view_proj;
    mu_vec4  camera_right;
    mu_vec4  camera_up;
    mu_vec4  params; /* time, radius, thickness, arm twist */
    mu_vec4  tint_core;
    mu_vec4  tint_arm;
    mu_vec4  params2; /* star size, brightness, dust size, core size */
    uint32_t meta[4]; /* layer, seed, reserved, reserved */
} GalaxyPush;

typedef struct ALIGNAS(16) TonePush {
    uint32_t texture_index;
    uint32_t sampler_index;
    float    exposure;
    float    pad;
} TonePush;

_Static_assert(sizeof(SkyPush) == 64u, "SkyPush std430 layout");
_Static_assert(sizeof(GalaxyPush) == 176u, "GalaxyPush std430 layout");
_Static_assert(sizeof(TonePush) == 16u, "TonePush std430 layout");

typedef struct Camera {
    float yaw, pitch, distance;
} Camera;

/* Fixed-step sim (game-engine-dod: accumulator + clamped steps). Render
 * runs at display rate; sim (camera/time/brightness) runs at SIM_HZ. */
#define SIM_HZ    60
#define SIM_DT    (1.0f / (float)SIM_HZ)
#define MAX_STEPS 5

typedef struct Demo {
    Gpu          *gpu;
    FrameProgram *program;
    Target        hdr_target;
    Texture       hdr_texture;
    Sampler       sampler;
    Pipe          sky_pipe, galaxy_pipe, tone_pipe;
    uint32_t      hdr_index, sampler_index;
    uint32_t      width, height;
    uint32_t      star_count, dust_count, seed;
    float         time, speed, twist, star_size, brightness, dust_size, thickness;
    float         core_size, sky_gain, exposure;
    bool          paused;
    Camera        camera;
    Platform     *platform;
    uint64_t      frame;
    double        last_report;
    /* input v2: fixed-step tick state */
    float         accumulator;
    uint32_t      tick;
    MuInputFrame  input_frame;
    MuInputChords chords;
    /* Nuklear debug UI (windowed only; NULL headless) */
    NkVk         *nkvk;
    BufUse        ui_buf_use; /* storage for PassDesc (must outlive build) */
} Demo;

static void camera_matrices(const Demo *d, mu_mat4 view_proj, mu_vec4 right, mu_vec4 up, mu_vec4 forward) {
    mu_vec3 pos, target, world_up;
    mu_mat4 view, proj, vp;

    mu_vec3_make(pos, d->camera.distance * cosf(d->camera.pitch) * sinf(d->camera.yaw),
                 d->camera.distance * sinf(d->camera.pitch),
                 d->camera.distance * cosf(d->camera.pitch) * cosf(d->camera.yaw));
    mu_vec3_zero(target);
    mu_vec3_make(world_up, 0.0f, 1.0f, 0.0f);
    mu_mat4_look_at(view, pos, target, world_up);
    /* Right-handed, column-major, Vulkan depth [0, 1]. Keep the existing
     * Y convention; the renderer already handles viewport orientation. */
    mu_mat4_perspective_zo(proj, mu_radians(60.0f), (float)d->width / (float)d->height, 0.1f, 150.0f);
    mu_mat4_mul(proj, view, vp);
    memcpy(view_proj, vp, sizeof vp);
    /* The view matrix' columns are the world-space camera axes. */
    mu_vec4_make(right, view[0], view[4], view[8], 0.0f);
    mu_vec4_make(up, view[1], view[5], view[9], 0.0f);
    mu_vec4_make(forward, -view[2], -view[6], -view[10], 0.0f);
}

static void record_sky(void *user, PassCtx *p) {
    Demo   *d = (Demo *)user;
    SkyPush push;
    mu_mat4 view_proj;
    mu_vec4 right, up, forward;
    camera_matrices(d, view_proj, right, up, forward);
    mu_vec4_copy(push.right, right);
    mu_vec4_copy(push.up, up);
    mu_vec4_copy(push.forward, forward);
    mu_vec4_make(push.params, (float)d->width / (float)d->height, tanf(mu_radians(30.0f)), d->time, d->sky_gain);
    if (pass_scope_begin(p, "procedural sky", SH_FRAG)) {
        pass_push(p, &push, sizeof push);
        pass_draw_fullscreen_triangle(p, d->width, d->height);
        (void)pass_scope_end(p);
    }
}

static void draw_galaxy_layer(Demo *d, PassCtx *p, const float view_proj[16], const float right[4], const float up[4],
                              uint32_t layer, uint32_t instances, const char *scope_name) {
    GalaxyPush push;
    memset(&push, 0, sizeof push);
    memcpy(push.view_proj, view_proj, sizeof push.view_proj);
    mu_vec4_copy(push.camera_right, right);
    mu_vec4_copy(push.camera_up, up);
    mu_vec4_make(push.params, d->time, GALAXY_RADIUS, d->thickness, d->twist);
    mu_vec4_make(push.tint_core, 1.0f, 0.72f, 0.36f, 1.0f);
    mu_vec4_make(push.tint_arm, 0.42f, 0.66f, 1.0f, 1.0f);
    mu_vec4_make(push.params2, d->star_size, d->brightness, d->dust_size, d->core_size);
    push.meta[0] = layer;
    push.meta[1] = d->seed;
    if (pass_scope_begin(p, scope_name, SH_VERT | SH_FRAG)) {
        pass_push(p, &push, sizeof push);
        pass_draw(p, 6u, instances, 0u, 0u);
        (void)pass_scope_end(p);
    }
}

static void record_galaxy(void *user, PassCtx *p) {
    Demo   *d = (Demo *)user;
    mu_mat4 view_proj;
    mu_vec4 right, up, forward;
    camera_matrices(d, view_proj, right, up, forward);
    (void)forward;
    draw_galaxy_layer(d, p, view_proj, right, up, 0u, d->star_count, "stars");
    draw_galaxy_layer(d, p, view_proj, right, up, 1u, d->dust_count, "dust");
    draw_galaxy_layer(d, p, view_proj, right, up, 2u, CORE_COUNT, "core glow");
}

static void record_tonemap(void *user, PassCtx *p) {
    Demo    *d = (Demo *)user;
    TonePush push;
    push.texture_index = d->hdr_index;
    push.sampler_index = d->sampler_index;
    push.exposure      = d->exposure;
    push.pad           = 0.0f;
    if (pass_scope_begin(p, "HDR tone map", SH_FRAG)) {
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

static bool init_gpu(Demo *d, bool windowed) {
    GpuDesc     desc;
    GpuInfo     info;
    ShaderId    sky_vs, sky_fs, galaxy_vs, galaxy_fs, tone_fs;
    TargetDesc  target_desc;
    SamplerDesc sampler_desc;
    AttachDesc  sky_color, galaxy_color, tone_color;
    Target      reads[1];
    PassDesc    passes[4];

    memset(&desc, 0, sizeof desc);
    desc.width            = d->width;
    desc.height           = d->height;
    desc.frames_in_flight = 3u;
    desc.app_name         = "mu_gfx galaxy";
    desc.vsync            = true;
    desc.validation       = false;
    d->gpu                = gpu_create(&desc);
    if (!d->gpu)
        return false;

    if (windowed) {
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        if (!platform_create_surface(d->platform, gpu_raw_instance(d->gpu), &surface))
            return false;
        if (!set_surface(d->gpu, surface)) {
            vkDestroySurfaceKHR(gpu_raw_instance(d->gpu), surface, NULL);
            fprintf(stderr, "[galaxy] set_surface: %s\n", gpu_last_error(d->gpu));
            return false;
        }
    }
    gpu_info(d->gpu, &info);
    d->width  = info.extent_w;
    d->height = info.extent_h;
    printf("[galaxy] device: %s (%ux%u)\n", info.device_name, d->width, d->height);

    memset(&target_desc, 0, sizeof target_desc);
    target_desc.name    = "galaxy_hdr";
    target_desc.width   = d->width;
    target_desc.height  = d->height;
    target_desc.format  = FMT_RGBA16F;
    target_desc.usage   = TGT_COLOR | TGT_SAMPLED;
    target_desc.samples = SC_1;
    target_desc.mips    = 1u;
    target_desc.cls     = RES_PERSISTENT;
    d->hdr_target       = target_create(d->gpu, &target_desc);
    if (!HANDLE_VALID(d->hdr_target)) {
        fprintf(stderr, "[galaxy] HDR target: %s\n", gpu_last_error(d->gpu));
        return false;
    }
    d->hdr_texture = target_texture(d->gpu, d->hdr_target);
    if (!texture_info(d->gpu, d->hdr_texture, &d->hdr_index))
        return false;

    sampler_desc = sampler_linear_clamp();
    d->sampler   = sampler_create(d->gpu, &sampler_desc);
    if (!HANDLE_VALID(d->sampler)) {
        fprintf(stderr, "[galaxy] sampler: %s\n", gpu_last_error(d->gpu));
        return false;
    }
    d->sampler_index = sampler_bindless_index(d->gpu, d->sampler);

    if (!shader_load(d->gpu, SHADER_DIR "fullscreen.vert.spv", STAGE_VERTEX, &sky_vs) ||
        !shader_load(d->gpu, SHADER_DIR "sky.frag.spv", STAGE_FRAGMENT, &sky_fs) ||
        !shader_load(d->gpu, SHADER_DIR "galaxy.vert.spv", STAGE_VERTEX, &galaxy_vs) ||
        !shader_load(d->gpu, SHADER_DIR "galaxy.frag.spv", STAGE_FRAGMENT, &galaxy_fs) ||
        !shader_load(d->gpu, SHADER_DIR "tonemap.frag.spv", STAGE_FRAGMENT, &tone_fs)) {
        fprintf(stderr, "[galaxy] shader load: %s\n", gpu_last_error(d->gpu));
        return false;
    }
    d->sky_pipe    = pipe_intern_graphics(d->gpu, sky_vs, sky_fs, FMT_RGBA16F, FMT_NONE, BLEND_OPAQUE);
    d->galaxy_pipe = pipe_intern_graphics(d->gpu, galaxy_vs, galaxy_fs, FMT_RGBA16F, FMT_NONE, BLEND_ADD);
    d->tone_pipe   = pipe_intern_graphics(d->gpu, sky_vs, tone_fs, info.swapchain_format, FMT_NONE, BLEND_OPAQUE);
    if (!HANDLE_VALID(d->sky_pipe) || !HANDLE_VALID(d->galaxy_pipe) || !HANDLE_VALID(d->tone_pipe)) {
        fprintf(stderr, "[galaxy] pipeline: %s\n", gpu_last_error(d->gpu));
        return false;
    }

    sky_color    = attach_clear(d->hdr_target, 0.001f, 0.0015f, 0.006f, 1.0f);
    galaxy_color = attach_color(d->hdr_target, ATT_WRITE_KEEP);
    tone_color   = attach_color(TARGET_SWAPCHAIN, ATT_WRITE);
    reads[0]     = d->hdr_target;
    memset(passes, 0, sizeof passes);

    strcpy(passes[0].name, "GalaxySky");
    passes[0].kind        = PASS_GRAPHICS;
    passes[0].pipe        = d->sky_pipe;
    passes[0].state       = draw_state_opaque(SC_1, CULL_NONE);
    passes[0].colors      = &sky_color;
    passes[0].color_count = 1u;
    passes[0].record      = record_sky;

    strcpy(passes[1].name, "GalaxyStars");
    passes[1].kind        = PASS_GRAPHICS;
    passes[1].pipe        = d->galaxy_pipe;
    passes[1].state       = draw_state_opaque(SC_1, CULL_NONE);
    passes[1].state.blend = BLEND_ADD;
    passes[1].colors      = &galaxy_color;
    passes[1].color_count = 1u;
    passes[1].record      = record_galaxy;

    strcpy(passes[2].name, "ToneMap");
    passes[2].kind           = PASS_GRAPHICS;
    passes[2].pipe           = d->tone_pipe;
    passes[2].state          = draw_state_opaque(SC_1, CULL_NONE);
    passes[2].colors         = &tone_color;
    passes[2].color_count    = 1u;
    passes[2].tex_reads      = reads;
    passes[2].tex_read_count = 1u;
    passes[2].record         = record_tonemap;

    /* Debug UI (windowed only): Nuklear overlay on the swapchain. */
    if (windowed) {
        AttachDesc ui_color = attach_color(TARGET_SWAPCHAIN, ATT_WRITE_KEEP);
        d->nkvk = nkvk_create(d->gpu, NK_SHADER_DIR "nk.vert.spv", NK_SHADER_DIR "nk.frag.spv",
                              info.swapchain_format, d->width, d->height);
        if (!d->nkvk) {
            fprintf(stderr, "[galaxy] debug UI: %s\n", gpu_last_error(d->gpu));
            return false;
        }
        d->ui_buf_use = nkvk_buf_use(d->nkvk);
        strcpy(passes[3].name, "DebugUI");
        passes[3].kind           = PASS_GRAPHICS;
        passes[3].pipe           = nkvk_pipe(d->nkvk);
        passes[3].state          = draw_state_opaque(SC_1, CULL_NONE);
        passes[3].state.blend    = BLEND_ALPHA;
        passes[3].colors         = &ui_color;
        passes[3].color_count    = 1u;
        passes[3].buf_reads      = &d->ui_buf_use;
        passes[3].buf_read_count = 1u;
        passes[3].record         = record_ui;
        d->program = frame_program_build(d->gpu, passes, 4u);
    } else {
        d->program = frame_program_build(d->gpu, passes, 3u);
    }
    if (!d->program) {
        fprintf(stderr, "[galaxy] frame program: %s\n", gpu_last_error(d->gpu));
        return false;
    }
    return true;
}

#define ORBIT_RATE       0.9f /* radians per second at speed 1 */
#define ZOOM_RATE        8.0f /* world units per second at speed 1 */
#define PITCH_RATE       0.8f
#define BRIGHTNESS_RATE  0.8f
#define LOOK_SENSITIVITY 0.004f /* radians per pixel dragged */

/* Tick-snapshot axis: value(pos) - value(neg), analog-aware. */
static float frame_axis(const MuInputFrame *f, uint32_t neg, uint32_t pos) {
    return mu_input_frame_value(f, pos) - mu_input_frame_value(f, neg);
}

/* Sim (fixed step): orbit/zoom/pitch from the tick snapshot. dt is SIM_DT. */
static void update_camera_tick(Demo *d, const MuInputFrame *f, float dt) {
    const float speed = dt * (mu_input_frame_down(f, ACT_SPRINT) ? 1.8f : 1.0f);
    d->camera.yaw += frame_axis(f, ACT_ORBIT_LEFT, ACT_ORBIT_RIGHT) * ORBIT_RATE * speed;
    d->camera.distance += frame_axis(f, ACT_ZOOM_IN, ACT_ZOOM_OUT) * ZOOM_RATE * speed;
    d->camera.pitch += frame_axis(f, ACT_PITCH_DOWN, ACT_PITCH_UP) * PITCH_RATE * speed;
}

/* Presentation (once per render frame): mouse drag + wheel are per-frame
 * deltas, so they are applied here, not per sim tick (which would multiply
 * them by the step count). Reads actions only — no MuInputState. */
static void update_presentation(Demo *d, const MuInputActions *in) {
    if (mu_input_action_down(in, ACT_LOOK_DRAG)) {
        d->camera.yaw -= in->mouse_dx * LOOK_SENSITIVITY;
        d->camera.pitch += in->mouse_dy * LOOK_SENSITIVITY;
    }
    /* Wheel scrolls the debug UI when it is visible; otherwise zoom. */
    if (in->wheel_y != 0.0f && !(d->nkvk && nkvk_visible(d->nkvk))) {
        d->camera.distance *= powf(0.90f, in->wheel_y);
    }
}

/* Sim (fixed step): transport keys, pause/resume/reseed, brightness, time.
 * dt is SIM_DT; called 0..MAX_STEPS times per render frame. */
static void update_controls_tick(Demo *d, const MuInputFrame *f, float dt, uint64_t time_ns) {
    if (mu_input_frame_down(f, ACT_SPEED_DOWN))
        d->speed *= powf(0.7f, dt);
    if (mu_input_frame_down(f, ACT_SPEED_UP))
        d->speed *= powf(1.4f, dt);
    d->speed = mu_clamp(d->speed, 0.05f, 12.0f);

    if (mu_input_frame_pressed(f, ACT_PAUSE))
        d->paused = true;
    if (mu_input_frame_pressed(f, ACT_RESUME))
        d->paused = false;
    /* R alone reseeds; Shift+R (chord 0, 5-tick window) also reseeds. */
    if (mu_input_frame_pressed(f, ACT_RESEED) || mu_input_chord_pressed(f, 0u)) {
        d->seed = (uint32_t)((double)time_ns * 1e-9 * 100000.0) ^ 0xA5C39E17u;
    }

    update_camera_tick(d, f, dt);
    d->brightness += frame_axis(f, ACT_BRIGHTNESS_DOWN, ACT_BRIGHTNESS_UP) * BRIGHTNESS_RATE * dt;
    if (!d->paused)
        d->time += dt * d->speed;

    d->camera.pitch    = mu_clamp(d->camera.pitch, -1.25f, 1.25f);
    d->camera.distance = mu_clamp(d->camera.distance, 7.0f, 100.0f);
    d->brightness      = mu_clamp(d->brightness, 0.1f, 3.0f);
}

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

static bool write_hdr_ppm(const char *path, const uint16_t *pixels, uint32_t width, uint32_t height, float exposure) {
    FILE    *f = fopen(path, "wb");
    uint32_t x, y;
    if (!f)
        return false;
    fprintf(f, "P6\n%u %u\n255\n", width, height);
    for (y = 0; y < height; ++y)
        for (x = 0; x < width; ++x) {
            const uint16_t *p = &pixels[((size_t)y * width + x) * 4u];
            float           linear[3];
            uint8_t         rgb[3];
            uint32_t        c;
            for (c = 0u; c < 3u; ++c) {
                float hdr = half_to_float(p[c]) * exposure;
                linear[c] = 1.0f - expf(-hdr);
                rgb[c]    = encode_srgb(linear[c]);
            }
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
    size_t    bytes  = (size_t)d->width * d->height * 8u;
    uint16_t *pixels = (uint16_t *)malloc(bytes);
    Readback *readback;
    if (!pixels)
        return false;
    if (!render_one_frame(d)) {
        free(pixels);
        return false;
    }
    readback = readback_request(d->gpu, d->hdr_texture, DATA_SPAN(pixels, bytes));
    if (!readback || !readback_ready(d->gpu, readback, true)) {
        fprintf(stderr, "[galaxy] readback failed: %s\n", gpu_last_error(d->gpu));
        if (readback)
            readback_release(d->gpu, readback);
        free(pixels);
        return false;
    }
    readback_release(d->gpu, readback);
    if (!write_hdr_ppm(path, pixels, d->width, d->height, d->exposure)) {
        fprintf(stderr, "[galaxy] could not write %s\n", path);
        free(pixels);
        return false;
    }
    free(pixels);
    printf("[galaxy] wrote %s (%ux%u, %u stars, %u dust)\n", path, d->width, d->height, d->star_count, d->dust_count);
    perf_dump(d->gpu, "demos/galaxy/galaxy_perf.txt");
    return true;
}

/* Nuklear debug panel: sliders edit Demo fields directly. */
static void build_debug_ui(Demo *d, float dt) {
    struct nk_context *ctx;
    if (!d->nkvk || !nkvk_visible(d->nkvk))
        return;
    ctx = nkvk_context(d->nkvk);
    if (nk_begin(ctx, "Galaxy Debug", nk_rect(10, 10, 280, 420),
                 NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE |
                 NK_WINDOW_TITLE)) {
        nk_layout_row_dynamic(ctx, 22, 1);
        nk_label(ctx, "F1 / ` toggles this panel", NK_TEXT_LEFT);
        nk_layout_row_dynamic(ctx, 22, 2);
        nk_label(ctx, "Time scale", NK_TEXT_LEFT);
        nk_slider_float(ctx, 0.05f, &d->speed, 12.0f, 0.01f);
        nk_layout_row_dynamic(ctx, 22, 2);
        nk_label(ctx, "Brightness", NK_TEXT_LEFT);
        nk_slider_float(ctx, 0.1f, &d->brightness, 3.0f, 0.01f);
        nk_layout_row_dynamic(ctx, 22, 2);
        nk_label(ctx, "Exposure", NK_TEXT_LEFT);
        nk_slider_float(ctx, 0.1f, &d->exposure, 3.0f, 0.01f);
        nk_layout_row_dynamic(ctx, 22, 2);
        nk_label(ctx, "Star size", NK_TEXT_LEFT);
        nk_slider_float(ctx, 0.01f, &d->star_size, 0.2f, 0.001f);
        nk_layout_row_dynamic(ctx, 22, 2);
        nk_label(ctx, "Dust size", NK_TEXT_LEFT);
        nk_slider_float(ctx, 0.1f, &d->dust_size, 2.0f, 0.01f);
        nk_layout_row_dynamic(ctx, 22, 2);
        nk_label(ctx, "Thickness", NK_TEXT_LEFT);
        nk_slider_float(ctx, 0.1f, &d->thickness, 2.0f, 0.01f);
        nk_layout_row_dynamic(ctx, 22, 2);
        nk_label(ctx, "Twist", NK_TEXT_LEFT);
        nk_slider_float(ctx, 0.0f, &d->twist, 6.0f, 0.01f);
        nk_layout_row_dynamic(ctx, 22, 2);
        nk_label(ctx, "Core size", NK_TEXT_LEFT);
        nk_slider_float(ctx, 1.0f, &d->core_size, 20.0f, 0.1f);
        nk_layout_row_dynamic(ctx, 22, 2);
        nk_label(ctx, "Sky gain", NK_TEXT_LEFT);
        nk_slider_float(ctx, 0.0f, &d->sky_gain, 2.0f, 0.01f);
        nk_layout_row_dynamic(ctx, 22, 2);
        nk_label(ctx, "Distance", NK_TEXT_LEFT);
        nk_slider_float(ctx, 7.0f, &d->camera.distance, 100.0f, 0.1f);
        nk_layout_row_dynamic(ctx, 22, 1);
        {
            nk_bool paused = d->paused ? nk_true : nk_false;
            nk_checkbox_label(ctx, "Paused", &paused);
            d->paused = paused ? true : false;
        }
        nk_layout_row_dynamic(ctx, 22, 1);
        if (nk_button_label(ctx, "Reseed (R)"))
            d->seed = (uint32_t)(d->frame * 2654435761u) ^ 0xA5C39E17u;
        nk_layout_row_dynamic(ctx, 22, 1);
        nk_label(ctx, "WASD orbit, W/S zoom, drag look", NK_TEXT_LEFT);
        (void)dt;
    }
    nk_end(ctx);
}

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
        if (HANDLE_VALID(d->hdr_target))
            target_destroy(d->gpu, d->hdr_target);
        gpu_destroy(d->gpu);
        d->gpu = NULL;
    }
    if (d->platform) {
        platform_destroy(d->platform);
        d->platform = NULL;
    }
}

static bool setup_bindings(Platform *platform) {
    MuInputMap *map = platform_map(platform);
    uint32_t    action, i;

    for (action = 0; action < ACT_COUNT; ++action) {
        if (!platform_bind_control_name(map, action, k_default_bindings[action]))
            return false;
        (void)i;
    }
    return true;
}

/* Print the map, not a hardcoded list: one source of truth, and the labels come
 * from the user's keyboard layout. */
static void print_bindings(const MuInputMap *map, FILE *file) {
    char     scratch[32];
    uint32_t action;

    for (action = 0; action < ACT_COUNT; ++action) {
        fputs("  ", file);
        fprintf(file, "%-12s", k_action_names[action]);
        if (action < map->action_count && map->source_count[action] > 0u) {
            const MuInputSource *src = &map->sources[action][0];
            if (src->kind == MU_INPUT_SRC_CONTROL) {
                fputs(platform_slot_label(src->code, scratch, sizeof scratch), file);
            } else if (src->kind == MU_INPUT_SRC_PAD_AXIS) {
                fprintf(file, "pad axis %u%s", (unsigned)src->code, src->dir < 0 ? " (inverted)" : "");
            } else {
                fprintf(file, "pad button %u", (unsigned)src->code);
            }
        } else {
            fputs("unbound", file);
        }
        fputc('\n', file);
    }
}

static void print_declared_bindings(FILE *file) {
    uint32_t action;

    for (action = 0; action < ACT_COUNT; ++action)
        fprintf(file, "  %-12s %s\n", k_action_names[action], k_default_bindings[action]);
}

static void usage(const char *argv0, FILE *file) {
    fprintf(file,
            "Usage: %s [--headless [output.ppm]] [--size W H] [--time seconds]\n"
            "       %s [--stars count] [--dust count] [--bindings]\n\n"
            "Default is an interactive window. --headless renders one frame\n"
            "offscreen and writes a PPM image.\n\nControls (positions on a US\n"
            "layout; labels are shown for your own layout in the window):\n",
            argv0, argv0);
    print_declared_bindings(file);
}

int main(int argc, char **argv) {
    Demo        d;
    bool        windowed    = true;
    const char *output_path = "galaxy.ppm";
    uint32_t    i;
    bool        success       = false;
    bool        show_usage    = false;
    bool        show_bindings = false;
    bool        bad_args      = false;
    memset(&d, 0, sizeof d);
    d.width           = WIDTH;
    d.height          = HEIGHT;
    d.star_count      = STAR_COUNT;
    d.dust_count      = DUST_COUNT;
    d.seed            = 1337u;
    d.time            = 1.35f;
    d.speed           = 1.0f;
    d.twist           = 2.05f;
    d.star_size       = 0.055f;
    d.brightness      = 1.0f;
    d.dust_size       = 0.55f;
    d.thickness       = 0.55f;
    d.core_size       = 7.5f;
    d.sky_gain        = 0.45f;
    d.exposure        = 1.0f;
    d.camera.yaw      = 0.0f;
    d.camera.pitch    = 0.60f;
    d.camera.distance = 34.0f;

    for (i = 1u; i < (uint32_t)argc; ++i) {
        if (!strcmp(argv[i], "--headless")) {
            windowed = false;
            if (i + 1u < (uint32_t)argc && argv[i + 1u][0] != '-')
                output_path = argv[++i];
        } else if (!strcmp(argv[i], "--size") && i + 2u < (uint32_t)argc) {
            d.width  = (uint32_t)strtoul(argv[++i], NULL, 10);
            d.height = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "--time") && i + 1u < (uint32_t)argc) {
            d.time = strtof(argv[++i], NULL);
        } else if (!strcmp(argv[i], "--stars") && i + 1u < (uint32_t)argc) {
            d.star_count = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "--dust") && i + 1u < (uint32_t)argc) {
            d.dust_count = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            show_usage = true;
        } else if (!strcmp(argv[i], "--bindings")) {
            show_bindings = true;
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
    if (show_bindings) {
        print_declared_bindings(stdout);
        return 0;
    }
    if (!d.width || !d.height || d.width > 4096u || d.height > 4096u) {
        fprintf(stderr, "[galaxy] invalid output extent\n");
        return 2;
    }
    if (!d.star_count || d.star_count > 1000000u || d.dust_count > 250000u) {
        fprintf(stderr, "[galaxy] particle counts outside supported range\n");
        return 2;
    }

    if (windowed) {
        d.platform = platform_create(d.width, d.height, "Galaxy — procedural spiral");
        if (!d.platform)
            return 1;
        if (!setup_bindings(d.platform))
            return 1;
        /* input v2: tick sampler + chord (Shift+R reseeds within 5 ticks) */
        mu_input_frame_reset(&d.input_frame);
        mu_input_chords_init(&d.chords);
        mu_input_chord_add(&d.chords, ACT_SPRINT, ACT_RESEED, 5u);
        d.accumulator = 0.0f;
        d.tick = 0u;
        printf("[galaxy] bindings for your layout:\n");
        print_bindings(platform_map(d.platform), stdout);
    }
    if (!init_gpu(&d, windowed)) {
        cleanup(&d);
        return 1;
    }

    if (!windowed) {
        success = render_headless(&d, output_path);
        cleanup(&d);
        return success ? 0 : 1;
    }

    printf("[galaxy] controls: A/D orbit, W/S zoom, up/down pitch, right-drag look, "
           "scroll zoom, [ ] time scale, P pause, Space resume, R reseed, "
           "-/= brightness, F1 debug UI, Esc quit\n");
    while (!platform_should_close(d.platform)) {
        const MuInputState   *raw;
        const MuInputActions *in;
        int steps = 0;

        /* Split poll (P3-2): events -> frame boundary -> actions. */
        platform_poll_events(d.platform);
        platform_frame_begin(d.platform);
        platform_resolve(d.platform);
        raw = platform_state(d.platform);
        in  = platform_actions(d.platform);
        /* Simple API + action API: either Esc path quits. */
        if (platform_key_pressed(d.platform, MU_KEY_ESCAPE) ||
            mu_input_action_pressed(in, ACT_QUIT))
            break;

        /* Debug UI toggle (F1 or `) + feed Nuklear from input state. */
        if (platform_key_pressed(d.platform, MU_KEY_F1) ||
            platform_key_pressed(d.platform, MU_KEY_GRAVE)) {
            if (d.nkvk)
                nkvk_toggle_visible(d.nkvk);
        }
        if (d.nkvk)
            nkvk_new_frame(d.nkvk, raw, raw->dt);

        update_presentation(&d, in); /* mouse/wheel once per render frame */

        /* Fixed step (game-engine-dod): accumulator, clamped steps. */
        d.accumulator += raw->dt;
        while (d.accumulator >= SIM_DT && steps < MAX_STEPS) {
            mu_input_sample_tick(raw, platform_map(d.platform), &d.chords,
                                 &d.input_frame, d.tick++);
            update_controls_tick(&d, &d.input_frame, SIM_DT, raw->time_ns);
            /* Press fires on exactly one tick per render frame. */
            mu_input_clear_pressed_frame(platform_state_mutable(d.platform));
            d.accumulator -= SIM_DT;
            ++steps;
        }
        if (steps == MAX_STEPS)
            d.accumulator = 0.0f; /* spiral-of-death guard */
        /* Build + upload debug UI (before frame_begin: upload submission). */
        if (d.nkvk) {
            build_debug_ui(&d, raw->dt);
            nkvk_prepare(d.nkvk, d.gpu, d.width, d.height);
        }
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
