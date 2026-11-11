/* pointlight — point lights whose brightness stays finite next to a surface.
 *
 *   Light (fullscreen, one ray per pixel, every light for every pixel) -> Present
 *
 * Cem Yuksel: "Point Light Attenuation Without Singularity" (SIGGRAPH 2020 Talks). The method
 * is one line and lives in shaders/pointlight.slang (`attenuation`); the scenes are described
 * there too. Left of the line the lights fall off with 1 / d^2, right of it with the paper's
 * eq. 7.
 *
 * --check reads the shader's three functions back and compares them with the formulas, the
 * paper's claims about them (limits, order, value at 0), the solid angle that eq. 7 stands for
 * and the light of a real glowing ball by numerical integration, how much the printed form of
 * eq. 7 loses in single precision, and in the picture: the brightest point, and how much the
 * picture changes with the number of virtual lights.
 */
#include "demo.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PI     3.14159265358979323846
#define PLACED 0.35   /* intensity of the lights of scene 1 */

typedef struct Push {
    float    eye_radius[4], right_tan[4], up_aspect[4], forward_time[4], spot[4], power[4];
    uint32_t view, lights, scene, hidden;
} Push;

typedef struct App {
    Demo      demo;
    DemoOrbit orbit;
    Target    image;
    uint32_t  view, lights, scene;
    float     radius, clock;
    bool      direct, still, hidden;
} App;

static const char *const k_views[] = {"1 / d^2 | eq. 7", "1 / d^2", "eq. 7", "1 / (d^2 + r^2 / 2), eq. 8"};
static const char *const k_scenes[] = {"virtual point lights", "lights placed near surfaces"};

static void app_frame(App *a) {
    Demo       *d = &a->demo;
    Push        p = {0};
    PassCtx    *pass;
    float       view[16], eye[3];
    const float aspect = (float)d->width / (float)d->height;
    float       sway, length;

    a->clock = a->still ? a->clock : d->time;
    sway = 0.25f * sinf(0.6f * a->clock); length = sqrtf(sway * sway + 1.0f + 0.3f * 0.3f);
    demo_orbit_input(d, &a->orbit);
    demo_orbit_camera(&a->orbit, aspect, view, NULL, NULL, eye);
    /* rows of the rotation part of a column-major view matrix are the camera axes; it looks down -z */
    p.eye_radius[0] = eye[0]; p.eye_radius[1] = eye[1]; p.eye_radius[2] = eye[2]; p.eye_radius[3] = a->radius;
    p.right_tan[0] = view[0]; p.right_tan[1] = view[4]; p.right_tan[2] = view[8]; p.right_tan[3] = tanf(a->orbit.fov_deg * 0.017453292f * 0.5f);
    p.up_aspect[0] = view[1]; p.up_aspect[1] = view[5]; p.up_aspect[2] = view[9]; p.up_aspect[3] = aspect;
    p.forward_time[0] = -view[2]; p.forward_time[1] = -view[6]; p.forward_time[2] = -view[10]; p.forward_time[3] = a->clock;
    p.spot[0] = sway / length; p.spot[1] = -1.0f / length; p.spot[2] = -0.3f / length; p.spot[3] = 0.66f;
    p.power[0] = 9.0f; p.power[1] = (float)PLACED; p.power[2] = a->direct ? 0.0f : 1.0f;
    p.view = a->view; p.lights = a->lights; p.scene = a->scene; p.hidden = a->hidden;

    if (!demo_frame_begin(d)) return;
    pass = demo_pass(d, "Light");
    pass_push(pass, &p, sizeof p);
    pass_draw_fullscreen_triangle(pass, d->width, d->height);
    demo_present(d);
    demo_frame_end(d);
}

static void app_ui(App *a, struct nk_context *ctx) {
    Demo *d = &a->demo;
    float lights = (float)a->lights;
    if (nk_begin(ctx, "Point lights", nk_rect(16, 16, 320, 260), NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_TITLE)) {
        demo_ui_text(ctx, "%.1f ms", (double)d->dt * 1000.0);
        demo_ui_choice(ctx, "Scene", k_scenes, 2u, &a->scene);
        demo_ui_choice(ctx, "Attenuation", k_views, 4u, &a->view);
        demo_ui_float(ctx, "Radius r of the lights", &a->radius, 0.01f, 1.0f);
        demo_ui_float(ctx, "Virtual lights", &lights, 1.0f, 256.0f);
        demo_ui_bool(ctx, "The spot's own light too", &a->direct);
        demo_ui_bool(ctx, "Still", &a->still);
        demo_ui_float(ctx, "Exposure (EV)", &d->present.exposure_ev, -4.0f, 4.0f);
    }
    nk_end(ctx);
    a->lights = (uint32_t)lights;
}

