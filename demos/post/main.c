/* post — bloom chain and display-transform lab.
 *
 *   Scene (RGBA16F)
 *     -> Down0 (threshold, optional Karis) -> Down1 -> ... -> Down4   13-tap
 *     -> Up3 -> Up2 -> Up1 -> Up0                                      3x3 tent, additive
 *     -> Composite (sharpen, bloom, tone map, vignette, grain, dither) -> output
 *
 * One target per level; an upsample pass blends additively into the level it
 * enlarges onto, so the chain needs five targets and no copies. Kernels and
 * tone mappers are external/mu_lib/mu/shaders/post.slang.
 *
 * --check verifies the chain numerically: it conserves energy (the filters'
 * weights sum to one at every level), and it contributes nothing when nothing
 * passes the threshold.
 */
#include "demo.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define LEVELS 5u
#define SHADERS "demos/post/shaders/"

typedef struct ScenePush { float time, aspect, kind, brightness; } ScenePush;
typedef struct DownPush {
    uint32_t source, sampler;
    float    inv_w, inv_h, threshold, knee;
    uint32_t flags, pad;
} DownPush;
typedef struct UpPush {
    uint32_t source, sampler;
    float    inv_w, inv_h, radius, pad[3];
} UpPush;
typedef struct CompositePush {
    uint32_t scene, bloom, sampler, tonemap;
    float    bloom_gain, exposure_ev, sharpen, vignette, grain, time, inv_w, inv_h;
    uint32_t flags, pad[3];
} CompositePush;

typedef struct App {
    Demo demo; /* first: see demo.h */
    Target   scene, level[LEVELS];
    uint32_t scene_index, level_index[LEVELS], level_w[LEVELS], level_h[LEVELS];
    /* parameters */
    float threshold, knee, strength, radius, sharpen, vignette, grain, brightness;
    bool  karis, compare, bloom_only;
    uint32_t scene_kind;
} App;

/* One frame: what changed since the last one, then every pass, in the order app_init listed them. */
static void app_frame(App *a) {
    Demo    *d = &a->demo;
    PassCtx *pass;

    if (!demo_frame_begin(d)) return;
    const float inv_w = 1.0f / (float)d->width, inv_h = 1.0f / (float)d->height;
    uint32_t    i;

    pass = demo_pass(d, "Scene");
    pass_push(pass, &(ScenePush){d->time, (float)d->width / (float)d->height, (float)a->scene_kind, a->brightness}, sizeof(ScenePush));
    pass_draw_fullscreen_triangle(pass, d->width, d->height);

    /* Down0 reads the scene, thresholds it and, with Karis on, tames fireflies; the others halve the level above. */
    for (i = 0u; i < LEVELS; ++i) {
        const DownPush s = {.source    = i ? a->level_index[i - 1u] : a->scene_index,
                            .sampler   = d->linear_clamp_index,
                            .inv_w     = i ? 1.0f / (float)a->level_w[i - 1u] : inv_w,
                            .inv_h     = i ? 1.0f / (float)a->level_h[i - 1u] : inv_h,
                            .threshold = a->threshold,
                            .knee      = a->knee,
                            .flags     = i ? 0u : (1u | (a->karis ? 2u : 0u))};
        pass = demo_pass(d, NULL);   /* "Down0" .. "Down4" */
        pass_push(pass, &s, sizeof s);
        pass_draw_fullscreen_triangle(pass, a->level_w[i], a->level_h[i]);
    }
    /* Up3 .. Up0: each level adds the blurred level below it. */
    for (i = LEVELS - 1u; i-- > 0u;) {
        const UpPush s = {.source  = a->level_index[i + 1u],
                          .sampler = d->linear_clamp_index,
                          .inv_w   = 1.0f / (float)a->level_w[i + 1u],
                          .inv_h   = 1.0f / (float)a->level_h[i + 1u],
                          .radius  = a->radius};
        pass = demo_pass(d, NULL);
        pass_push(pass, &s, sizeof s);
        pass_draw_fullscreen_triangle(pass, a->level_w[i], a->level_h[i]);
    }
    pass = demo_pass(d, "Composite");   /* writes demo->out itself: there is no Present pass */
    pass_push(pass, &(CompositePush){.scene       = a->scene_index,
                                     .bloom       = a->level_index[0],
                                     .sampler     = d->linear_clamp_index,
                                     .tonemap     = d->present.tonemap,
                                     .bloom_gain  = a->strength / (float)LEVELS,
                                     .exposure_ev = d->present.exposure_ev,
                                     .sharpen     = a->sharpen,
                                     .vignette    = a->vignette,
                                     .grain       = a->grain,
                                     .time        = d->time,
                                     .inv_w       = inv_w,
                                     .inv_h       = inv_h,
                                     .flags       = (d->out_encode_srgb ? 1u : 0u) | (d->present.dither ? 2u : 0u) |
                                                    (a->compare ? 4u : 0u) | (a->bloom_only ? 8u : 0u)},
              sizeof(CompositePush));
    pass_draw_fullscreen_triangle(pass, d->width, d->height);
    demo_frame_end(d);
}

