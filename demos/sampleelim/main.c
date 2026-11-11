/* sampleelim — a Poisson disk sample set by eliminating samples from a random one.
 *
 *   Draw -> Present          (the method runs in C: method.c)
 *
 * Cem Yuksel: "Sample Elimination for Generating Poisson Disk Sample Sets" (Eurographics
 * 2015). Left: the samples, those still to be eliminated small. Right: the power spectrum of
 * the samples shown (white noise is flat grey; blue noise is dark round the centre).
 * Scene 0 eliminates from M to N, scene 1 does it with a density map (the paper's figure 12),
 * scene 2 orders the set so that every prefix is a sample set (section 5.1).
 *
 * --check replays the elimination against weights computed from scratch, measures the radius
 * ratios for five input sizes with and without weight limiting, compares with dart throwing,
 * and tests the spectrum, the ordering and the density map.
 */
#include "canvas.h"
#include "method.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MOST  40000   /* input samples */
#define REACH 32      /* the spectrum is drawn for |k| <= REACH */

typedef struct App {
    Demo     demo;   /* first: see demo.h */
    Canvas   canvas;
    Se       set;
    uint32_t scene, built, count, ratio, seed, made_count, made_ratio;   /* count: N; ratio: M / N */
    bool     no_limit, open, made_limit, made_open;
    uint32_t have, shown, drawn;   /* drawn: for how many samples `power` was computed */
    float    clock;
    double   p[2 * MOST], intensity[MOST], power[(2 * REACH + 1) * (2 * REACH + 1)];
    uint32_t index[MOST];
} App;

/* the density map of scene 1: dark (many samples) in the middle, bright at the rim */
static double map(double x, double y) { return fmin(2.2 * hypot(x - 0.5, y - 0.5), 1.0); }

static void scatter(double *p, uint32_t *index, uint32_t n, uint64_t *state) {
    uint32_t i;
    for (i = 0u; i < n; ++i) { p[2u * i] = se_random(state); p[2u * i + 1u] = se_random(state); index[i] = i; }
}

static void build(App *a) {
    uint64_t state = 0x9e3779b97f4a7c15ull + a->seed;
    Se       s = {{1.0, 1.0}, 8.0, a->no_limit ? 0.0 : 0.65, 1.5, !a->open};
    uint32_t i;
    a->have = a->count * a->ratio;
    scatter(a->p, a->index, a->have, &state);
    for (i = 0u; i < a->have; ++i) a->intensity[i] = map(a->p[2u * i], a->p[2u * i + 1u]);
    if (a->scene == 1u) s.beta = 0.0;   /* as for the paper's figure 12 */
    if (a->scene == 0u) se_eliminate(&s, a->p, a->index, a->have, a->count, 0, 0);
    if (a->scene == 1u) se_eliminate(&s, a->p, a->index, a->have, a->count, a->intensity, 0);
    if (a->scene == 2u) se_order(&s, a->p, a->index, a->have, a->count);
    a->set = s; a->built = a->scene; a->made_count = a->count; a->made_ratio = a->ratio; a->made_limit = a->no_limit; a->made_open = a->open;
    a->clock = 0.0f; a->drawn = 0u;
}

static void app_ui(App *a, struct nk_context *ctx) {
    static const char *const k_scenes[3] = {"Elimination", "With a density map", "Ordered for progressive use"};
    float  count = (float)a->count, ratio = (float)a->ratio, mean;
    bool   limit = !a->no_limit, tiled = !a->open;
    double r;
    if (nk_begin(ctx, "Sample elimination", nk_rect(16, 16, 320, 290), NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_TITLE)) {
        demo_ui_choice(ctx, "Scene", k_scenes, 3u, &a->scene);
        demo_ui_float(ctx, "Samples kept (N)", &count, 64.0f, 2000.0f);
        demo_ui_float(ctx, "Input size (M / N)", &ratio, 1.0f, 8.0f);
        demo_ui_bool(ctx, "Weight limiting (eq. 4)", &limit);
        demo_ui_bool(ctx, "Distances wrap round", &tiled);
        r = se_radius(&a->set, a->p, a->index, a->shown < 2u ? 2u : a->shown, &(double){0});
        mean = (float)(r / se_rmax(&a->set, a->shown < 2u ? 2u : a->shown));
        demo_ui_text(ctx, "%u of %u samples shown", a->shown, a->have);
        demo_ui_text(ctx, "radius / largest possible: %.3f", mean);
    }
    nk_end(ctx);
    a->count = (uint32_t)(count + 0.5f); a->ratio = (uint32_t)(ratio + 0.5f); a->no_limit = !limit; a->open = !tiled;
}