static void app_check(App *a) {
    Demo        *d = &a->demo;
    const size_t count = (size_t)d->width * d->height;
    float       *px = (float *)malloc(count * 16u), *other = (float *)malloc(count * 16u);
    char         label[200];
    uint32_t     c, x;
    size_t       i;
    int          k, j;

    a->still = true; a->clock = 1.0f;

    /* 1: the shader's functions against the formulas in double precision */
    for (c = 0u; c < 3u; ++c) {
        const double r = 0.5 + 0.5 * c;
        double       worst[3] = {0};
        a->radius = (float)r; a->view = 4u;
        app_frame(a);
        demo_readback(d, a->image, px, count * 16u);
        for (x = 0u; x < d->width; ++x) {
            const float *q = px + ((size_t)(d->height / 2u) * d->width + x) * 4u;
            const double dist = q[3];
            worst[0] = fmax(worst[0], fabs(q[0] * dist * dist - 1.0));
            worst[1] = fmax(worst[1], fabs(q[1] / (2.0 / (r * r) * (1.0 - dist / sqrt(dist * dist + r * r))) - 1.0));
            worst[2] = fmax(worst[2], fabs(q[2] * (dist * dist + 0.5 * r * r) - 1.0));
        }
        snprintf(label, sizeof label, "shader, r = %.1f, d from 0 to 8: 1 / d^2, largest relative difference from the formula", r);
        demo_check(d, label, worst[0], "< 1e-5", worst[0] < 1e-5);
        demo_check(d, "  eq. 7", worst[1], "< 1e-5", worst[1] < 1e-5);
        demo_check(d, "  eq. 8", worst[2], "< 1e-5", worst[2] < 1e-5);
    }

    /* 2: what the paper says about the functions (r = 1; they scale as f(d / r) / r^2) */
    {
        double order = 0.0, rising = 0.0, before = 2.0;
        for (k = 1; k <= 100000; ++k) {
            const double dist = 1e-3 * k, ours = 2.0 * (1.0 - dist / sqrt(dist * dist + 1.0)), usual = 1.0 / (dist * dist + 0.5), square = 1.0 / (dist * dist);
            order += !(ours <= usual && usual <= square);
            rising += ours > before;
            before = ours;
        }
        demo_check(d, "eq. 7 at d = 0, times r^2", 2.0 * (1.0 - 0.0), "= 2", true);
        demo_check(d, "d / r from 0.001 to 100: points where eq. 7 <= eq. 8 <= 1 / d^2 does not hold", order, "= 0", order == 0.0);
        demo_check(d, "  points where eq. 7 rises with d", rising, "= 0", rising == 0.0);
        for (k = 0; k < 6; ++k) {
            static const double k_far[6] = {0.5, 1, 2, 5, 10, 100};
            const double        dist = k_far[k], ours = 2.0 / (dist * dist + 1.0 + dist * sqrt(dist * dist + 1.0)) * dist * dist, bound = k < 4 ? 1.0 : 1.0 / (dist * dist);
            snprintf(label, sizeof label, "  eq. 7 / (1 / d^2) at d = %g r", dist);
            demo_check(d, label, ours, k < 4 ? "info" : "1 - (r / d)^2 .. 1", ours <= 1.0 && ours >= 1.0 - bound);
            snprintf(label, sizeof label, "  eq. 8 / (1 / d^2) at d = %g r (info)", dist);
            demo_check(d, label, dist * dist / (dist * dist + 0.5), "any", true);
        }
    }

    /* 3: eq. 7 is the solid angle of a disc of radius r, seen from its axis at distance d, over pi r^2 (r = 1) */
    {
        double worst = 0.0;
        for (k = 0; k < 6; ++k) {
            static const double k_at[6] = {0.01, 0.1, 0.5, 1, 3, 10};
            const double        dist = k_at[k];
            double              sum = 0.0;
            /* rings of the disc: area 2 pi s ds, each seen under dist / (dist^2 + s^2)^(3/2); midpoint rule in s^(1/2) */
            for (j = 0; j < 400000; ++j) {
                const double u = (j + 0.5) / 400000.0, ring = u * u;
                sum += 2.0 * PI * ring * (2.0 * u / 400000.0) * dist / pow(dist * dist + ring * ring, 1.5);
            }
            worst = fmax(worst, fabs(sum / PI / (2.0 * (1.0 - dist / sqrt(dist * dist + 1.0))) - 1.0));
        }
        demo_check(d, "solid angle of a facing disc by summing rings / (pi r^2 eq. 7) - 1, d = 0.01 r .. 10 r, largest", worst, "< 1e-6", worst < 1e-6);
    }

    /* 4: a real ball of radius r that glows evenly with I / (pi r^2), on a surface that faces it: the sum over the
     * directions that meet the ball of cos(angle to the normal), against 1 / d^2, eq. 6 (the ball's solid angle with
     * the cosine taken at the centre) and eq. 7 */
    for (k = 0; k < 5; ++k) {
        static const double k_at[5] = {1.0, 1.5, 2.0, 4.0, 10.0};
        const double        dist = k_at[k], edge = sqrt(1.0 - 1.0 / (dist * dist));   /* cosine of the ball's angular radius */
        double              lit = 0.0, angle = 0.0;
        for (j = 0; j < 2000000; ++j) {
            const double cosine = edge + (1.0 - edge) * (j + 0.5) / 2000000.0;
            lit += 2.0 * PI * (1.0 - edge) / 2000000.0 * cosine; angle += 2.0 * PI * (1.0 - edge) / 2000000.0;
        }
        snprintf(label, sizeof label, "glowing ball at d = %g r, facing surface: light by summing directions, times d^2 / I", dist);
        demo_check(d, label, lit / PI * dist * dist, "= 1 (inverse square holds for a ball)", fabs(lit / PI * dist * dist - 1.0) < 1e-9);
        demo_check(d, "  eq. 6 with the ball's own solid angle / that (info)", angle / PI * dist * dist, "any", true);
        demo_check(d, "  eq. 7 / that (info)", 2.0 * (1.0 - dist / sqrt(dist * dist + 1.0)) * dist * dist, "any", true);
    }

    /* 5: eq. 7 as printed, 2 / r^2 (1 - d / sqrt(d^2 + r^2)), in single precision */
    for (k = 0; k < 4; ++k) {
        static const float k_far[4] = {10.0f, 100.0f, 1000.0f, 10000.0f};
        double             printed = 0.0, stable = 0.0;
        for (j = 0; j < 1000; ++j) {
            const volatile float dist = k_far[k] * (1.0f + 0.001f * j), root = sqrtf(dist * dist + 1.0f), ratio = dist / root, one = 2.0f * (1.0f - ratio);
            const volatile float sum = dist * dist + 1.0f + dist * root, two = 2.0f / sum;
            const double         exact = 2.0 / ((double)dist * dist + 1.0 + (double)dist * sqrt((double)dist * dist + 1.0));
            printed = fmax(printed, fabs(one / exact - 1.0)); stable = fmax(stable, fabs(two / exact - 1.0));
        }
        snprintf(label, sizeof label, "single precision, d from %g r to %g r: largest relative error of eq. 7 as printed (info)", (double)k_far[k], 2.0 * k_far[k]);
        demo_check(d, label, printed, "any", true);
        demo_check(d, "  of 2 / (d^2 + r^2 + d sqrt(d^2 + r^2)), the form the shader uses", stable, "< 1e-6", stable < 1e-6);
    }

    /* 6: the picture with virtual point lights, their light alone */
    a->scene = 0u; a->direct = false; a->radius = 0.25f;
    {
        double brightest[4] = {0}, mean[4] = {0}, change[4] = {0};
        for (c = 1u; c <= 3u; ++c) {
            double total = 0.0, difference = 0.0;
            a->view = c;
            a->lights = 256u;
            app_frame(a);
            demo_readback(d, a->image, other, count * 16u);
            a->lights = 32u;
            app_frame(a);
            demo_readback(d, a->image, px, count * 16u);
            for (i = 0u; i < count; ++i) {
                const double few = (px[i * 4u] + px[i * 4u + 1u] + px[i * 4u + 2u]) / 3.0, many = (other[i * 4u] + other[i * 4u + 1u] + other[i * 4u + 2u]) / 3.0;
                brightest[c] = fmax(brightest[c], few);
                mean[c] += many / count; total += many * many; difference += (few - many) * (few - many);
            }
            change[c] = sqrt(difference / total);
        }
        demo_check(d, "32 virtual lights, r = 0.25: brightest pixel with 1 / d^2", brightest[1], "> 3 times that with eq. 7", brightest[1] > 3.0 * brightest[2]);
        demo_check(d, "  with eq. 7", brightest[2], "info", true);
        demo_check(d, "  with eq. 8", brightest[3], ">= with eq. 7", brightest[3] >= brightest[2]);
        demo_check(d, "32 against 256 virtual lights, root mean square difference / root mean square: 1 / d^2", change[1], "> twice that with eq. 7", change[1] > 2.0 * change[2]);
        demo_check(d, "  with eq. 7", change[2], "< 0.25", change[2] < 0.25);
        demo_check(d, "  with eq. 8 (info)", change[3], "any", true);
        demo_check(d, "256 virtual lights: mean of the picture with eq. 7 / with 1 / d^2 (the light that eq. 7 takes away)", mean[2] / mean[1], "0.5 .. 1", mean[2] / mean[1] > 0.5 && mean[2] / mean[1] < 1.0);
        demo_check(d, "  with eq. 8 / with 1 / d^2", mean[3] / mean[1], "between that and 1", mean[3] / mean[1] >= mean[2] / mean[1] && mean[3] / mean[1] < 1.0);
    }

    /* 7: lights 0.04 from a surface, not drawn themselves. The green one is over a ball of colour 0.8; the camera
     * goes to 1.2 from that place (a pixel is 6 mm there) */
    a->scene = 1u; a->hidden = true; a->orbit.distance = 1.2f;
    a->orbit.target[0] = -0.9f + 0.5f * 0.8f * cosf(0.7f); a->orbit.target[1] = 0.5f + 0.5f * 0.6f; a->orbit.target[2] = -0.7f + 0.5f * 0.8f * sinf(0.7f);
    for (c = 0u; c < 3u; ++c) {
        static const float k_radius[3] = {0.05f, 0.15f, 0.5f};
        const double       r = k_radius[c], near = 0.8 / PI * PLACED * 2.0 / (r * r) * (1.0 - 0.04 / sqrt(0.04 * 0.04 + r * r));
        double             brightest[3] = {0};
        a->radius = k_radius[c];
        for (x = 1u; x <= 2u; ++x) {
            a->view = x;
            app_frame(a);
            demo_readback(d, a->image, px, count * 16u);
            for (i = 0u; i < count; ++i) brightest[x] = fmax(brightest[x], px[i * 4u + 1u]);
        }
        snprintf(label, sizeof label, "three lights 0.04 from a surface, r = %.2f: brightest green value with eq. 7 / what eq. 7 gives under the green light", r);
        demo_check(d, label, brightest[2] / near, "0.97 .. 1.08 (the other two lights add a little)", brightest[2] / near > 0.97 && brightest[2] / near < 1.08);
        demo_check(d, "  / the most a light of that intensity can give, 0.8 / pi * I * 2 / r^2", brightest[2] / (0.8 / PI * PLACED * 2.0 / (r * r)), "< 1", brightest[2] / (0.8 / PI * PLACED * 2.0 / (r * r)) < 1.0);
        demo_check(d, "  brightest green value with 1 / d^2 / with eq. 7", brightest[1] / brightest[2], "> 1", brightest[1] / brightest[2] > 1.0);
    }
    free(px); free(other);
}

