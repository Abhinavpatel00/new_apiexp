/* demo.c — see demo.h. */
#include "demo.h"

#include "mu_math.h"

#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define COMMON_SHADERS "demos/common/shaders/"

const char *const demo_tonemap_names[DEMO_TONEMAP_COUNT] = {
    "none (clamp)", "Reinhard", "ACES (fitted)", "ACES (Hill)",
    "Khronos neutral", "Uchimura", "Hable", "AgX"};

/* Must match PresentPush in demos/common/shaders/present.slang. */
typedef struct PresentPush {
    uint32_t source_index, sampler_index, tonemap, flags;
    float    exposure_ev, pad[3];
} PresentPush;
#define PRESENT_ENCODE_SRGB 1u
#define PRESENT_DITHER      2u

void demo_defaults(Demo *d, const char *name, const char *title) {
    memset(d, 0, sizeof *d);
    d->name   = name;
    d->title  = title;
    d->width  = 1280u;
    d->height = 720u;
    d->present.tonemap = DEMO_TONEMAP_ACES_FITTED;
    d->present.dither  = true;
}

void demo_fatal(const Demo *d, const char *fmt, ...) {
    const char *reason = d->gpu ? gpu_last_error(d->gpu) : "";
    va_list     ap;
    fprintf(stderr, "[%s] ", d->name);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, reason[0] && strcmp(reason, "no error") ? ": %s\n" : "\n", reason);
    exit(1);
}

/* ---- resources ----------------------------------------------------------- */

ShaderId demo_shader(Demo *d, const char *path, ShaderStage2 stage) {
    ShaderId id = SHADER_NONE;
    if (!shader_load(d->gpu, path, stage, &id)) demo_fatal(d, "shader %s", path);
    return id;
}

/* demo_target and demo_volume: a 2D target has depth 0. */
static Target make_target(Demo *d, const char *name, Format format, uint32_t width, uint32_t height,
                          uint32_t depth, uint32_t usage, uint32_t *bindless_index) {
    const TargetDesc td = {.name = name, .width = width, .height = height, .depth = depth, .format = format,
                           .usage = usage, .mips = 1u, .samples = SC_1, .cls = RES_PERSISTENT};
    const Target     t  = target_create(d->gpu, &td);
    if (!HANDLE_VALID(t)) demo_fatal(d, "target %s", name);
    if (bindless_index && !texture_info(d->gpu, target_texture(d->gpu, t), bindless_index))
        demo_fatal(d, "target %s has no bindless index", name);
    return t;
}

Target demo_target(Demo *d, const char *name, Format format, uint32_t width, uint32_t height,
                   bool storage, uint32_t *bindless_index) {
    return make_target(d, name, format, width, height, 0u,
                       (format_is_depth(format) ? TGT_DEPTH : TGT_COLOR) | TGT_SAMPLED | (storage ? TGT_STORAGE : 0u),
                       bindless_index);
}

Target demo_volume(Demo *d, const char *name, Format format, uint32_t width, uint32_t height,
                   uint32_t depth, uint32_t *bindless_index) {
    return make_target(d, name, format, width, height, depth, TGT_SAMPLED | TGT_STORAGE, bindless_index);
}

void demo_upload(Demo *d, BufSlice dst, const void *bytes, uint64_t size) {
    Upload *u;
    if (!bytes || !size) return;            /* demo_alloc(d, NULL, n): allocate only */
    u = upload_begin(d->gpu);
    if (!u) demo_fatal(d, "upload of %llu bytes", (unsigned long long)size);
    dst.size = size;
    upload_buffer(u, dst, DATA_SPAN(bytes, size));
    upload_end(d->gpu, u);
}

BufSlice demo_alloc(Demo *d, const void *bytes, uint64_t size) {
    const BufSlice s = arena_alloc(d->gpu, ARENA_DEVICE, size, 16u);
    if (!HANDLE_VALID(s.buffer)) demo_fatal(d, "device arena is full (%llu bytes requested)", (unsigned long long)size);
    demo_upload(d, s, bytes, size);
    return s;
}