static void app_frame(App *a) {
    Demo       *d = &a->demo;
    Canvas     *c = &a->canvas;
    const float corner[4][3] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    const int   side = 2 * REACH + 1;
    const float cell = 1.0f / (float)side;
    uint32_t    i, stage;
    int         kx, ky;

    if (a->built != a->scene || a->made_count != a->count || a->made_ratio != a->ratio || a->made_limit != a->no_limit || a->made_open != a->open) build(a);
    /* a new stage every quarter of a second: 8 stages from M down to N, or the halves of scene 2 */
    a->clock += 1.0f / 60.0f;
    stage = (uint32_t)(a->clock * 4.0f);
    a->shown = (uint32_t)((double)a->have * pow((double)a->count / a->have, fmin(stage / 8.0, 1.0)) + 0.5);
    if (a->scene == 2u) a->shown = a->count >> (stage / 4u % 6u);
    if (a->drawn != a->shown) { se_spectrum(a->p, a->index, a->shown, REACH, a->power); a->drawn = a->shown; }

    /* both squares, 2.2 wide and 1 high, with a margin, whatever the shape of the window */
    canvas_view_flat(d, c, 1.1f, 0.5f, fmaxf(0.56f, 1.16f * (float)d->height / (float)d->width));
    for (i = 0u; i < 4u; ++i) canvas_line(c, corner[i], corner[(i + 1u) % 4u], 0.004f, CANVAS_RGB(120, 126, 140));
    for (i = 0u; i < a->shown; ++i) {
        const float x = (float)a->p[2u * a->index[i]], y = (float)a->p[2u * a->index[i] + 1u], at[3] = {x, y, 0.1f};
        const float    r = i < a->count ? 0.3f * (float)se_rmax(&a->set, a->count) : 0.0035f;
        const float    q[4][3] = {{x - r, y, 0.05f}, {x, y - r, 0.05f}, {x + r, y, 0.05f}, {x, y + r, 0.05f}};
        const uint32_t colour = i < a->count ? CANVAS_RGB(240, 200, 90) : CANVAS_RGB(90, 110, 150);
        /* round dots cost 36 vertices each: only for a set of up to 1000 */
        if (i < a->count && a->count <= 1000u) { canvas_dot(c, at, r, colour); continue; }
        canvas_triangle(c, q[0], q[1], q[2], colour);
        canvas_triangle(c, q[0], q[2], q[3], colour);
    }
    for (ky = 0; ky < side; ++ky)
        for (kx = 0; kx < side; ++kx) {
            const double value = kx == REACH && ky == REACH ? 0.0 : a->power[ky * side + kx];
            const int    grey = (int)(255.0 * fmin(value / 2.0, 1.0));
            const float  x = 1.2f + (float)kx * cell, y = (float)ky * cell;
            const float  q[4][3] = {{x, y, 0}, {x + cell, y, 0}, {x + cell, y + cell, 0}, {x, y + cell, 0}};
            canvas_triangle(c, q[0], q[1], q[2], CANVAS_RGB(grey, grey, grey));
            canvas_triangle(c, q[0], q[2], q[3], CANVAS_RGB(grey, grey, grey));
        }
    canvas_frame(d, c);
}

/* eq. 1 with eq. 4 and eq. 5, written again for the check */
static double plain_share(const Se *s, const double *a, const double *b, uint32_t have, uint32_t want) {
    const double rmax = sqrt(s->size[0] * s->size[1] / (2.0 * sqrt(3.0) * want)), rmin = rmax * (1.0 - pow((double)want / have, s->gamma)) * s->beta;
    double       dx = a[0] - b[0], dy = a[1] - b[1], d;
    if (s->tiled) { dx -= s->size[0] * floor(dx / s->size[0] + 0.5); dy -= s->size[1] * floor(dy / s->size[1] + 0.5); }
    d = sqrt(dx * dx + dy * dy);
    d = d > 2.0 * rmin ? fmin(d, 2.0 * rmax) : 2.0 * rmin;
    return pow(1.0 - d / (2.0 * rmax), s->alpha);
}