int main(int argc, char **argv) {
    static App app;
    static const DemoOption options[] = {
        {"--scene", .u32 = &app.scene, .lo = 0, .hi = 1, .help = "0 virtual point lights from a spot light, 1 three lights placed near surfaces"},
        {"--view", .u32 = &app.view, .lo = 0, .hi = 3, .help = "0 left 1 / d^2 and right eq. 7, 1 1 / d^2, 2 eq. 7, 3 eq. 8"},
        {"--radius", .f32 = &app.radius, .lo = 0.01f, .hi = 1.0f, .help = "radius r of the lights"},
        {"--lights", .u32 = &app.lights, .lo = 1, .hi = 256, .help = "virtual point lights (scene 0)"},
        {"--direct", .flag = &app.direct, .help = "scene 0 with the spot's own light too, not only what the virtual lights carry on"},
        {"--still", .flag = &app.still, .help = "nothing moves"},
        {0}};
    Demo    *d = &app.demo;
    PassDesc passes[3];   /* demo_program adds the panel's pass when there is a window */
    Format   format;
    demo_defaults(d, "pointlight", "Point light attenuation");
    app.orbit = (DemoOrbit){.target = {0.0f, 1.2f, 0.0f}, .distance = 5.0f, .yaw = 0.0f, .pitch = 0.0f, .fov_deg = 50.0f, .znear = 0.1f, .zfar = 100.0f, .min_distance = 2.5f, .max_distance = 9.0f};
    app.lights = 64u; app.radius = 0.25f; d->present.exposure_ev = 1.0f;
    demo_open(d, options, argc, argv);
    /* half floats hold 1 part in 2000; --check wants more */
    format = d->check ? FMT_RGBA32F : FMT_RGBA16F;
    app.image = demo_target(d, "pointlight_image", format, d->width, d->height, false, NULL);
    passes[0] = (PassDesc){.name = "Light", .kind = PASS_GRAPHICS, .state = draw_state_opaque(SC_1, CULL_NONE),
                           .pipe = pipe_intern_graphics(d->gpu, d->fullscreen_vs, demo_shader(d, "demos/pointlight/shaders/light.frag.spv", STAGE_FRAGMENT), format, FMT_NONE, BLEND_OPAQUE),
                           .colors = (AttachDesc[]){attach_color(app.image, ATT_WRITE)}, .color_count = 1u};
    passes[1] = demo_present_pass(d, app.image);
    d->program = demo_program(d, passes, 2u);
    while (demo_running(d)) {
        if (d->ui) app_ui(&app, d->ui);
        app_frame(&app);
    }
    if (d->check) app_check(&app);
    return demo_close(d);
}
