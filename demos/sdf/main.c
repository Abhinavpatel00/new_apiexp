/* sdf — a sphere-traced signed-distance scene.
 *
 *   Trace (full-screen, RGBA16F, full or half resolution) -> Present
 *
 * The scene, the march, the normals, the soft shadow and the ambient occlusion
 * are external/mu_lib/mu/shaders/sdf.slang; see shaders/sdf.slang.
 *
 * --check renders a calibration scene — one sphere, no ground — and compares
 * the number of pixels the march reports as hit with the area of the sphere's
 * exact perspective silhouette.
 */
#include "demo.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct TracePush {
    float    eye_time[4], right_tan[4], up_aspect[4], forward_tmax[4], sun_k[4], params[4];
    uint32_t scene, view, max_steps, shadow_steps;
} TracePush;

typedef struct App {
    Demo      demo; /* first: see demo.h */
    DemoOrbit orbit;
    Target    trace;
    uint32_t  trace_w, trace_h;
    TracePush push;
    /* options */
    bool     half;
    uint32_t view, max_steps, shadow_steps;
    float    sun_yaw, sun_height, shadow_k, ao, fog, blend;
    /* calibration (--check) */
    uint32_t scene;
    float    radius;
} App;

static const char *const k_views[] = {"shaded", "normals", "step count", "ambient occlusion", "sun shadow"};

/* One frame: what changed since the last one, then every pass, in the order app_init listed them. */
static void app_frame(App *a) {
    Demo      *d = &a->demo;
    TracePush *p = &a->push;
    PassCtx   *pass;
    float      view[16], eye[3], len;
    const float aspect = (float)d->width / (float)d->height;

    demo_orbit_input(d, &a->orbit);
    demo_orbit_camera(&a->orbit, aspect, view, NULL, NULL, eye);
    memset(p, 0, sizeof *p);
    p->eye_time[0] = eye[0]; p->eye_time[1] = eye[1]; p->eye_time[2] = eye[2];
    p->eye_time[3] = d->time;
    /* Rows of the rotation part of a column-major view matrix are the camera
     * axes in world space; the camera looks down -Z. */
    p->right_tan[0] = view[0]; p->right_tan[1] = view[4]; p->right_tan[2] = view[8];
    p->right_tan[3] = tanf(a->orbit.fov_deg * 0.017453292f * 0.5f);
    p->up_aspect[0] = view[1]; p->up_aspect[1] = view[5]; p->up_aspect[2] = view[9];
    p->up_aspect[3] = aspect;
    p->forward_tmax[0] = -view[2]; p->forward_tmax[1] = -view[6]; p->forward_tmax[2] = -view[10];
    p->forward_tmax[3] = a->orbit.zfar;
    p->sun_k[0] = cosf(a->sun_height) * sinf(a->sun_yaw);
    p->sun_k[1] = sinf(a->sun_height);
    p->sun_k[2] = cosf(a->sun_height) * cosf(a->sun_yaw);
    len = sqrtf(p->sun_k[0] * p->sun_k[0] + p->sun_k[1] * p->sun_k[1] + p->sun_k[2] * p->sun_k[2]);
    p->sun_k[0] /= len; p->sun_k[1] /= len; p->sun_k[2] /= len;
    p->sun_k[3] = a->shadow_k;
    p->params[0] = a->ao;
    p->params[1] = a->fog;
    p->params[2] = a->radius;
    p->params[3] = a->blend;
    p->scene        = a->scene;
    p->view         = a->view;
    p->max_steps    = a->max_steps;
    p->shadow_steps = a->shadow_steps;

    if (!demo_frame_begin(d)) return;
    pass = demo_pass(d, "Trace");
    pass_push(pass, &a->push, sizeof a->push);
    pass_note_samples(pass, a->max_steps, a->max_steps, (double)a->max_steps);
    pass_draw_fullscreen_triangle(pass, a->trace_w, a->trace_h);
    demo_present(d);
    demo_frame_end(d);
}

static void app_init(App *a) {
    Demo    *d = &a->demo;
    PassDesc passes[3];

    a->trace_w = a->half ? (d->width + 1u) / 2u : d->width;
    a->trace_h = a->half ? (d->height + 1u) / 2u : d->height;
    a->trace   = demo_target(d, "sdf_trace", FMT_RGBA16F, a->trace_w, a->trace_h, false, NULL);
    passes[0] = (PassDesc){
        .name = "Trace", .kind = PASS_GRAPHICS, .state = draw_state_opaque(SC_1, CULL_NONE),
        .pipe = pipe_intern_graphics(d->gpu, d->fullscreen_vs, demo_shader(d, "demos/sdf/shaders/trace.frag.spv", STAGE_FRAGMENT),
                                     FMT_RGBA16F, FMT_NONE, BLEND_OPAQUE),
        .colors = (AttachDesc[]){attach_color(a->trace, ATT_WRITE)}, .color_count = 1u};
    /* Present samples the trace target with a linear filter, which is the
     * upsample when the trace ran at half resolution. */
    passes[1]  = demo_present_pass(d, a->trace);
    d->program = demo_program(d, passes, 2u);
}