static void app_check(App *a) {
    Demo     *d = &a->demo;
    Se        s = {{1.0, 1.0}, 8.0, 0.65, 1.5, true};
    uint64_t  state = 2015u;
    double   *p = a->p, mean, r;
    uint32_t *index = a->index, i, j, n, t, m;
    char      label[160];

    /* 1: every sample eliminated had the largest weight when it went */
    for (m = 0u; m < 2u; ++m) {
        static double weight[50];
        double        worst = 0.0, final = 0.0;
        s.tiled = m == 0u; s.size[0] = m ? 1.7 : 1.0;
        for (i = 0u; i < 200u; ++i) { p[2u * i] = s.size[0] * se_random(&state); p[2u * i + 1u] = se_random(&state); index[i] = i; }
        se_eliminate(&s, p, index, 200u, 50u, 0, weight);
        for (n = 200u; n >= 50u; --n) {
            /* weights of index[0 .. n) from scratch; the next to go is index[n - 1] */
            double largest = -1.0, last = 0.0;
            for (i = 0u; i < n; ++i) {
                double w = 0.0;
                for (j = 0u; j < n; ++j)
                    if (j != i) w += plain_share(&s, p + 2u * index[i], p + 2u * index[j], 200u, 50u);
                largest = fmax(largest, w);
                last    = w;
                if (n == 50u) final = fmax(final, fabs(w - weight[i]));
            }
            if (n > 50u) worst = fmax(worst, largest - last);
        }
        snprintf(label, sizeof label, "200 to 50 samples, %s: largest (top weight - weight of the one eliminated), 150 steps", m ? "1.7 x 1 with edges" : "1 x 1 wrapped");
        demo_check(d, label, worst, "< 1e-9", worst < 1e-9);
        demo_check(d, "  weights kept through the elimination against weights from scratch", final, "< 1e-9", final < 1e-9);
    }
    s.tiled = true; s.size[0] = 1.0;

    /* 2: radius ratios of 1000 samples for input sizes 1 .. 5, means of 8 sets */
    {
        double rho[2][5] = {{0}}, hat[2][5] = {{0}};
        for (m = 0u; m < 2u; ++m)
            for (n = 0u; n < 5u; ++n) {
                s.beta = m ? 0.65 : 0.0;
                for (t = 0u; t < 8u; ++t) {
                    scatter(p, index, 1000u * (n + 1u), &state);
                    se_eliminate(&s, p, index, 1000u * (n + 1u), 1000u, 0, 0);
                    rho[m][n] += se_radius(&s, p, index, 1000u, &mean) / se_rmax(&s, 1000u) / 8.0;
                    hat[m][n] += mean / se_rmax(&s, 1000u) / 8.0;
                }
                snprintf(label, sizeof label, "1000 from %u, beta %.2f: radius / r_max", 1000u * (n + 1u), s.beta);
                demo_check(d, label, rho[m][n], n == 0u ? "< 0.1" : n == 1u ? "> 0.4" : "> with a smaller input", n == 0u ? rho[m][n] < 0.1 : n == 1u ? rho[m][n] > 0.4 : rho[m][n] > rho[m][n - 1u]);
                demo_check(d, "  mean radius / r_max", hat[m][n], n == 0u ? "0.4654 +- 0.02 (random points)" : "> with a smaller input", n == 0u ? fabs(hat[m][n] - 0.4654) < 0.02 : hat[m][n] > hat[m][n - 1u]);
            }
        demo_check(d, "weight limiting: gain in radius / r_max at M/N 2", rho[1][1] - rho[0][1], "info", true);
        demo_check(d, "  at M/N 3", rho[1][2] - rho[0][2], "> 0.01", rho[1][2] - rho[0][2] > 0.01);
        demo_check(d, "  at M/N 5", rho[1][4] - rho[0][4], "> 0.03", rho[1][4] - rho[0][4] > 0.03);
        demo_check(d, "  without it: gain from M/N 4 to 5", rho[0][4] - rho[0][3], "< 0.02", rho[0][4] - rho[0][3] < 0.02);
        demo_check(d, "  with it: gain from M/N 4 to 5", rho[1][4] - rho[1][3], "> 0.01", rho[1][4] - rho[1][3] > 0.01);
    }

    /* 3: dart throwing with the radius that elimination reached (M/N 4), 8 sets */
    {
        double   ours = 0.0, theirs = 0.0, radius = 0.0;
        uint32_t failed = 0u, done = 0u;
        for (t = 0u; t < 8u; ++t) {
            scatter(p, index, 4000u, &state);
            se_eliminate(&s, p, index, 4000u, 1000u, 0, 0);
            r = se_radius(&s, p, index, 1000u, &mean);
            n = se_darts(&s, r, 1000u, 4000000u, &state, p + 2u * 4000u);
            if (n < 1000u) { failed++; continue; }
            for (i = 0u; i < 1000u; ++i) index[4000u + i] = 4000u + i;
            done++; ours += mean; radius += r;
            se_radius(&s, p, index + 4000u, 1000u, &mean);
            theirs += mean;
        }
        demo_check(d, "dart throwing with elimination's radius: sets of 8 that did not reach 1000 samples in 4 million darts", failed, "< 8", failed < 8u);
        demo_check(d, "  radius / r_max of both (mean)", radius / done / se_rmax(&s, 1000u), "info", true);
        demo_check(d, "  mean radius / r_max, elimination", ours / done / se_rmax(&s, 1000u), "info", true);
        demo_check(d, "  mean radius / r_max, dart throwing", theirs / done / se_rmax(&s, 1000u), "info", true);
        demo_check(d, "  dart throwing's mean radius / elimination's", theirs / ours, "< 1", theirs < ours);
    }

    /* 4: the spectrum of 1000 samples from 5000 */
    {
        static double power[97 * 97];
        double        low[2] = {0.0, 0.0}, ring[49] = {0}, peak = 0.0;
        uint32_t      cells = 0u, in_ring[49] = {0}, where = 0u;
        int           kx, ky;
        scatter(p, index, 5000u, &state);
        for (m = 0u; m < 2u; ++m) {
            se_spectrum(p, index, m ? 1000u : 5000u, 48, power);
            for (ky = -10; ky <= 10; ++ky)
                for (kx = -10; kx <= 10; ++kx)
                    if (kx * kx + ky * ky >= 4 && kx * kx + ky * ky <= 100) { low[m] += power[(ky + 48) * 97 + kx + 48]; cells += m; }
            if (m == 0u) se_eliminate(&s, p, index, 5000u, 1000u, 0, 0);
        }
        for (ky = -48; ky <= 48; ++ky)
            for (kx = -48; kx <= 48; ++kx) {
                const uint32_t k = (uint32_t)(sqrt((double)(kx * kx + ky * ky)) + 0.5);
                if (k > 48u) continue;
                ring[k] += power[(ky + 48) * 97 + kx + 48]; in_ring[k]++;
            }
        for (i = 1u; i <= 48u; ++i)
            if (ring[i] / in_ring[i] > peak) { peak = ring[i] / in_ring[i]; where = i; }
        demo_check(d, "spectrum, 5000 random samples: mean power for 2 <= |k| <= 10", low[0] / cells, "0.8 .. 1.2 (white noise: 1)", low[0] / cells > 0.8 && low[0] / cells < 1.2);
        demo_check(d, "  the 1000 kept: mean power for 2 <= |k| <= 10", low[1] / cells, "< 0.15", low[1] / cells < 0.15);
        demo_check(d, "  largest ring average of the power", peak, "> 1.5", peak > 1.5);
        demo_check(d, "  at |k| =", where, "30 .. 38 (1 / spacing of a hexagonal packing: 34)", where >= 30u && where <= 38u);
    }

    /* 5: the order of section 5.1 */
    {
        double worst = 1.0, plain_worst = 0.0;
        scatter(p, index, 5120u, &state);
        se_order(&s, p, index, 5120u, 1024u);
        for (n = 512u; n >= 64u; n /= 2u) {
            r = se_radius(&s, p, index, n, &mean) / se_rmax(&s, n);
            snprintf(label, sizeof label, "1024 from 5120, ordered: radius / r_max of the first %u", n);
            demo_check(d, label, r, "info", true);
            worst = fmin(worst, r);
        }
        scatter(p, index, 5120u, &state);
        se_eliminate(&s, p, index, 5120u, 1024u, 0, 0);
        for (n = 512u; n >= 64u; n /= 2u) {
            r = se_radius(&s, p, index, n, &mean) / se_rmax(&s, n);
            snprintf(label, sizeof label, "  not ordered: radius / r_max of the first %u", n);
            demo_check(d, label, r, "info", true);
            plain_worst = fmax(plain_worst, r);
        }
        demo_check(d, "ordered: smallest of those four", worst, "> 0.55", worst > 0.55);
        demo_check(d, "  not ordered: largest of those four", plain_worst, "< 0.52", plain_worst < 0.52);
    }

    /* 6: a density map that is 0 on the left half and 1 on the right, beta 0 */
    {
        double   near[2] = {0.0, 0.0};
        uint32_t side[2] = {0u, 0u}, inner[2] = {0u, 0u};
        s.beta = 0.0;
        scatter(p, index, 5000u, &state);
        for (i = 0u; i < 5000u; ++i) a->intensity[i] = p[2u * i] < 0.5 ? 0.0 : 1.0;
        se_eliminate(&s, p, index, 5000u, 1000u, a->intensity, 0);
        for (i = 0u; i < 1000u; ++i) {
            const double *x = p + 2u * index[i];
            double        nearest = 1e300;
            m = x[0] < 0.5 ? 0u : 1u;
            side[m]++;
            if (fabs(x[0] - 0.25 - 0.5 * m) > 0.15) continue;   /* away from the two borders between the halves */
            for (j = 0u; j < 1000u; ++j)
                if (j != i) nearest = fmin(nearest, hypot(x[0] - p[2u * index[j]], fmin(fabs(x[1] - p[2u * index[j] + 1u]), 1.0 - fabs(x[1] - p[2u * index[j] + 1u]))));
            near[m] += nearest; inner[m]++;
        }
        demo_check(d, "density map 0 | 1, 1000 from 5000: samples in the half with 0", side[0], "info", true);
        demo_check(d, "  samples in the half with 1", side[1], "info", true);
        demo_check(d, "  mean distance to the nearest sample, half with 1 / half with 0", (near[1] / inner[1]) / (near[0] / inner[0]), "2 .. 3.5 (d^ = 3 d against d)", (near[1] / inner[1]) / (near[0] / inner[0]) > 2.0 && (near[1] / inner[1]) / (near[0] / inner[0]) < 3.5);
    }
    a->built = ~0u;
}