/* Every pass here is a fullscreen triangle into one colour target. */
static PassDesc fullscreen_pass(const char *name, Pipe pipe, const AttachDesc *color, const Target *reads,
                                uint32_t read_count) {
    PassDesc pass = {.kind = PASS_GRAPHICS, .pipe = pipe, .state = draw_state_opaque(SC_1, CULL_NONE),
                     .colors = color, .color_count = 1u, .tex_reads = reads, .tex_read_count = read_count};
    snprintf(pass.name, sizeof pass.name, "%s", name);
    return pass;
}

static void app_init(App *a) {
    Demo      *d = &a->demo;
    Pipe       scene_pipe, down_pipe, up_pipe, composite_pipe;
    AttachDesc scene_color, down_color[LEVELS], up_color[LEVELS], composite_color;
    Target     down_read[LEVELS], up_read[LEVELS], composite_read[2];
    PassDesc   passes[2u * LEVELS + 3u];
    uint32_t   count = 0u, i;
    char       name[MAX_PASS_NAME];

    a->scene = demo_target(d, "post_scene", FMT_RGBA16F, d->width, d->height, false, &a->scene_index);
    for (i = 0u; i < LEVELS; ++i) {
        a->level_w[i] = d->width >> (i + 1u) ? d->width >> (i + 1u) : 1u;
        a->level_h[i] = d->height >> (i + 1u) ? d->height >> (i + 1u) : 1u;
        snprintf(name, sizeof name, "post_level%u", i);
        a->level[i] = demo_target(d, name, FMT_RGBA16F, a->level_w[i], a->level_h[i], false, &a->level_index[i]);
    }
    scene_pipe     = pipe_intern_graphics(d->gpu, d->fullscreen_vs, demo_shader(d, SHADERS "scene.frag.spv", STAGE_FRAGMENT),
                                          FMT_RGBA16F, FMT_NONE, BLEND_OPAQUE);
    down_pipe      = pipe_intern_graphics(d->gpu, d->fullscreen_vs, demo_shader(d, SHADERS "down.frag.spv", STAGE_FRAGMENT),
                                          FMT_RGBA16F, FMT_NONE, BLEND_OPAQUE);
    up_pipe        = pipe_intern_graphics(d->gpu, d->fullscreen_vs, demo_shader(d, SHADERS "up.frag.spv", STAGE_FRAGMENT),
                                          FMT_RGBA16F, FMT_NONE, BLEND_ADD);
    composite_pipe = pipe_intern_graphics(d->gpu, d->fullscreen_vs, demo_shader(d, SHADERS "composite.frag.spv", STAGE_FRAGMENT),
                                          d->out_format, FMT_NONE, BLEND_OPAQUE);

    scene_color = attach_color(a->scene, ATT_WRITE);
    passes[count++] = fullscreen_pass("Scene", scene_pipe, &scene_color, NULL, 0u);
    for (i = 0u; i < LEVELS; ++i) {
        down_color[i] = attach_color(a->level[i], ATT_WRITE);
        down_read[i]  = i ? a->level[i - 1u] : a->scene;
        snprintf(name, sizeof name, "Down%u", i);
        passes[count++] = fullscreen_pass(name, down_pipe, &down_color[i], &down_read[i], 1u);
    }
    for (i = LEVELS - 1u; i-- > 0u;) {
        up_color[i] = attach_color(a->level[i], ATT_WRITE_KEEP);
        up_read[i]  = a->level[i + 1u];
        snprintf(name, sizeof name, "Up%u", i);
        passes[count] = fullscreen_pass(name, up_pipe, &up_color[i], &up_read[i], 1u);
        passes[count++].state.blend = BLEND_ADD;
    }
    composite_color   = attach_color(d->out, ATT_WRITE);
    composite_read[0] = a->scene;
    composite_read[1] = a->level[0];
    passes[count++] = fullscreen_pass("Composite", composite_pipe, &composite_color, composite_read, 2u);
    d->program = demo_program(d, passes, count);
}

static void app_ui(App *a, struct nk_context *ctx) {
    Demo *d = &a->demo;
    if (nk_begin(ctx, "Post", nk_rect(10, 10, 310, 470),
                 NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_TITLE)) {
        demo_ui_text(ctx, "%.1f ms   %u bloom levels", (double)d->dt * 1000.0, LEVELS);
        demo_ui_choice(ctx, "Tone map", demo_tonemap_names, DEMO_TONEMAP_COUNT, &d->present.tonemap);
        demo_ui_bool(ctx, "All eight side by side", &a->compare);
        demo_ui_float(ctx, "Exposure (EV)", &d->present.exposure_ev, -6.0f, 4.0f);
        demo_ui_float(ctx, "Bloom strength", &a->strength, 0.0f, 3.0f);
        demo_ui_float(ctx, "Threshold", &a->threshold, 0.0f, 8.0f);
        demo_ui_float(ctx, "Knee", &a->knee, 0.0f, 2.0f);
        demo_ui_float(ctx, "Upsample radius", &a->radius, 0.5f, 3.0f);
        demo_ui_bool(ctx, "Karis average (firefly filter)", &a->karis);
        demo_ui_bool(ctx, "Bloom only", &a->bloom_only);
        demo_ui_float(ctx, "Sharpen", &a->sharpen, 0.0f, 2.0f);
        demo_ui_float(ctx, "Vignette", &a->vignette, 0.0f, 1.0f);
        demo_ui_float(ctx, "Grain", &a->grain, 0.0f, 0.2f);
        demo_ui_bool(ctx, "Dither", &d->present.dither);
        demo_ui_float(ctx, "Scene brightness", &a->brightness, 0.1f, 4.0f);
    }
    nk_end(ctx);
}