/* ---- passes -------------------------------------------------------------- */

PassDesc demo_present_pass(Demo *d, Target source) {
    if (!HANDLE_VALID(d->present.pipe))     /* the first of several programs makes it */
        d->present.pipe = pipe_intern_graphics(d->gpu, d->fullscreen_vs,
                                               demo_shader(d, COMMON_SHADERS "present.frag.spv", STAGE_FRAGMENT),
                                               d->out_format, FMT_NONE, BLEND_OPAQUE);
    if (!texture_info(d->gpu, target_texture(d->gpu, source), &d->present.source_index))
        demo_fatal(d, "present: the source target cannot be sampled");
    d->present.color = attach_color(d->out, ATT_WRITE);
    d->present.read  = source;
    return (PassDesc){.name = "Present", .kind = PASS_GRAPHICS, .pipe = d->present.pipe,
                      .state = draw_state_opaque(SC_1, CULL_NONE),
                      .colors = &d->present.color, .color_count = 1u,
                      .tex_reads = &d->present.read, .tex_read_count = 1u};
}

FrameProgram *demo_program(Demo *d, PassDesc *passes, uint32_t count) {
    FrameProgram *program;
    uint32_t      i;
    if (d->nkvk) {
        d->ui_color = attach_color(d->out, ATT_WRITE_KEEP);
        d->ui_use   = nkvk_buf_use(d->nkvk);
        passes[count] = (PassDesc){.name = "DebugUI", .kind = PASS_GRAPHICS, .pipe = nkvk_pipe(d->nkvk),
                                   .state = draw_state_opaque(SC_1, CULL_NONE),
                                   .colors = &d->ui_color, .color_count = 1u,
                                   .buf_reads = &d->ui_use, .buf_read_count = 1u};
        passes[count++].state.blend = BLEND_ALPHA;
    }
    /* The one place a pipeline is checked: none of the pipe_intern calls in a demo is. */
    for (i = 0u; i < count; ++i)
        if (!HANDLE_VALID(passes[i].pipe)) demo_fatal(d, "pass '%s' has no pipeline", passes[i].name);
    program = frame_program_build(d->gpu, passes, count);
    if (!program) demo_fatal(d, "frame program");
    return program;
}

/* ---- for --check ----------------------------------------------------------- */

/* Both readbacks: wait for the copy, up to two minutes. */
static void wait_readback(Demo *d, Readback *rb, const char *what) {
    bool     ok = false;
    uint32_t i;
    for (i = 0u; rb && !ok && i < 120u; ++i) ok = readback_ready(d->gpu, rb, true);
    if (!ok) demo_fatal(d, "%s readback", what);
    readback_release(d->gpu, rb);
}

void demo_readback(Demo *d, Target target, void *dst, size_t bytes) {
    wait_readback(d, readback_request(d->gpu, target_texture(d->gpu, target), DATA_SPAN(dst, bytes), 0u), "target");
}

void demo_readback_buffer(Demo *d, BufSlice src, void *dst, size_t bytes) {
    wait_readback(d, readback_request_buffer(d->gpu, src, DATA_SPAN(dst, bytes)), "buffer");
}

float *demo_read_floats(Demo *d, Target target, size_t count) {
    uint16_t *half = (uint16_t *)malloc(count * 2u);
    float    *out  = (float *)malloc(count * sizeof(float));
    size_t    i;
    demo_readback(d, target, half, count * 2u);
    for (i = 0u; i < count; ++i) out[i] = demo_half_to_float(half[i]);
    free(half);
    return out;
}

