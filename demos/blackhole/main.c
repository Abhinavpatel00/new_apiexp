/* blackhole — what a camera sees next to a black hole.
 *
 *   Trace (full-screen, RGBA16F) -> Present
 *
 * Every pixel follows one light ray backwards through the curved space
 * around a Schwarzschild black hole until it falls in, crosses the disc of
 * gas or leaves for the stars. See shaders/blackhole.slang for the equation.
 *
 * --check compares the picture with general relativity: the size of the
 * hole's shadow, the direction each ray leaves in (against the orbit equation
 * integrated on the CPU in polar form), Einstein's deflection angle far from
 * the hole, and the range of redshifts gas on a circular orbit can show.
 */
#include "demo.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct Push {
    float    eye_time[4], right_tan[4], up_aspect[4], forward_escape[4], disc[4], look[4];
    uint32_t view, max_steps, flags, pad;
} Push;

typedef struct App {
    Demo      demo;
    DemoOrbit orbit;
    Target    trace;
    Push      push;
    /* options */
    uint32_t view, max_steps;
    bool     flat, disc, doppler, grid;
    float    inner, outer, brightness, temperature, step, escape;
} App;

static const char *const k_views[] = {"picture", "picture", "leaving direction", "redshift"};

static void app_frame(App *a) {
    Demo   *d = &a->demo;
    Push   *p = &a->push;
    PassCtx *pass;
    float   view[16], eye[3];
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
    p->forward_escape[0] = -view[2]; p->forward_escape[1] = -view[6]; p->forward_escape[2] = -view[10];
    p->forward_escape[3] = fmaxf(a->escape, 2.0f * a->orbit.distance);
    p->disc[0] = a->inner; p->disc[1] = a->outer; p->disc[2] = a->brightness;
    p->disc[3] = a->flat ? 0.0f : 1.0f;
    p->look[0] = a->temperature;
    p->look[1] = a->step;
    p->view      = a->view;
    p->max_steps = a->max_steps;
    p->flags     = (a->disc ? 1u : 0u) | (a->doppler ? 2u : 0u) | (a->grid ? 4u : 0u);

    if (!demo_frame_begin(d)) return;
    pass = demo_pass(d, "Trace");
    pass_push(pass, p, sizeof *p);
    pass_draw_fullscreen_triangle(pass, d->width, d->height);
    demo_present(d);
    demo_frame_end(d);
}

static void app_init(App *a) {
    Demo    *d = &a->demo;
    PassDesc passes[3];
    /* Half floats hold a direction to 1 part in 2000; --check wants more. */
    const Format format = d->check ? FMT_RGBA32F : FMT_RGBA16F;

    a->trace  = demo_target(d, "blackhole_trace", format, d->width, d->height, false, NULL);
    passes[0] = (PassDesc){
        .name = "Trace", .kind = PASS_GRAPHICS, .state = draw_state_opaque(SC_1, CULL_NONE),
        .pipe = pipe_intern_graphics(d->gpu, d->fullscreen_vs,
                                     demo_shader(d, "demos/blackhole/shaders/trace.frag.spv", STAGE_FRAGMENT),
                                     format, FMT_NONE, BLEND_OPAQUE),
        .colors = (AttachDesc[]){attach_color(a->trace, ATT_WRITE)}, .color_count = 1u};
    passes[1]  = demo_present_pass(d, a->trace);
    d->program = demo_program(d, passes, 2u);
}