int main(int argc, char **argv) {
    static App app;
    static const DemoOption options[] = {
        {"--scene", .u32 = &app.scene, .lo = 0, .hi = 2, .help = "0 elimination, 1 with a density map, 2 ordered for progressive use"},
        {"--count", .u32 = &app.count, .lo = 64, .hi = 2000, .help = "samples kept (N)"},
        {"--ratio", .u32 = &app.ratio, .lo = 1, .hi = 8, .help = "input samples for each one kept (M / N)"},
        {"--seed", .u32 = &app.seed, .lo = 0, .hi = 1000000, .help = "of the random input"},
        {"--no-limit", .flag = &app.no_limit, .help = "beta 0: without the weight limiting of eq. 4"},
        {"--open", .flag = &app.open, .help = "distances do not wrap round the square"},
        {0}};
    Demo *d = &app.demo;
    app.count = 400u; app.ratio = 5u; app.built = ~0u;
    demo_defaults(d, "sampleelim", "Sample elimination");
    demo_open(d, options, argc, argv);
    canvas_open(d, &app.canvas, 160000u, CANVAS_RGB(24, 27, 36));
    if (d->headless && !d->frames) d->frames = 150u;
    while (demo_running(d)) {
        if (d->ui) app_ui(&app, d->ui);
        app_frame(&app);
    }
    if (d->check) app_check(&app);
    return demo_close(d);
}