void demo_write_ppm(Demo *d, const char *path) {
    const size_t pixels = (size_t)d->width * d->height;
    uint8_t     *px     = (uint8_t *)malloc(pixels * 4u);
    FILE        *f      = fopen(path, "wb");
    size_t       i;
    if (!f) demo_fatal(d, "cannot write %s", path);
    demo_readback(d, d->out, px, pixels * 4u);
    fprintf(f, "P6\n%u %u\n255\n", d->width, d->height);
    for (i = 0u; i < pixels; ++i) fwrite(px + i * 4u, 1u, 3u, f);
    fclose(f);
    free(px);
    printf("[%s] wrote %s (%ux%u)\n", d->name, path, d->width, d->height);
}

bool demo_check(Demo *d, const char *label, double value, const char *rule, bool ok) {
    printf("[%s] check %-34s %12.6g  (%s)  %s\n", d->name, label, value, rule, ok ? "PASS" : "FAIL");
    d->checks_failed += !ok;
    return ok;
}

float demo_half_to_float(uint16_t h) {
    const uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
    uint32_t       exp  = (h >> 10) & 0x1Fu;
    uint32_t       man  = h & 0x3FFu;
    uint32_t       bits;
    float          out;
    if (exp == 0u) {
        if (man == 0u) {
            bits = sign;
        } else {
            exp = 1u;
            while (!(man & 0x400u)) { man <<= 1; --exp; }
            man &= 0x3FFu;
            bits = sign | ((exp + 112u) << 23) | (man << 13);
        }
    } else if (exp == 31u) {
        bits = sign | 0x7F800000u | (man << 13);
    } else {
        bits = sign | ((exp + 112u) << 23) | (man << 13);
    }
    memcpy(&out, &bits, sizeof out);
    return out;
}

/* ---- camera --------------------------------------------------------------- */

void demo_orbit_input(Demo *d, DemoOrbit *o) {
    const MuInputState *s;
    if (!d->platform || d->ui_hovered) return;
    s = platform_state(d->platform);
    if (mu_input_mouse_down(s, MU_MOUSE_LEFT) || mu_input_mouse_down(s, MU_MOUSE_RIGHT)) {
        o->yaw   -= s->mouse_dx * 0.005f;
        o->pitch += s->mouse_dy * 0.005f;
        if (o->pitch > 1.5f) o->pitch = 1.5f;
        if (o->pitch < -1.5f) o->pitch = -1.5f;
    }
    if (s->wheel_y != 0.0f) {
        o->distance *= powf(0.9f, s->wheel_y);
        if (o->min_distance > 0.0f && o->distance < o->min_distance) o->distance = o->min_distance;
        if (o->max_distance > 0.0f && o->distance > o->max_distance) o->distance = o->max_distance;
    }
}

void demo_orbit_camera(const DemoOrbit *o, float aspect, float view[16], float proj[16],
                       float view_proj[16], float eye[3]) {
    mu_mat4 v, p, vp;
    mu_vec3 e, t, up = {0.0f, 1.0f, 0.0f};
    const float cp = cosf(o->pitch);
    e[0] = o->target[0] + o->distance * cp * sinf(o->yaw);
    e[1] = o->target[1] + o->distance * sinf(o->pitch);
    e[2] = o->target[2] + o->distance * cp * cosf(o->yaw);
    t[0] = o->target[0]; t[1] = o->target[1]; t[2] = o->target[2];
    mu_mat4_look_at(v, e, t, up);
    mu_mat4_perspective_zo(p, o->fov_deg * 0.017453292f, aspect, o->znear, o->zfar);
    mu_mat4_mul(p, v, vp);
    if (view) memcpy(view, v, sizeof v);
    if (proj) memcpy(proj, p, sizeof p);
    if (view_proj) memcpy(view_proj, vp, sizeof vp);
    if (eye) { eye[0] = e[0]; eye[1] = e[1]; eye[2] = e[2]; }
}

/* ---- Nuklear rows ---------------------------------------------------------- */

