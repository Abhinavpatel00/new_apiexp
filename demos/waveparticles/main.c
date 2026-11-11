/* waveparticles — water waves as particles that carry a bump each.
 *
 *   Draw -> Present          (the method runs in C: method.c)
 *
 * Yuksel, House, Keyser: "Wave Particles" (SIGGRAPH 2007), section 3. Scene 0
 * is rain on a pool with four walls, scene 1 a source that moves faster than
 * the waves on open water, scene 2 a train of straight waves against a wall.
 * --dots shows the particles.
 *
 * --check tests the bump of eq. 7, what section 3.2 says about a front of
 * particles half a radius apart, subdivision, that the step length does not
 * matter, the time table of events, walls, removal, the filtered image of
 * section 5 and the circular motion of section 3.4.
 */
#include "canvas.h"
#include "method.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { GRID = 128, LATER = 64 };
#define POOL 8.0
#define PI 3.14159265358979323846

typedef struct App {
    Demo      demo;   /* first: see demo.h */
    Canvas    canvas;
    DemoOrbit orbit;
    Wp        water;
    uint32_t  scene, built, frame, image, rng;
    float     damping;
    bool      no_warp, dots, restart, drop_now;
    struct { uint32_t frame; double x, y, amplitude; } later[LATER];   /* troughs that follow crests */
    uint32_t  laters;
    double    height[GRID * GRID], shift[GRID * GRID * 2], unused[GRID * GRID];
} App;

static void reset(Wp *s, double pool) {
    s->slots = s->events = s->spares = s->alive = 0u;
    s->visited = s->removed = 0u;
    s->speed = 1.5; s->radius = 0.3; s->damping = 0.0; s->threshold = 0.0; s->pool = pool; s->time = 0.0;
}

/* A ripple from a point: one particle that stands for the whole circle (section 4.1), and the
 * same with the other sign half a wavelength later, so that no water is added. */
static void drop(App *a, double x, double y, double amplitude) {
    wp_emit(&a->water, x, y, 0.0, amplitude, 2.0 * PI);
    a->later[a->laters % LATER].frame     = a->frame + (uint32_t)(60.0 * a->water.radius / a->water.speed);
    a->later[a->laters % LATER].x         = x;
    a->later[a->laters % LATER].y         = y;
    a->later[a->laters++ % LATER].amplitude = -amplitude;
}

static double noise(uint32_t *state) {
    *state = *state * 1664525u + 1013904223u;
    return (double)(*state >> 8) / 16777216.0;
}

static void seed(App *a) {
    reset(&a->water, a->scene == 1u ? 0.0 : POOL);
    /* a crest is at least twice a particle's amplitude: below a millimetre nobody sees it */
    a->water.threshold = a->scene == 1u ? 6e-4 : 3e-4;
    a->built = a->scene;
    a->frame = 0u; a->laters = 0u; a->rng = 3u;
    memset(a->later, 0, sizeof a->later);
}

static void app_ui(App *a, struct nk_context *ctx) {
    static const char *const k_scenes[3] = {"Rain on a pool", "A fast source, open water", "Straight waves at a wall"};
    static const char *const k_images[3] = {"Each bump as it is (eq. 7)", "Points, then a filter", "Points, then two 1-D filters"};
    bool warp = !a->no_warp;
    if (nk_begin(ctx, "Wave particles", nk_rect(16, 16, 330, 270), NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_TITLE)) {
        if (demo_ui_choice(ctx, "Scene", k_scenes, 3u, &a->scene)) a->restart = true;
        demo_ui_choice(ctx, "Height field", k_images, 3u, &a->image);
        demo_ui_float(ctx, "Damping, 1/s", &a->damping, 0.0f, 2.0f);
        demo_ui_bool(ctx, "Horizontal motion (3.4)", &warp);
        demo_ui_bool(ctx, "Show particles", &a->dots);
        demo_ui_text(ctx, "%u particles, %llu subdivided or removed so far", a->water.alive, (unsigned long long)a->water.visited);
        nk_layout_row_dynamic(ctx, 24, 2);
        if (nk_button_label(ctx, "Drop")) a->drop_now = true;
        if (nk_button_label(ctx, "Restart")) a->restart = true;
    }
    nk_end(ctx);
    a->no_warp = !warp;
}