/* ---- acceptance ----------------------------------------------------------- */



/* Sum of the RGB channels of an RGBA16F target. */
/* Sum of r + g + b over a half-float target. */
static double target_sum(App *a, Target t, uint32_t w, uint32_t h) {
    const size_t count = (size_t)w * h;
    float       *px    = demo_read_floats(&a->demo, t, count * 4u);
    double       sum   = 0.0;
    size_t       i;
    for (i = 0u; i < count; ++i) sum += (double)px[i * 4u] + (double)px[i * 4u + 1u] + (double)px[i * 4u + 2u];
    free(px);
    return sum;
}

static void app_check(App *a) {
    Demo        *d     = &a->demo;
    const size_t bytes = (size_t)d->width * d->height * 4u;
    uint8_t     *ref = (uint8_t *)malloc(bytes), *img = (uint8_t *)malloc(bytes);
    double       scene_mean, bloom_mean, bloom_sum, ratio;
    size_t       i, differing = 0u;

    /* 1. Energy. Calibration scene (zero at the borders), no threshold, no
     * Karis weight: every level then has the mean of the scene, and the
     * additive up-chain collects all LEVELS of them in level 0. */
    a->scene_kind = 1u;
    a->threshold  = 0.0f;
    a->knee       = 0.0f;
    a->karis      = false;
    a->radius     = 1.0f;
    app_frame(a);
    scene_mean = target_sum(a, a->scene, d->width, d->height) / ((double)d->width * d->height);
    bloom_mean = target_sum(a, a->level[0], a->level_w[0], a->level_h[0]) / ((double)a->level_w[0] * a->level_h[0]);
    ratio      = bloom_mean / ((double)LEVELS * scene_mean);
    demo_check(d, "scene mean (rgb sum)", scene_mean, "> 0", scene_mean > 0.0);
    demo_check(d, "bloom energy / (levels x scene)", ratio, "1 +- 0.01", fabs(ratio - 1.0) <= 0.01);

    /* 2. Nothing above the threshold -> the chain is exactly zero and the
     * output equals the output with bloom switched off. */
    a->scene_kind = 0u;
    a->threshold  = 1.0e6f;
    a->strength   = 1.0f;
    d->present.dither = false;
    app_frame(a);
    demo_readback(d, d->out, img, bytes);
    bloom_sum   = target_sum(a, a->level[0], a->level_w[0], a->level_h[0]);
    a->strength = 0.0f;
    app_frame(a);
    demo_readback(d, d->out, ref, bytes);
    for (i = 0u; i < bytes; ++i) differing += ref[i] != img[i];
    demo_check(d, "bloom sum above a 1e6 threshold", bloom_sum, "== 0", bloom_sum == 0.0);
    demo_check(d, "bytes differing from bloom off", (double)differing, "== 0", differing == 0u);
    free(ref);
    free(img);
}

int main(int argc, char **argv) {
    static App app;
    static const DemoOption options[] = {
        {"--compare", .flag = &app.compare, .help = "all eight tone mappers side by side"},
        {"--tonemap", .u32 = &app.demo.present.tonemap, .lo = 0, .hi = DEMO_TONEMAP_COUNT - 1, .help = "0 none, 1 Reinhard, 2 ACES fitted, 3 ACES, 4 neutral, 5 Uchimura, 6 Hable, 7 AgX"},
        {"--exposure", .f32 = &app.demo.present.exposure_ev, .lo = -20, .hi = 20, .help = "exposure in stops"},
        {"--bloom-only", .flag = &app.bloom_only, .help = "show the bloom chain alone"},
        {"--strength", .f32 = &app.strength, .lo = 0, .hi = 8, .help = "bloom strength; 0 is no bloom"},
        {"--no-karis", .off = &app.karis, .help = "first downsample without the firefly weight"},
        {0}};
    demo_defaults(&app.demo, "post", "Post - bloom and tone mapping (mu_lib post.slang)");
    app.threshold  = 1.0f;
    app.knee       = 0.5f;
    app.strength   = 1.0f;
    app.radius     = 1.0f;
    app.karis      = true;
    app.brightness = 1.0f;
    app.vignette   = 0.3f;
    demo_open(&app.demo, options, argc, argv);
    app_init(&app);
    while (demo_running(&app.demo)) {
        if (app.demo.ui) app_ui(&app, app.demo.ui);
        app_frame(&app);
    }
    if (app.demo.check) app_check(&app);
    return demo_close(&app.demo);
}