void demo_ui_float(struct nk_context *ctx, const char *label, float *value, float lo, float hi) {
    char text[64];
    snprintf(text, sizeof text, "%s  %.3g", label, (double)*value);
    nk_layout_row_dynamic(ctx, 20, 2);
    nk_label(ctx, text, NK_TEXT_LEFT);
    nk_slider_float(ctx, lo, value, hi, (hi - lo) / 400.0f);
}

void demo_ui_bool(struct nk_context *ctx, const char *label, bool *value) {
    nk_bool v = *value ? nk_true : nk_false;
    nk_layout_row_dynamic(ctx, 20, 1);
    nk_checkbox_label(ctx, label, &v);
    *value = v ? true : false;
}

bool demo_ui_choice(struct nk_context *ctx, const char *label, const char *const *names,
                    uint32_t count, uint32_t *index) {
    int selected;
    nk_layout_row_dynamic(ctx, 24, 2);
    nk_label(ctx, label, NK_TEXT_LEFT);
    selected = nk_combo(ctx, (const char **)names, (int)count, (int)*index, 22, nk_vec2(220, 260));
    if ((uint32_t)selected == *index) return false;
    *index = (uint32_t)selected;
    return true;
}

void demo_ui_text(struct nk_context *ctx, const char *fmt, ...) {
    char    text[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    nk_layout_row_dynamic(ctx, 18, 1);
    nk_label(ctx, text, NK_TEXT_LEFT);
}

/* ---- main ----------------------------------------------------------------- */

static void usage(const Demo *d, const char *argv0) {
    const DemoOption *o;
    printf("%s\n\n"
           "usage: %s [options]\n"
           "  --headless [FILE]  render without a window; write a PPM (default %s.ppm)\n"
           "  --size W H         output size (default %ux%u)\n"
           "  --frames N         headless: frames before the capture (default 2);\n"
           "                     windowed: quit after N frames\n"
           "  --time T           freeze the demo clock at T seconds\n"
           "  --check            headless numeric acceptance test (exit 0 = pass)\n"
           "  --validate         Khronos validation layers, if installed\n"
           "  --perf FILE        write the pass timing report\n"
           "  --help\n",
           d->title, argv0, d->name, d->width, d->height);
    for (o = d->options; o && o->name; ++o) {
        char left[40];
        snprintf(left, sizeof left, "%s%s", o->name, o->u32 ? " N" : o->f32 ? " X" : "");
        printf("  %-18s %s", left, o->help);
        if (o->u32) printf(" (%g..%g, default %u)", o->lo, o->hi, *o->u32);
        if (o->f32) printf(" (%g..%g, default %g)", o->lo, o->hi, (double)*o->f32);
        printf("\n");
    }
    if (d->note) printf("  (%s)\n", d->note);
    printf("\nwindow: left-drag orbit, wheel zoom, F1 panel, Esc quit\n");
}

/* The window (unless headless), the device, three samplers, the fullscreen vertex stage and the
 * output: the swapchain plus the Nuklear overlay in a window, an sRGB target when headless. */
static void open_gpu(Demo *d) {
    const bool    windowed = !d->headless;
    const GpuDesc gd = {.width = d->width, .height = d->height, .frames_in_flight = 3u, .app_name = d->name,
                        .vsync = windowed, .validation = d->validate};
    SamplerDesc   sd = sampler_linear_clamp();

    if (windowed) {
        d->platform = platform_create(d->width, d->height, d->title);
        if (!d->platform) demo_fatal(d, "cannot open a window (no display?). Use --headless.");
    }
    d->gpu = gpu_create(&gd);
    if (!d->gpu) demo_fatal(d, "gpu_create failed: %s", gpu_last_error(NULL));
    if (windowed) {
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        if (!platform_create_surface(d->platform, gpu_raw_instance(d->gpu), &surface) || !set_surface(d->gpu, surface))
            demo_fatal(d, "window surface");
    }
    gpu_info(d->gpu, &d->info);
    d->width  = d->info.extent_w;
    d->height = d->info.extent_h;
    printf("[%s] device: %s (%ux%u)\n", d->name, d->info.device_name, d->width, d->height);

    d->linear_clamp = sampler_create(d->gpu, &sd);
    sd.addr_u = sd.addr_v = sd.addr_w = ADDR_REPEAT;
    d->linear_repeat = sampler_create(d->gpu, &sd);
    sd = sampler_linear_clamp();
    sd.min_filter = sd.mag_filter = sd.mip_filter = FILT_NEAREST;
    d->nearest_clamp = sampler_create(d->gpu, &sd);
    if (!HANDLE_VALID(d->linear_clamp) || !HANDLE_VALID(d->linear_repeat) || !HANDLE_VALID(d->nearest_clamp))
        demo_fatal(d, "sampler");
    d->linear_clamp_index  = sampler_bindless_index(d->gpu, d->linear_clamp);
    d->linear_repeat_index = sampler_bindless_index(d->gpu, d->linear_repeat);
    d->nearest_clamp_index = sampler_bindless_index(d->gpu, d->nearest_clamp);
    d->fullscreen_vs       = demo_shader(d, COMMON_SHADERS "fullscreen.vert.spv", STAGE_VERTEX);

    if (windowed) {
        d->out             = TARGET_SWAPCHAIN;
        d->out_format      = d->info.swapchain_format;
        d->out_encode_srgb = !d->info.swapchain_srgb;
        d->nkvk = nkvk_create(d->gpu, COMMON_SHADERS "nk.vert.spv", COMMON_SHADERS "nk.frag.spv",
                              d->out_format, d->width, d->height);
        if (!d->nkvk) demo_fatal(d, "debug UI");
    } else {
        d->out_format = FMT_RGBA8_SRGB;
        d->out        = demo_target(d, "demo_out", d->out_format, d->width, d->height, false, NULL);
    }
}

void demo_open(Demo *d, const DemoOption *options, int argc, char **argv) {
    int i;
    d->options = options;
    /* --help first, so that it prints the defaults and not what the other options changed. */
    for (i = 1; i < argc; ++i)
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(d, argv[0]); exit(0); }

    /* The demo's table, then the options every demo has. Anything left over, a missing value or
     * a value out of range ends the run here. */
    for (i = 1; i < argc; ++i) {
        const char       *a = argv[i];
        const DemoOption *o = options;
        const int         left = argc - 1 - i;       /* arguments after this one */
        while (o && o->name && strcmp(o->name, a)) ++o;
        if (o && o->name) {
            if (o->flag) *o->flag = true;
            if (o->off) *o->off = false;
            if (!o->u32 && !o->f32) continue;
            if (left >= 1) {
                const double v = strtod(argv[++i], NULL);
                if (o->u32) *o->u32 = (uint32_t)v;
                if (o->f32) *o->f32 = (float)v;
                if (v >= o->lo && v <= o->hi) continue;
            }
            demo_fatal(d, "%s needs a value from %g to %g", a, o->lo, o->hi);
        }
        if (!strcmp(a, "--headless")) {
            d->headless = true;
            d->out_path = (left >= 1 && argv[i + 1][0] != '-') ? argv[++i] : "";
        } else if (!strcmp(a, "--size") && left >= 2) {
            d->width  = (uint32_t)strtoul(argv[++i], NULL, 10);
            d->height = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (!strcmp(a, "--frames") && left >= 1) {
            d->frames = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (!strcmp(a, "--time") && left >= 1) {
            d->time       = strtof(argv[++i], NULL);
            d->time_fixed = true;
        } else if (!strcmp(a, "--perf") && left >= 1) {
            d->perf_path = argv[++i];
        } else if (!strcmp(a, "--check")) {
            d->check = d->headless = true;
        } else if (!strcmp(a, "--validate")) {
            d->validate = true;
        } else {
            demo_fatal(d, "unknown or incomplete option %s (see --help)", a);
        }
    }
    if (!d->width || !d->height || d->width > 8192u || d->height > 8192u)
        demo_fatal(d, "invalid size %ux%u", d->width, d->height);
    open_gpu(d);
}

bool demo_running(Demo *d) {
    static char         default_path[96];
    const MuInputState *s;
    struct nk_context  *ctx;

    if (d->started) ++d->frame;      /* the frame the caller has just drawn */
    if (d->headless) {
        if (d->started && !d->time_fixed) d->time += d->dt;
        d->started = true;
        d->dt      = 1.0f / 60.0f;
        if (d->frame < (d->frames ? d->frames : 2u)) return true;
        if (d->out_path && !d->out_path[0]) {
            snprintf(default_path, sizeof default_path, "%s.ppm", d->name);
            d->out_path = default_path;
        }
        if (d->out_path) demo_write_ppm(d, d->out_path);
        return false;
    }
    if (!d->started) printf("[%s] left-drag orbit, wheel zoom, F1 panel, Esc quit\n", d->name);
    d->started = true;
    if (platform_should_close(d->platform) || platform_key_pressed(d->platform, MU_KEY_ESCAPE) ||
        (d->frames && d->frame >= d->frames))
        return false;
    platform_poll(d->platform);
    s   = platform_state(d->platform);
    ctx = nkvk_context(d->nkvk);
    if (platform_key_pressed(d->platform, MU_KEY_F1) || platform_key_pressed(d->platform, MU_KEY_GRAVE))
        nkvk_toggle_visible(d->nkvk);
    d->dt = s->dt;
    if (!d->time_fixed) d->time += d->dt;
    d->ui_hovered = nkvk_visible(d->nkvk) && (nk_window_is_any_hovered(ctx) || nk_item_is_any_active(ctx));
    nkvk_new_frame(d->nkvk, s, s->dt);
    d->ui = nkvk_visible(d->nkvk) ? ctx : NULL;
    return true;
}

bool demo_frame_begin(Demo *d) {
    if (d->nkvk) nkvk_prepare(d->nkvk, d->gpu, d->width, d->height);
    if (frame_begin(d->gpu)) return true;
    if (d->headless) demo_fatal(d, "frame");
    return false;
}

PassCtx *demo_pass(Demo *d, const char *name) {
    PassCtx *pass = frame_pass(d->gpu, d->program, name);
    if (!pass) demo_fatal(d, "pass '%s'", name);
    return pass;
}

void demo_present(Demo *d) {
    PassCtx *p = demo_pass(d, "Present");
    const PresentPush push = {.source_index  = d->present.source_index,
                              .sampler_index = d->linear_clamp_index,
                              .tonemap       = d->present.tonemap,
                              .flags         = (d->out_encode_srgb ? PRESENT_ENCODE_SRGB : 0u) |
                                               (d->present.dither ? PRESENT_DITHER : 0u),
                              .exposure_ev   = d->present.exposure_ev};
    pass_push(p, &push, sizeof push);
    pass_draw_fullscreen_triangle(p, d->width, d->height);
}

void demo_frame_end(Demo *d) {
    if (d->nkvk) nkvk_draw(d->nkvk, demo_pass(d, "DebugUI"));
    frame_end(d->gpu);
}

int demo_close(Demo *d) {
    if (d->check) printf("[%s] acceptance: %s\n", d->name, d->checks_failed ? "FAIL" : "PASS");
    if (d->perf_path) perf_dump(d->gpu, d->perf_path);
    frame_program_destroy(d->gpu, d->program);
    if (d->nkvk) nkvk_destroy(d->nkvk, d->gpu);
    gpu_destroy(d->gpu);
    if (d->platform) platform_destroy(d->platform);
    return d->checks_failed ? 1 : 0;
}