static void app_frame(App *a) {
    Demo    *d = &a->demo;
    Canvas  *c = &a->canvas;
    Wp      *s = &a->water;
    uint32_t i, j, k;

    if (a->restart || a->built != a->scene) { seed(a); a->restart = false; }
    s->damping = a->damping;
    for (k = 0u; k < LATER; ++k)
        if (a->later[k].amplitude != 0.0 && a->later[k].frame == a->frame) wp_emit(s, a->later[k].x, a->later[k].y, 0.0, a->later[k].amplitude, 2.0 * PI);
    if (a->drop_now || (a->scene == 0u && a->frame % 30u == 0u)) drop(a, 1.0 + 6.0 * noise(&a->rng), 1.0 + 6.0 * noise(&a->rng), 0.5);
    a->drop_now = false;
    /* scene 1: round a circle of 2.4 m at 2.6 m/s; the waves travel at 1.5 */
    if (a->scene == 1u && a->frame % 6u == 0u) drop(a, 4.0 + 2.4 * cos(a->frame / 60.0 * 2.6 / 2.4), 4.0 + 2.4 * sin(a->frame / 60.0 * 2.6 / 2.4), 0.25);
    /* scene 2: every half wavelength a straight front leaves the wall x = 0, crest and trough in turn.
     * Particles half a radius apart add up to twice their amplitude along the crest. */
    if (a->scene == 2u && a->frame % 12u == 0u)
        for (k = 0u; k < 54u; ++k) wp_emit(s, 0.0, 0.075 * (2.0 * k + 1.0), 0.0, a->frame % 24u ? -0.02 : 0.02, 0.0);
    wp_advance(s, 1.0 / 60.0);
    a->frame++;

    if (a->image) wp_field_filtered(s, GRID, POOL / GRID, a->image == 2u, a->height);
    wp_field(s, GRID, POOL / GRID, a->image ? a->unused : a->height, a->shift);   /* with --image 1 or 2 only the shift is wanted */

    canvas_view_orbit(d, c, &a->orbit);
    for (j = 0u; j + 1u < GRID; ++j)
        for (i = 0u; i + 1u < GRID; ++i) {
            float corner[4][3];
            float mean = 0.0f;
            for (k = 0u; k < 4u; ++k) {
                const uint32_t x = i + (k == 1u || k == 2u), y = j + (k >= 2u), at = y * GRID + x;
                corner[k][0] = (float)((x + 0.5) * POOL / GRID - 0.5 * POOL + (a->no_warp ? 0.0 : a->shift[2u * at]));
                corner[k][1] = (float)a->height[at];
                corner[k][2] = (float)((y + 0.5) * POOL / GRID - 0.5 * POOL + (a->no_warp ? 0.0 : a->shift[2u * at + 1u]));
                mean += 0.25f * corner[k][1];
            }
            {
                const float    tint = fminf(fmaxf(0.5f + 14.0f * mean, 0.0f), 1.0f);
                const uint32_t rgba = CANVAS_RGB(30.0f + 110.0f * tint, 95.0f + 110.0f * tint, 150.0f + 90.0f * tint);
                canvas_lit(c, corner[0], corner[2], corner[1], rgba);
                canvas_lit(c, corner[0], corner[3], corner[2], rgba);
            }
        }
    for (k = 0u; k < s->slots && a->dots; ++k) {
        double at[2], unused[2];
        if (!s->particle[k].alive) continue;
        wp_place(s, &s->particle[k], at, unused);
        {
            /* a small flat triangle each: there can be fifty thousand */
            const float x = (float)(at[0] - 0.5 * POOL), z = (float)(at[1] - 0.5 * POOL), p[3][3] = {{x - 0.02f, 0.12f, z - 0.012f}, {x + 0.02f, 0.12f, z - 0.012f}, {x, 0.12f, z + 0.023f}};
            if (fabsf(x) < 0.5f * (float)POOL && fabsf(z) < 0.5f * (float)POOL) canvas_triangle(c, p[0], p[1], p[2], CANVAS_RGB(245, 245, 235));
        }
    }
    canvas_frame(d, c);
}