static void app_ui(App *a, struct nk_context *ctx) {
    Demo *d = &a->demo;
    float steps = (float)a->max_steps, sh = (float)a->shadow_steps;
    if (nk_begin(ctx, "SDF", nk_rect(10, 10, 300, 400),
                 NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_TITLE)) {
        demo_ui_text(ctx, "%.1f ms   trace %ux%u", (double)d->dt * 1000.0, a->trace_w, a->trace_h);
        demo_ui_choice(ctx, "View", k_views, 5u, &a->view);
        demo_ui_choice(ctx, "Tone map", demo_tonemap_names, DEMO_TONEMAP_COUNT, &d->present.tonemap);
        demo_ui_float(ctx, "Max steps", &steps, 8.0f, 256.0f);
        demo_ui_float(ctx, "Shadow steps", &sh, 0.0f, 96.0f);
        demo_ui_float(ctx, "Shadow hardness", &a->shadow_k, 2.0f, 64.0f);
        demo_ui_float(ctx, "Ambient occlusion", &a->ao, 0.0f, 1.0f);
        demo_ui_float(ctx, "Smooth blend", &a->blend, 0.01f, 0.8f);
        demo_ui_float(ctx, "Fog", &a->fog, 0.0f, 0.01f);
        demo_ui_float(ctx, "Sun yaw", &a->sun_yaw, -3.14f, 3.14f);
        demo_ui_float(ctx, "Sun height", &a->sun_height, 0.05f, 1.5f);
        demo_ui_float(ctx, "Exposure (EV)", &d->present.exposure_ev, -3.0f, 3.0f);
        a->max_steps    = (uint32_t)steps;
        a->shadow_steps = (uint32_t)sh;
    }
    nk_end(ctx);
}

/* Render the calibration sphere at three distances and compare the hit-pixel
 * count with the area of its perspective silhouette; also step statistics. */
static void app_check(App *a) {
    static const float cases[3][2] = {{4.0f, 1.0f}, {9.0f, 1.0f}, {2.5f, 0.6f}};   /* distance, radius */
    Demo        *d     = &a->demo;
    const size_t count = (size_t)a->trace_w * a->trace_h;
    uint32_t     c;
    size_t       i;

    a->max_steps = 128u;
    a->scene     = 1u;
    a->orbit.target[0] = a->orbit.target[1] = a->orbit.target[2] = 0.0f;
    for (c = 0u; c < 3u; ++c) {
        const double distance = cases[c][0], radius = cases[c][1];
        double       hits = 0.0, sum = 0.0, exhausted = 0.0, rpx, expected, err;
        float       *px;
        char         label[64];
        a->radius         = cases[c][1];
        a->orbit.distance = cases[c][0];
        app_frame(a);
        px = demo_read_floats(d, a->trace, count * 4u);
        for (i = 0u; i < count; ++i) {
            double v = px[i * 4u + 3u];                          /* alpha: steps, + 1024 on a hit */
            if (v >= 1024.0) { hits += 1.0; v -= 1024.0; }
            sum += v;
            if (v >= (double)a->max_steps) exhausted += 1.0;     /* ran out of budget */
        }
        free(px);
        /* A sphere centred on the view axis projects to a circle: the tangent cone
         * has half-angle asin(r/d), which is tan() of that on the plane z = 1. */
        rpx      = tan(asin(radius / distance)) / tan((double)a->orbit.fov_deg * 0.017453292519943295 * 0.5) * 0.5 * (double)a->trace_h;
        expected = 3.141592653589793 * rpx * rpx;
        err      = (hits - expected) / expected;
        snprintf(label, sizeof label, "coverage r=%.1f d=%.1f (rel. error)", radius, distance);
        demo_check(d, label, err, "|x| <= 0.01", fabs(err) <= 0.01);
        demo_check(d, "  mean steps per pixel", sum / (double)count, "< 16", sum / (double)count < 16.0);
        /* Rays grazing the silhouette converge slowly; they are the only ones
         * allowed to use the whole budget. */
        demo_check(d, "  rays that exhaust 128 steps", exhausted / (double)count, "< 0.5 %", exhausted / (double)count < 0.005);
    }
}

int main(int argc, char **argv) {
    static App app;
    static const DemoOption options[] = {
        {"--half", .flag = &app.half, .help = "trace at half resolution, upsample in Present"},
        {"--view", .u32 = &app.view, .lo = 0, .hi = 4, .help = "0 shaded, 1 normals, 2 step count, 3 AO, 4 sun shadow"},
        {"--steps", .u32 = &app.max_steps, .lo = 1, .hi = 1023, .help = "march budget per ray"},
        {0}};
    demo_defaults(&app.demo, "sdf", "SDF - sphere tracing (mu_lib sdf.slang)");
    app.orbit = (DemoOrbit){.target = {0.0f, 0.9f, 0.0f}, .distance = 7.5f, .yaw = 0.6f, .pitch = 0.32f,
                            .fov_deg = 45.0f, .znear = 0.05f, .zfar = 60.0f,
                            .min_distance = 2.0f, .max_distance = 30.0f};
    app.max_steps    = 96u;
    app.shadow_steps = 48u;
    app.shadow_k     = 14.0f;
    app.ao           = 1.0f;
    app.fog          = 0.002f;
    app.blend        = 0.35f;
    app.sun_yaw      = -0.9f;
    app.sun_height   = 0.75f;
    app.radius       = 1.0f;
    demo_open(&app.demo, options, argc, argv);
    app_init(&app);
    while (demo_running(&app.demo)) {
        if (app.demo.ui) app_ui(&app, app.demo.ui);
        app_frame(&app);
    }
    if (app.demo.check) app_check(&app);
    return demo_close(&app.demo);
}