static void app_ui(App *a, struct nk_context *ctx) {
    Demo *d = &a->demo;
    float steps = (float)a->max_steps;
    if (nk_begin(ctx, "Black hole", nk_rect(10, 10, 300, 400),
                 NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_TITLE)) {
        demo_ui_text(ctx, "%.1f ms   camera at r = %.1f M", (double)d->dt * 1000.0, (double)a->orbit.distance);
        demo_ui_choice(ctx, "View", k_views, 4u, &a->view);
        demo_ui_bool(ctx, "Flat space (no bending)", &a->flat);
        demo_ui_bool(ctx, "Disc", &a->disc);
        demo_ui_bool(ctx, "Doppler and redshift", &a->doppler);
        demo_ui_bool(ctx, "Latitude / longitude sky", &a->grid);
        demo_ui_float(ctx, "Disc temperature (K)", &a->temperature, 2000.0f, 12000.0f);
        demo_ui_float(ctx, "Disc brightness", &a->brightness, 0.0f, 20.0f);
        demo_ui_float(ctx, "Disc outer radius (M)", &a->outer, 8.0f, 30.0f);
        demo_ui_float(ctx, "Step (fraction of r)", &a->step, 0.01f, 0.3f);
        demo_ui_float(ctx, "Max steps", &steps, 16.0f, 1000.0f);
        demo_ui_choice(ctx, "Tone map", demo_tonemap_names, DEMO_TONEMAP_COUNT, &d->present.tonemap);
        demo_ui_float(ctx, "Exposure (EV)", &d->present.exposure_ev, -4.0f, 4.0f);
        a->max_steps = (uint32_t)steps;
    }
    nk_end(ctx);
}

/* ---- acceptance ------------------------------------------------------------ */

/* The ray through pixel (x, y), as the shader builds it: the direction the
 * camera sees, and the one it has in coordinates. Returns the impact
 * parameter b (angular momentum per unit energy). */
static double pixel_ray(const App *a, uint32_t x, uint32_t y, double look[3], double coord[3]) {
    const Demo *d = &a->demo;
    const Push *p = &a->push;
    /* Row 0 of a readback is the top of the picture, where the shader's uv.y is 1. */
    const double nx = ((double)x + 0.5) / d->width * 2.0 - 1.0, ny = 1.0 - ((double)y + 0.5) / d->height * 2.0;
    const double r0 = sqrt((double)p->eye_time[0] * p->eye_time[0] + (double)p->eye_time[1] * p->eye_time[1] +
                           (double)p->eye_time[2] * p->eye_time[2]);
    const double f0 = 1.0 - 2.0 * p->disc[3] / r0;
    double along = 0.0, len = 0.0, across = 0.0;
    uint32_t i;
    for (i = 0u; i < 3u; ++i) {
        look[i] = p->forward_escape[i] + p->right_tan[i] * (nx * p->up_aspect[3] * p->right_tan[3]) +
                  p->up_aspect[i] * (ny * p->right_tan[3]);
        len += look[i] * look[i];
    }
    for (i = 0u; i < 3u; ++i) { look[i] /= sqrt(len); along += look[i] * p->eye_time[i] / r0; }
    len = 0.0;
    for (i = 0u; i < 3u; ++i) {
        const double radial = p->eye_time[i] / r0, tangent = look[i] - radial * along;
        coord[i] = radial * along * sqrt(f0) + tangent;
        across += tangent * tangent;
        len += coord[i] * coord[i];
    }
    for (i = 0u; i < 3u; ++i) coord[i] /= sqrt(len);
    return r0 * sqrt(across) / sqrt(f0);
}