static void app_check(App *a) {
    Demo        *d = &a->demo;
    Wp          *s = &a->water;
    static Wp    other;
    static double field[256 * 256], second[256 * 256], shift[256 * 256 * 2];
    const double volume = 0.3 * 0.3 * (PI / 2.0 - 2.0 / PI);   /* of a bump of amplitude 1 and radius 0.3 */
    uint32_t     i, j, k;
    int          m;

    /* 1. One bump (eq. 7). */
    {
        const double cell = 4.0 / 256.0;
        double       sum = 0.0, edge = 0.0;
        reset(s, 0.0);
        wp_emit(s, 127.5 * cell, 127.5 * cell, 0.0, 0.7, 0.0);
        wp_field(s, 256u, cell, field, NULL);
        for (i = 0u; i < 256u * 256u; ++i) sum += field[i];
        for (i = 0u; i < 256u; ++i) edge = fmax(edge, fabs(i * cell - 127.0 * cell) > 0.3 ? field[127u * 256u + i] : 0.0);
        demo_check(d, "one bump, amplitude 0.7: height at its centre", field[127u * 256u + 127u], "0.7", fabs(field[127u * 256u + 127u] - 0.7) < 1e-12);
        demo_check(d, "  height a radius away and beyond", edge, "0", edge == 0.0);
        demo_check(d, "  volume / (a r^2 (pi/2 - 2/pi)) - 1", sum * cell * cell / (0.7 * volume) - 1.0, "|.| < 1e-3", fabs(sum * cell * cell / (0.7 * volume) - 1.0) < 1e-3);
    }

    /* 2. Section 3.2: a straight front of particles half a radius apart, then a whole radius, then a sixteenth. */
    for (k = 0u; k < 3u; ++k) {
        static const char *const k_label[3] = {"front of particles half a radius apart: across the front, largest |height / peak - eq. 5| (paper: < 3 %)",
                                               "the same a whole radius apart: across the front (reported)", "the same a sixteenth of a radius apart: across the front (reported)"};
        const double   cell = 0.3 / 32.0, gap = k == 0u ? 0.15 : k == 1u ? 0.3 : 0.3 / 16.0;
        const uint32_t cells = k == 0u ? 16u : k == 1u ? 32u : 2u;
        double         peak, shape = 0.0, crest = 0.0;
        reset(s, 0.0);
        for (m = -(int)(3.0 / gap); m <= (int)(4.5 / gap); ++m) wp_emit(s, 127.5 * cell, 127.5 * cell + m * gap, 0.0, 1.0, 0.0);
        wp_field(s, 256u, cell, field, NULL);
        peak = field[127u * 256u + 127u];
        for (j = 127u; j <= 127u + cells; ++j) {
            crest = fmax(crest, fabs(field[j * 256u + 127u] / peak - 1.0));
            for (i = 0u; i < 256u; ++i) {
                const double u = ((double)i - 127.0) * cell, ideal = fabs(u) < 0.3 ? 0.5 * (cos(PI * u / 0.3) + 1.0) : 0.0;
                shape = fmax(shape, fabs(field[j * 256u + i] / peak - ideal));
            }
        }
        demo_check(d, k_label[k], shape, k ? "" : "< 0.08", k || shape < 0.08);
        demo_check(d, k ? "  along the crest (reported)" : "  along the crest, largest |height / peak - 1| (paper: < 0.1 %)", crest, k ? "" : "< 0.001", k || crest < 0.001);
        demo_check(d, "  crest height / (amplitude x radius / spacing) - 1", peak / (0.3 / gap) - 1.0, k ? "" : "|.| < 0.001", k || fabs(peak / (0.3 / gap) - 1.0) < 0.001);
    }

    /* 3. A ripple: one particle with a dispersion angle of 2 pi, 2 s later (3 m out). */
    {
        const double cell = POOL / 256.0;
        double       total = 0.0, widest = 0.0, low = INFINITY, high = 0.0, sum = 0.0, ring[2] = {0.0, 0.0};
        uint32_t     t;
        reset(s, 0.0);
        wp_emit(s, 4.0, 4.0, 0.3, 1.0, 2.0 * PI);
        for (t = 0u; t < 2u; ++t) {
            double best = 0.0;
            for (i = 0u; i < 60u; ++i) wp_advance(s, 1.0 / 60.0);
            wp_field(s, 256u, cell, field, NULL);
            /* where the crest is on the row through the middle, to the right of it */
            for (i = 128u; i < 256u; ++i)
                if (field[128u * 256u + i] > best) { best = field[128u * 256u + i]; ring[t] = (i + 0.5) * cell - 4.0; }
        }
        for (k = 0u; k < s->slots; ++k) { total += s->particle[k].amplitude; widest = fmax(widest, s->particle[k].spread * 3.0); }
        for (j = 0u; j < 256u; ++j)
            for (i = 0u; i < 256u; ++i) {
                const double x = (i + 0.5) * cell - 4.0, y = (j + 0.5) * cell - 4.0, v = field[j * 256u + i];
                sum += v;
                if (fabs(sqrt(x * x + y * y) - 3.0) < 0.5 * cell) { low = fmin(low, v); high = fmax(high, v); }
            }
        demo_check(d, "ripple, 3 m out: particles (3^5: the first count whose spacing is under half a radius)", s->alive, "243", s->alive == 243u);
        demo_check(d, "  widest spacing / radius", widest / 0.3, "<= 0.5", widest / 0.3 <= 0.5);
        demo_check(d, "  sum of amplitudes - the first particle's", total - 1.0, "|.| < 1e-12", fabs(total - 1.0) < 1e-12);
        demo_check(d, "  crest height round the ring, lowest / (a r / 2 pi distance)", low / (0.3 / (2.0 * PI * 3.0)), "0.97 .. 1.03", low / (0.3 / (2.0 * PI * 3.0)) > 0.97 && low / (0.3 / (2.0 * PI * 3.0)) < 1.03);
        demo_check(d, "  highest / the same", high / (0.3 / (2.0 * PI * 3.0)), "0.97 .. 1.03", high / (0.3 / (2.0 * PI * 3.0)) > 0.97 && high / (0.3 / (2.0 * PI * 3.0)) < 1.03);
        demo_check(d, "  volume / one bump's - 1", sum * cell * cell / volume - 1.0, "|.| < 1e-3", fabs(sum * cell * cell / volume - 1.0) < 1e-3);
        demo_check(d, "  crest's speed between 1 s and 2 s / wave speed", (ring[1] - ring[0]) / 1.5, "0.95 .. 1.05", (ring[1] - ring[0]) / 1.5 > 0.95 && (ring[1] - ring[0]) / 1.5 < 1.05);
        /* 6 (here because the state is right for it). The time table: 120 steps looked at this many particles. */
        demo_check(d, "  particles visited in 120 steps (one for each subdivision: (243 - 1) / 2)", (double)s->visited, "121", s->visited == 121u);

        /* 7. Section 5's image of the same ripple: points, then a filter. */
        {
            double exact_peak = 0.0, whole = 0.0, split = 0.0;
            wp_field_filtered(s, 256u, cell, false, second);
            for (i = 0u; i < 256u * 256u; ++i) { exact_peak = fmax(exact_peak, field[i]); whole = fmax(whole, fabs(second[i] - field[i])); }
            wp_field_filtered(s, 256u, cell, true, second);
            for (i = 0u; i < 256u * 256u; ++i) split = fmax(split, fabs(second[i] - field[i]));
            demo_check(d, "  as points and a filter with eq. 7 (a radius is 9.6 samples): largest difference / peak", whole / exact_peak, "< 0.15", whole / exact_peak < 0.15);
            demo_check(d, "  as points and two 1-D filters with eq. 5: largest difference / peak (reported)", split / exact_peak, "", true);
        }
    }

    /* 4. Three ripples in a pool with walls, 4 s: 240 steps against 7 steps. */
    {
        double   largest = 0.0, peak = 0.0;
        uint32_t t;
        for (t = 0u; t < 2u; ++t) {
            Wp *w = t ? &other : s;
            reset(w, POOL);
            wp_emit(w, 2.0, 3.0, 0.0, 1.0, 2.0 * PI); wp_emit(w, 6.5, 5.0, 1.0, -0.6, 2.0 * PI); wp_emit(w, 4.0, 7.2, 2.0, 0.8, 2.0 * PI);
            for (i = 0u; i < (t ? 7u : 240u); ++i) wp_advance(w, t ? 4.0 / 7.0 : 1.0 / 60.0);
            wp_field(w, GRID, POOL / GRID, t ? second : field, NULL);
        }
        for (i = 0u; i < GRID * GRID; ++i) { largest = fmax(largest, fabs(field[i] - second[i])); peak = fmax(peak, fabs(field[i])); }
        demo_check(d, "three ripples in a pool, 4 s, 240 steps and 7 steps: particles, 240 steps", s->alive, "", true);
        demo_check(d, "  particles, 7 steps", other.alive, "the same", other.alive == s->alive);
        demo_check(d, "  largest difference of the two height fields / peak", largest / peak, "< 1e-9", largest / peak < 1e-9);
    }

    /* 5. A wall: a ripple 1 m from the wall x = 0, against open water with a second ripple behind the wall. */
    {
        double   largest = 0.0, peak = 0.0, sum[2] = {0.0, 0.0};
        uint32_t t;
        for (t = 0u; t < 2u; ++t) {
            reset(s, POOL);
            wp_emit(s, 1.0, 4.0, 0.2, 1.0, 2.0 * PI);
            wp_advance(s, t ? 1.5 : 0.4);
            wp_field(s, GRID, POOL / GRID, field, NULL);
            for (i = 0u; i < GRID * GRID; ++i) sum[t] += field[i] * (POOL / GRID) * (POOL / GRID);
        }
        reset(&other, 0.0);
        wp_emit(&other, 1.0, 4.0, 0.2, 1.0, 2.0 * PI); wp_emit(&other, -1.0, 4.0, PI - 0.2, 1.0, 2.0 * PI);
        wp_advance(&other, 1.5);
        wp_field(&other, GRID, POOL / GRID, second, NULL);
        for (i = 0u; i < GRID * GRID; ++i) { largest = fmax(largest, fabs(field[i] - second[i])); peak = fmax(peak, fabs(field[i])); }
        demo_check(d, "ripple 1 m from a wall, after 1.5 s: largest difference from open water with its mirror image / peak", largest / peak, "< 1e-12", largest / peak < 1e-12);
        demo_check(d, "  water in the pool before the wave reaches the wall / one bump's volume - 1", sum[0] / volume - 1.0, "|.| < 2e-3", fabs(sum[0] / volume - 1.0) < 2e-3);
        demo_check(d, "  the same while it reflects", sum[1] / volume - 1.0, "|.| < 2e-3", fabs(sum[1] / volume - 1.0) < 2e-3);
    }

    /* 8. Removal: amplitude 0.05, threshold 1e-4. 0.05 / 3^6 is the first share below it, so the 729 of the
     * sixth generation go when they are due, 17.4 m out (11.6 s). */
    {
        reset(s, 0.0);
        s->threshold = 1e-4;
        wp_emit(s, 0.0, 0.0, 0.0, 0.05, 2.0 * PI);
        wp_advance(s, 11.5);
        demo_check(d, "ripple of amplitude 0.05, threshold 1e-4: particles after 11.5 s", s->alive, "729", s->alive == 729u);
        wp_advance(s, 0.2);
        demo_check(d, "  after 11.7 s", s->alive, "0", s->alive == 0u);
        demo_check(d, "  removed", (double)s->removed, "729", s->removed == 729u);
        /* a straight front never subdivides; damping 1 / s takes 0.01 below 1e-4 after ln 100 = 4.605 s */
        reset(s, 0.0);
        s->threshold = 1e-4; s->damping = 1.0;
        wp_emit(s, 0.0, 0.0, 0.0, 0.01, 0.0);
        wp_advance(s, 4.5);
        demo_check(d, "particle of a straight front, amplitude 0.01, damping 1 / s: alive after 4.5 s", s->alive, "1", s->alive == 1u);
        wp_advance(s, 0.2);
        demo_check(d, "  after 4.7 s", s->alive, "0", s->alive == 0u);
    }

    /* 9. Section 3.4: a train of crests and troughs a radius apart; height and horizontal deviation over one wavelength. */
    {
        const double cell = 0.3 / 32.0;
        double       round = 0.0, sideways = 0.0, wrong = 0.0, slope;
        int          front;
        reset(s, 0.0);
        for (front = -6; front <= 6; ++front)
            for (m = -10; m <= 30; ++m) wp_emit(s, 127.5 * cell + front * 0.3, 127.5 * cell + m * 0.15, 0.0, front % 2 ? -0.5 : 0.5, 0.0);
        wp_field(s, 256u, cell, field, shift);
        for (i = 95u; i < 159u; ++i) {
            const uint32_t at = 131u * 256u + i;
            const double   u = ((double)i - 127.0) * cell;
            round    = fmax(round, fabs(sqrt(field[at] * field[at] + shift[2u * at] * shift[2u * at]) - 1.0));
            sideways = fmax(sideways, fabs(shift[2u * at + 1u]));
            wrong    = fmax(wrong, fmax(fabs(field[at] - cos(PI * u / 0.3)), fabs(shift[2u * at] + sin(PI * u / 0.3))));
        }
        slope = (shift[2u * (131u * 256u + 128u)] - shift[2u * (131u * 256u + 126u)]) / (2.0 * cell);
        demo_check(d, "wave train of height 1: a point of the surface stays on a circle of radius 1; largest error", round, "< 0.15", round < 0.15);
        demo_check(d, "  largest |height - cos| or |horizontal + sin|", wrong, "< 0.15", wrong < 0.15);
        demo_check(d, "  largest deviation along the crests", sideways, "< 0.01", sideways < 0.01);
        demo_check(d, "  horizontal velocity at a crest / (a pi v / r): forwards, as in water", -slope / (PI / 0.3), "0.95 .. 1.05", -slope / (PI / 0.3) > 0.95 && -slope / (PI / 0.3) < 1.05);
    }
}