static void app_check(App *a) {
    static const double k_pi = 3.141592653589793, k_critical = 5.196152422706632;   /* 3 sqrt(3) M */
    Demo        *d = &a->demo;
    const size_t count = (size_t)d->width * d->height;
    float       *px = (float *)malloc(count * 16u);
    const uint32_t row = d->height / 2u;
    char         label[128];
    uint32_t     c, x;
    size_t       i;

    a->orbit.yaw = 0.0f;
    a->orbit.pitch = 0.0f;
    a->max_steps = 4000u;
    a->step      = 0.02f;
    a->disc      = false;
    a->view      = 2u;

    /* 1. The shadow. Light that passes closer than b = 3 sqrt(3) M falls in. A
     * camera hovering at r sees that as a disc of angular radius
     * asin(3 sqrt(3) M sqrt(1 - 2M/r) / r) (Synge 1966). */
    for (c = 0u; c < 3u; ++c) {
        static const float k_distance[3] = {15.0f, 30.0f, 60.0f};
        const double r0 = k_distance[c], half = tan((double)a->orbit.fov_deg * k_pi / 360.0);
        const double radius = tan(asin(k_critical * sqrt(1.0 - 2.0 / r0) / r0)) / half * 0.5 * d->height;
        const double horizon = 2.0 / sqrt(r0 * r0 - 4.0) / half * 0.5 * d->height;   /* a ball of radius 2M, no bending */
        double fell = 0.0, undecided = 0.0;
        a->orbit.distance = k_distance[c];
        app_frame(a);
        demo_readback(d, a->trace, px, count * 16u);
        for (i = 0u; i < count; ++i) {
            fell += px[i * 4u + 3u] == 1.0f;
            undecided += px[i * 4u + 3u] == 2.0f;
        }
        snprintf(label, sizeof label, "shadow seen from r = %.0f M: area / pi (b_c sqrt(1 - 2M/r) / r)^2 - 1", r0);
        demo_check(d, label, fell / (k_pi * radius * radius) - 1.0, "|x| <= 0.01",
                   fabs(fell / (k_pi * radius * radius) - 1.0) <= 0.01);
        demo_check(d, "  its radius / the radius of the horizon without bending (reported)", sqrt(fell / k_pi) / horizon, "", true);
        demo_check(d, "  rays that ran out of steps", undecided, "<= 8", undecided <= 8.0);
    }

    /* 2. Where each ray goes, against the orbit equation u'' + u = 3 M u^2
     * (u = 1 / r) integrated in polar form in double precision: a different
     * formulation from the shader's, which pushes a particle in x, y, z. */
    {
        double worst = 0.0, worst_near = 0.0, disagree = 0.0, compared = 0.0;
        a->orbit.distance = 30.0f;
        app_frame(a);
        demo_readback(d, a->trace, px, count * 16u);
        for (x = 0u; x < d->width; x += 4u) {
            const float *out = &px[((size_t)row * d->width + x) * 4u];
            const double r0 = 30.0, h = 2.0e-4;
            double look[3], coord[3], inward = 0.0, across[3], across_len = 0.0, u, w, phi = 0.0, error = 0.0;
            const double b = pixel_ray(a, x, row, look, coord);
            uint32_t k, n = 0u;
            bool     fell = false;
            /* Angle between the ray and the inward radius, in coordinates. */
            for (k = 0u; k < 3u; ++k) inward -= coord[k] * a->push.eye_time[k] / r0;
            for (k = 0u; k < 3u; ++k) { across[k] = coord[k] + inward * a->push.eye_time[k] / r0; across_len += across[k] * across[k]; }
            u = 1.0 / r0;
            w = u * inward / sqrt(across_len);            /* du/dphi = u / tan(angle) */
            while (u > 0.0 && !fell && n++ < 400000u) {   /* RK4 in phi */
                const double k1u = w,                  k1w = -u + 3.0 * u * u;
                const double u2 = u + 0.5 * h * k1u,   k2u = w + 0.5 * h * k1w, k2w = -u2 + 3.0 * u2 * u2;
                const double u3 = u + 0.5 * h * k2u,   k3u = w + 0.5 * h * k2w, k3w = -u3 + 3.0 * u3 * u3;
                const double u4 = u + h * k3u,         k4u = w + h * k3w,       k4w = -u4 + 3.0 * u4 * u4;
                const double nu = u + h / 6.0 * (k1u + 2.0 * k2u + 2.0 * k3u + k4u);
                if (nu <= 0.0) { phi += h * u / (u - nu); u = nu; break; }   /* r = infinity: the direction is phi */
                phi += h;
                u = nu;
                w += h / 6.0 * (k1w + 2.0 * k2w + 2.0 * k3w + k4w);
                fell = u > 0.5;
            }
            disagree += fell != (out[3] == 1.0f) && fabs(b - k_critical) > 0.05;
            if (fell || out[3] != 0.0f) continue;
            /* The ray leaves in the direction at angle phi from the camera's radius, in the plane of the orbit. */
            for (k = 0u; k < 3u; ++k) {
                const double predicted = cos(phi) * a->push.eye_time[k] / r0 + sin(phi) * across[k] / sqrt(across_len);
                error += (predicted - out[k]) * (predicted - out[k]);
            }
            compared += 1.0;
            if (b > 1.1 * k_critical && sqrt(error) > worst) worst = sqrt(error);
            if (b <= 1.1 * k_critical && sqrt(error) > worst_near) worst_near = sqrt(error);
        }
        demo_check(d, "rays compared with the orbit equation", compared, ">= 100", compared >= 100.0);
        demo_check(d, "  fell in / left: rays where the two disagree (|b - b_c| > 0.05 M)", disagree, "== 0", disagree == 0.0);
        demo_check(d, "  leaving direction, b > 1.1 b_c: largest error (rad)", worst, "< 1e-4", worst < 1e-4);
        demo_check(d, "  leaving direction, b_c < b < 1.1 b_c, where rays circle the hole (rad, reported)", worst_near, "", true);
    }

    /* 3. Einstein's deflection. Far from the hole a ray that passes at b is
     * turned by 4 M / b: twice what Newton's gravity gives, and what the 1919
     * eclipse measured. The next term of the series is 15 pi / 4 (M / b)^2. */
    {
        double worst = 0.0, first_order = 0.0, at = 0.0, compared = 0.0;
        a->orbit.distance = 4000.0f;
        a->orbit.fov_deg  = 5.0f;
        a->escape         = 16000.0f;
        app_frame(a);
        demo_readback(d, a->trace, px, count * 16u);
        for (x = 0u; x < d->width; x += 4u) {
            const float *out = &px[((size_t)row * d->width + x) * 4u];
            double look[3], coord[3], cross[3], angle, series;
            const double b = pixel_ray(a, x, row, look, coord);
            if (b < 60.0 || out[3] != 0.0f) continue;
            cross[0] = coord[1] * out[2] - coord[2] * out[1];
            cross[1] = coord[2] * out[0] - coord[0] * out[2];
            cross[2] = coord[0] * out[1] - coord[1] * out[0];
            angle  = asin(sqrt(cross[0] * cross[0] + cross[1] * cross[1] + cross[2] * cross[2]));
            series = 4.0 / b + 15.0 * k_pi / 4.0 / (b * b);
            compared += 1.0;
            if (fabs(angle / series - 1.0) > worst) worst = fabs(angle / series - 1.0);
            if (fabs(b - 100.0) < fabs(at - 100.0)) { at = b; first_order = angle / (4.0 / b) - 1.0; }
        }
        demo_check(d, "rays that pass at b >= 60 M", compared, ">= 50", compared >= 50.0);
        demo_check(d, "  deflection / (4M/b + 15 pi/4 (M/b)^2) - 1: largest", worst, "< 0.005", worst < 0.005);
        demo_check(d, "  deflection / (4M/b) - 1 at b = 100 M (the next term says 0.029)", first_order, "0.025 .. 0.033",
                   first_order > 0.025 && first_order < 0.033);
        a->orbit.fov_deg = 50.0f;
        a->escape        = 200.0f;
    }

    /* 4. The disc seen exactly edge-on. In flat space it is a line and covers
     * no pixel; here the far side shows above and below the hole. */
    {
        double flat = 0.0, curved = 0.0;
        a->orbit.distance = 30.0f;
        a->disc = true;
        a->view = 3u;
        a->flat = true;
        app_frame(a);
        demo_readback(d, a->trace, px, count * 16u);
        for (i = 0u; i < count; ++i) flat += px[i * 4u + 3u] >= 4.0f;
        a->flat = false;
        app_frame(a);
        demo_readback(d, a->trace, px, count * 16u);
        for (i = 0u; i < count; ++i) curved += px[i * 4u + 3u] >= 4.0f;
        demo_check(d, "edge-on disc, flat space: pixels that show it", flat, "== 0", flat == 0.0);
        demo_check(d, "edge-on disc, curved space: fraction of pixels that show it", curved / (double)count, "> 0.02",
                   curved / (double)count > 0.02);
    }

    /* 5. Redshift. Light from gas circling at radius r carries at most
     * r / sqrt(1 - 2M/r) of angular momentum per unit energy, so g, the
     * frequency seen over the frequency sent, lies between two bounds that
     * depend on r alone. The shader gets g from the ray's angular momentum at
     * the camera and r from where the ray met the disc. */
    {
        const double f0 = 1.0 - 2.0 / 30.0;
        double outside = 0.0, low = 1e9, high = 0.0, discs = 0.0;
        a->orbit.pitch = 0.12f;
        app_frame(a);
        demo_readback(d, a->trace, px, count * 16u);
        for (i = 0u; i < count; ++i) {
            const double g = px[i * 4u], r = px[i * 4u + 1u];
            const double omega = sqrt(1.0 / (r * r * r)), most = r / sqrt(1.0 - 2.0 / r), clock = sqrt(1.0 - 3.0 / r) / sqrt(f0);
            if (px[i * 4u + 3u] < 4.0f) continue;
            discs += 1.0;
            outside += g < clock / (1.0 + omega * most) * 0.999 || g > clock / (1.0 - omega * most) * 1.001;
            if (g < low) low = g;
            if (g > high) high = g;
        }
        demo_check(d, "disc pixels", discs, "> 1000", discs > 1000.0);
        demo_check(d, "  redshift outside what a circular orbit at that radius allows", outside, "== 0", outside == 0.0);
        demo_check(d, "  bluest, on the side coming towards the camera (g)", high, "> 1.1", high > 1.1);
        demo_check(d, "  reddest, on the side going away (g)", low, "< 0.75", low < 0.75);
    }
    free(px);
}

int main(int argc, char **argv) {
    static App app;
    static const DemoOption options[] = {
        {"--view", .u32 = &app.view, .lo = 0, .hi = 3, .help = "0 picture, 2 leaving direction, 3 redshift"},
        {"--flat", .flag = &app.flat, .help = "no bending: the same scene in flat space"},
        {"--no-disc", .off = &app.disc, .help = "the hole and the sky alone"},
        {"--no-doppler", .off = &app.doppler, .help = "the disc as it would look at rest"},
        {"--grid", .flag = &app.grid, .help = "a sky of latitude and longitude lines instead of stars"},
        {"--distance", .f32 = &app.orbit.distance, .lo = 4, .hi = 200, .help = "camera radius, in M"},
        {"--tilt", .f32 = &app.orbit.pitch, .lo = -1.5, .hi = 1.5, .help = "camera height above the disc, radians"},
        {"--temperature", .f32 = &app.temperature, .lo = 1000, .hi = 40000, .help = "hottest part of the disc, kelvin"},
        {"--steps", .u32 = &app.max_steps, .lo = 1, .hi = 4000, .help = "step budget per ray"},
        {0}};
    demo_defaults(&app.demo, "blackhole", "Black hole - light paths in curved space");
    app.orbit = (DemoOrbit){.target = {0.0f, 0.0f, 0.0f}, .distance = 34.0f, .yaw = 0.5f, .pitch = 0.11f,
                            .fov_deg = 50.0f, .znear = 0.1f, .zfar = 1000.0f,
                            .min_distance = 4.0f, .max_distance = 200.0f};
    app.max_steps   = 400u;
    app.step        = 0.06f;
    app.escape      = 200.0f;
    app.disc        = true;
    app.doppler     = true;
    app.inner       = 6.0f;       /* the innermost stable circular orbit */
    app.outer       = 17.0f;
    app.brightness  = 1.4f;
    app.temperature = 4800.0f;
    demo_open(&app.demo, options, argc, argv);
    app_init(&app);
    while (demo_running(&app.demo)) {
        if (app.demo.ui) app_ui(&app, app.demo.ui);
        app_frame(&app);
    }
    if (app.demo.check) app_check(&app);
    return demo_close(&app.demo);
}