int main(int argc, char **argv) {
    static App app;
    static const DemoOption options[] = {
        {"--scene", .u32 = &app.scene, .lo = 0, .hi = 2, .help = "0 rain on a pool, 1 a fast source on open water, 2 straight waves at a wall"},
        {"--image", .u32 = &app.image, .lo = 0, .hi = 2, .help = "height field: 0 each bump as it is, 1 points and a filter, 2 points and two 1-D filters"},
        {"--damping", .f32 = &app.damping, .lo = 0, .hi = 2, .help = "amplitudes fall as exp(-this x age), 1/s"},
        {"--no-warp", .flag = &app.no_warp, .help = "height only, without the horizontal motion of section 3.4"},
        {"--dots", .flag = &app.dots, .help = "draw the particles"},
        {0}};
    Demo *d = &app.demo;
    app.damping = 0.15f;
    app.orbit = (DemoOrbit){.target = {0.0f, 0.0f, 0.0f}, .distance = 12.4f, .yaw = 0.5f, .pitch = 0.7f,
                            .fov_deg = 40.0f, .znear = 0.05f, .zfar = 60.0f, .min_distance = 1.0f, .max_distance = 30.0f};
    demo_defaults(d, "waveparticles", "Wave particles");
    demo_open(d, options, argc, argv);
    canvas_open(d, &app.canvas, 250000u, CANVAS_RGB(24, 28, 38));
    seed(&app);
    if (d->headless && !d->frames) d->frames = 300u;
    while (demo_running(d)) {
        if (d->ui) app_ui(&app, d->ui);
        app_frame(&app);
    }
    if (d->check) app_check(&app);
    return demo_close(d);
}
