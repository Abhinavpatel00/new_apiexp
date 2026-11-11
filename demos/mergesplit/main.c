/* mergesplit — collisions by merging particles for a time step and splitting them again.
 *
 *   Draw -> Present          (the method runs in C: method.c)
 *
 * Truong, Yuksel, Watcharopas, Levine, Kirby: "Particle Merging-and-Splitting"
 * (IEEE TVCG 2021). Scene 0 is the paper's figure 3 in cross-section: a ball
 * of particles falls on an elastic sheet. Scene 1 pours loose particles on
 * the sheet, scene 2 is a box of discs that only collide.
 *
 * --check tests the split against the textbook elastic collision, eq. 8, 9
 * and 14 for arbitrary meta-particle velocities, two orders of merging, a row
 * of discs, energy in the box, the ball on the sheet for three methods and
 * three mass ratios, a particle at rest on others, and the limit on the size
 * of a meta-particle.
 */
#include "canvas.h"
#include "method.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SHEET 8000.0   /* N/m, each spring of the sheet */

typedef struct App {
    Demo     demo;   /* first: see demo.h */
    Canvas   canvas;
    Ms       world;
    uint32_t scene, built, method, limit, sheet;   /* sheet: how many particles it has; they come first */
    float    alpha, penalty;                       /* penalty: log10 N/m */
    double   energy0;
    bool     restart;
} App;

static void empty(Ms *s) {
    s->particles = s->springs = 0u;
    s->merges = s->second_stages = s->without_root = 0u; s->largest = 1u;
    s->dt = 1.0 / 1200.0; s->gravity = 9.81; s->box[0] = 6.1; s->box[1] = 4.5;
    s->alpha = 1.0; s->spring_damping = 0.0; s->penalty = 2000.0; s->limit = 64u; s->method = MS_MERGE;
}

/* The sheet of figure 3 seen from the side: 61 particles in a row from wall to wall, the two ends held. */
static void sheet(Ms *s, double stiffness) {
    uint32_t k;
    for (k = 0u; k < 61u; ++k) {
        ms_add(s, 0.05 + 0.1 * k, 2.2, 0.05, 0.05);
        if (k) ms_link(s, k - 1u, k, stiffness);
    }
    s->particle[0].pinned = s->particle[60].pinned = true;
}

/* How many particles after the sheet's 61 are under it: a line straight up from one crosses the sheet an
 * odd number of times. */
static uint32_t under(const Ms *s) {
    uint32_t count = 0u, k, j;
    for (k = 61u; k < s->particles; ++k) {
        const MsParticle *p = &s->particle[k];
        uint32_t          crossings = 0u;
        for (j = 1u; j < 61u; ++j) {
            const MsParticle *a = &s->particle[j - 1u], *b = &s->particle[j];
            if ((a->x[0] <= p->x[0]) != (b->x[0] <= p->x[0])) crossings += a->x[1] + (p->x[0] - a->x[0]) / (b->x[0] - a->x[0]) * (b->x[1] - a->x[1]) > p->x[1];
        }
        count += crossings & 1u;
    }
    return count;
}

/* A disc of 37 particles 0.1 apart in a triangular pattern, each joined to its neighbours by a spring
 * whose stiffness goes with the mass. */
static void ball(Ms *s, double mass) {
    const double   cx = 3.05, cy = 3.6;
    const int      rings = 3;
    const uint32_t first = s->particles;
    uint32_t       a, b;
    int            i, j;
    for (j = -rings; j <= rings; ++j)
        for (i = -rings - 1; i <= rings; ++i) {
            const double x = 0.1 * (i + (j & 1 ? 0.5 : 0.0)), y = 0.0866025 * j;
            if (x * x + y * y <= 0.01 * rings * rings + 1e-9) ms_add(s, cx + x, cy + y, mass, 0.05);
        }
    for (a = first; a < s->particles; ++a)
        for (b = a + 1u; b < s->particles; ++b) {
            const double dx = s->particle[b].x[0] - s->particle[a].x[0], dy = s->particle[b].x[1] - s->particle[a].x[1];
            if (dx * dx + dy * dy < 0.0101) ms_link(s, a, b, 400000.0 * mass);
        }
}

static double noise(uint32_t *state) {
    *state = *state * 1664525u + 1013904223u;
    return (double)(*state >> 8) / 16777216.0;
}

/* Scene 2: 150 discs of different sizes and weights in a box, no gravity. */
static void gas(Ms *s) {
    uint32_t rng = 9u, k;
    s->gravity = 0.0;
    for (k = 0u; k < 150u; ++k) {
        const double   radius = 0.05 + 0.07 * noise(&rng);
        const uint32_t index = ms_add(s, 0.3 + 0.39 * (k % 15u), 0.3 + 0.43 * (k / 15u), 40.0 * radius * radius, radius);
        s->particle[index].v[0] = 4.0 * noise(&rng) - 2.0;
        s->particle[index].v[1] = 4.0 * noise(&rng) - 2.0;
    }
}

static void seed(App *a) {
    Ms      *s = &a->world;
    uint32_t i, j;
    empty(s);
    a->sheet = a->scene == 2u ? 0u : 61u;
    if (a->scene < 2u) { sheet(s, SHEET); s->spring_damping = 0.02; }
    if (a->scene == 0u) ball(s, 0.05);
    for (j = 0u; j < 24u && a->scene == 1u; ++j)
        for (i = 0u; i < 8u; ++i) s->particle[ms_add(s, 2.655 + 0.11 * i + 0.02 * (j & 1u), 2.6 + 0.11 * j, 0.05, 0.05)].v[1] = -3.0;
    if (a->scene == 2u) gas(s);
    a->built = a->scene;
    a->energy0 = ms_energy(s);
}

static void app_ui(App *a, struct nk_context *ctx) {
    static const char *const k_scenes[3] = {"A ball on a sheet", "Loose particles on a sheet", "A box of discs"};
    static const char *const k_methods[3] = {"Merging-and-splitting", "Penalty force", "Impulses"};
    Ms *s = &a->world;
    if (nk_begin(ctx, "Merging-and-splitting", nk_rect(16, 16, 330, 275), NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_TITLE)) {
        if (demo_ui_choice(ctx, "Scene", k_scenes, 3u, &a->scene)) a->restart = true;
        if (demo_ui_choice(ctx, "Collisions", k_methods, 3u, &a->method)) a->restart = true;
        demo_ui_float(ctx, "Energy given back (alpha)", &a->alpha, 0.0f, 1.0f);
        demo_ui_float(ctx, "log10 penalty, N/m", &a->penalty, 2.0f, 6.0f);
        demo_ui_text(ctx, "energy / at the start: %.4f", ms_energy(s) / a->energy0);
        demo_ui_text(ctx, "%llu merges, %llu steps done twice", (unsigned long long)s->merges, (unsigned long long)s->second_stages);
        demo_ui_text(ctx, "largest meta-particle so far: %u of at most %u", s->largest, s->limit);
        demo_ui_text(ctx, "%llu splits needed a sideways term", (unsigned long long)s->without_root);
        nk_layout_row_dynamic(ctx, 24, 1);
        if (nk_button_label(ctx, "Restart")) a->restart = true;
    }
    nk_end(ctx);
}

static void app_frame(App *a) {
    Demo    *d = &a->demo;
    Canvas  *c = &a->canvas;
    Ms      *s = &a->world;
    uint32_t i;

    if (a->restart || a->built != a->scene) { seed(a); a->restart = false; }
    s->method = (MsMethod)a->method; s->alpha = a->alpha; s->penalty = pow(10.0, a->penalty); s->limit = a->limit;
    for (i = 0u; i < 20u; ++i) ms_step(s);   /* 20 steps of 1/1200 s a frame */

    canvas_view_flat(d, c, 3.05f, 2.25f, 2.45f);
    {
        const float corner[4][3] = {{0, 0, 0}, {6.1f, 0, 0}, {6.1f, 4.5f, 0}, {0, 4.5f, 0}};
        for (i = 0u; i < 4u; ++i) canvas_line(c, corner[i], corner[(i + 1u) % 4u], 0.02f, CANVAS_RGB(120, 126, 140));
    }
    for (i = 0u; i < s->springs; ++i) {
        const MsParticle *p = &s->particle[s->spring[i].a], *q = &s->particle[s->spring[i].b];
        const float       from[3] = {(float)p->x[0], (float)p->x[1], 0.1f}, to[3] = {(float)q->x[0], (float)q->x[1], 0.1f};
        canvas_line(c, from, to, 0.012f, CANVAS_RGB(80, 86, 100));
    }
    for (i = 0u; i < s->particles; ++i) {
        const MsParticle *p = &s->particle[i];
        const float       at[3] = {(float)p->x[0], (float)p->x[1], 0.2f};
        canvas_dot(c, at, (float)p->radius, p->pinned ? CANVAS_RGB(150, 150, 150) : i < a->sheet ? CANVAS_RGB(235, 150, 60) : a->scene == 1u ? CANVAS_RGB(90, 190, 130) : CANVAS_RGB(90, 150, 225));
    }
    canvas_frame(d, c);
}

static void app_check(App *a) {
    Demo    *d = &a->demo;
    Ms      *s = &a->world;
    uint32_t rng = 21u, trial, i, k;

    /* 1. Two free particles: merge, nothing happens to the meta-particle, split. */
    {
        double classic = 0.0, momentum = 0.0, energy = 0.0, partial = 0.0, dead = 0.0;
        for (trial = 0u; trial < 500u; ++trial) {
            const double m1 = 0.2 + 3.0 * noise(&rng), m2 = 0.2 + 3.0 * noise(&rng), angle = 6.2831853 * noise(&rng), n[2] = {cos(angle), sin(angle)};
            double       v1[2] = {4.0 * noise(&rng) - 2.0, 4.0 * noise(&rng) - 2.0}, v2[2] = {4.0 * noise(&rng) - 2.0, 4.0 * noise(&rng) - 2.0}, v12[2], o1[2], o2[2], bond, closing, before, after;
            /* make them approach along n */
            if (n[0] * (v2[0] - v1[0]) + n[1] * (v2[1] - v1[1]) > 0.0) { v1[0] = -v1[0]; v1[1] = -v1[1]; v2[0] = -v2[0]; v2[1] = -v2[1]; }
            closing = n[0] * (v2[0] - v1[0]) + n[1] * (v2[1] - v1[1]);
            before  = 0.5 * m1 * (v1[0] * v1[0] + v1[1] * v1[1]) + 0.5 * m2 * (v2[0] * v2[0] + v2[1] * v2[1]);
            bond    = ms_merge(m1, v1, m2, v2, v12);
            ms_split(m1, v1, m2, n, bond, v12, o1, o2);
            for (k = 0u; k < 2u; ++k) {
                /* the textbook elastic collision: an impulse along n that turns the closing speed round */
                classic  = fmax(classic, fabs(o1[k] - (v1[k] + 2.0 * m2 / (m1 + m2) * closing * n[k])));
                classic  = fmax(classic, fabs(o2[k] - (v2[k] - 2.0 * m1 / (m1 + m2) * closing * n[k])));
                momentum = fmax(momentum, fabs(m1 * o1[k] + m2 * o2[k] - m1 * v1[k] - m2 * v2[k]));
            }
            after  = 0.5 * m1 * (o1[0] * o1[0] + o1[1] * o1[1]) + 0.5 * m2 * (o2[0] * o2[0] + o2[1] * o2[1]);
            energy = fmax(energy, fabs(after - before));
            /* alpha 0.25 gives back a quarter of the bond; alpha 0 leaves no closing or parting speed */
            ms_split(m1, v1, m2, n, 0.25 * bond, v12, o1, o2);
            after   = 0.5 * m1 * (o1[0] * o1[0] + o1[1] * o1[1]) + 0.5 * m2 * (o2[0] * o2[0] + o2[1] * o2[1]);
            partial = fmax(partial, fabs(after - (before - 0.75 * bond)));
            ms_split(m1, v1, m2, n, 0.0, v12, o1, o2);
            dead = fmax(dead, fabs(n[0] * (o2[0] - o1[0]) + n[1] * (o2[1] - o1[1])));
        }
        demo_check(d, "500 free pairs, alpha 1: largest difference from the textbook elastic collision, m/s", classic, "< 1e-12", classic < 1e-12);
        demo_check(d, "  change of momentum", momentum, "< 1e-12", momentum < 1e-12);
        demo_check(d, "  change of kinetic energy", energy, "< 1e-12", energy < 1e-12);
        demo_check(d, "  alpha 0.25: energy after - (before - 3/4 of the bond)", partial, "< 1e-12", partial < 1e-12);
        demo_check(d, "  alpha 0: parting speed along n", dead, "< 1e-7", dead < 1e-7);
    }

    /* 2. The meta-particle's velocity after the step can be anything: eq. 8, 9, 14 must still hold. */
    {
        double   momentum = 0.0, energy = 0.0, closing = INFINITY, sideways = 0.0;
        uint32_t without = 0u;
        for (trial = 0u; trial < 2000u; ++trial) {
            const double m1 = 0.2 + 3.0 * noise(&rng), m2 = 0.2 + 3.0 * noise(&rng), angle = 6.2831853 * noise(&rng), n[2] = {cos(angle), sin(angle)};
            const double v1[2] = {4.0 * noise(&rng) - 2.0, 4.0 * noise(&rng) - 2.0}, v2[2] = {4.0 * noise(&rng) - 2.0, 4.0 * noise(&rng) - 2.0};
            const double moved[2] = {6.0 * noise(&rng) - 3.0, 6.0 * noise(&rng) - 3.0};
            double       v12[2], o1[2], o2[2];
            const double bond = ms_merge(m1, v1, m2, v2, v12);
            const bool   root = ms_split(m1, v1, m2, n, bond, moved, o1, o2);
            for (k = 0u; k < 2u; ++k) momentum = fmax(momentum, fabs(m1 * o1[k] + m2 * o2[k] - (m1 + m2) * moved[k]));
            energy  = fmax(energy, fabs(0.5 * m1 * (o1[0] * o1[0] + o1[1] * o1[1]) + 0.5 * m2 * (o2[0] * o2[0] + o2[1] * o2[1]) - bond - 0.5 * (m1 + m2) * (moved[0] * moved[0] + moved[1] * moved[1])));
            closing = fmin(closing, n[0] * (o2[0] - o1[0]) + n[1] * (o2[1] - o1[1]));
            without += !root;
            if (!root) sideways = fmax(sideways, fabs(n[0] * (o1[0] - moved[0]) + n[1] * (o1[1] - moved[1])));
        }
        demo_check(d, "2000 splits with an arbitrary meta-particle velocity: eq. 8 (momentum), largest error", momentum, "< 1e-11", momentum < 1e-11);
        demo_check(d, "  eq. 9 (energy with the bond), largest error", energy, "< 1e-11", energy < 1e-11);
        demo_check(d, "  eq. 14: smallest parting speed along n", closing, ">= -1e-12", closing >= -1e-12);
        demo_check(d, "  splits where eq. 13 had no root (reported)", without, "", true);
        demo_check(d, "  in those: (v1' - v12') . n", sideways, "< 1e-12", sideways < 1e-12);
    }

    /* 3. Three discs, the middle one touching both, all closing in: merged as (0 1) 2 and as (2 1) 0. */
    {
        double   out[2][3][2], difference = 0.0, energy[2] = {0.0, 0.0}, before = 0.0, momentum = 0.0, sum[2];
        uint32_t order;
        for (order = 0u; order < 2u; ++order) {
            static const double k_place[3][4] = {{-0.095, 0.0, 1.0, 0.2}, {0.0, 0.0, 0.0, 0.0}, {0.06, 0.075, -0.7, -0.9}};
            empty(s);
            s->gravity = 0.0; s->box[0] = s->box[1] = 100.0;
            for (i = 0u; i < 3u; ++i) {
                const double *p = k_place[order ? 2u - i : i];
                const uint32_t index = ms_add(s, 50.0 + p[0], 50.0 + p[1], order ? 1.0 + (2u - i) : 1.0 + i, 0.05);
                s->particle[index].v[0] = p[2]; s->particle[index].v[1] = p[3];
            }
            before = ms_energy(s);
            for (k = 0u; k < 2u; ++k) sum[k] = s->particle[0].mass * s->particle[0].v[k] + s->particle[1].mass * s->particle[1].v[k] + s->particle[2].mass * s->particle[2].v[k];
            ms_step(s);
            for (k = 0u; k < 2u; ++k) momentum = fmax(momentum, fabs(s->particle[0].mass * s->particle[0].v[k] + s->particle[1].mass * s->particle[1].v[k] + s->particle[2].mass * s->particle[2].v[k] - sum[k]));
            energy[order] = ms_energy(s);
            for (i = 0u; i < 3u; ++i) memcpy(out[order][order ? 2u - i : i], s->particle[i].v, sizeof(double[2]));
        }
        for (i = 0u; i < 3u; ++i)
            for (k = 0u; k < 2u; ++k) difference = fmax(difference, fabs(out[0][i][k] - out[1][i][k]));
        demo_check(d, "three discs merged in two orders: merges in a step", (double)s->merges, "2", s->merges == 2u);
        demo_check(d, "  energy after / before - 1, first order", energy[0] / before - 1.0, "|.| < 1e-12", fabs(energy[0] / before - 1.0) < 1e-12);
        demo_check(d, "  second order", energy[1] / before - 1.0, "|.| < 1e-12", fabs(energy[1] / before - 1.0) < 1e-12);
        demo_check(d, "  largest change of momentum in either order", momentum, "< 1e-12", momentum < 1e-12);
        demo_check(d, "  largest difference of the final velocities between the two orders, m/s (reported)", difference, "", true);
    }

    /* 4. A row of five equal discs at rest, a sixth arrives at 1 m/s: with gaps of 1 cm between them, and
     * touching. Apart, the collisions happen one after another and the last disc leaves with everything.
     * Touching, all six merge in one step and the split does not know the answer of the row of balls. */
    for (trial = 0u; trial < 2u; ++trial) {
        const double step = trial ? 0.1 - 1e-6 : 0.11;
        double       slow = 0.0, momentum = 0.0, before;
        empty(s);
        s->gravity = 0.0; s->box[0] = s->box[1] = 100.0;
        for (i = 0u; i < 6u; ++i) ms_add(s, 50.0 + step * i - (i == 0u ? 0.05 : 0.0), 50.0, 1.0, 0.05);
        s->particle[0].v[0] = 1.0;
        before = ms_energy(s);
        for (i = 0u; i < 1200u; ++i) ms_step(s);
        for (i = 0u; i < 6u; ++i) momentum += s->particle[i].v[0];
        for (i = 0u; i < 5u; ++i) slow = fmax(slow, fabs(s->particle[i].v[0]));
        demo_check(d, trial ? "the same row with the five touching: the last one's speed (reported)" : "row of five discs 1 cm apart hit by a sixth at 1 m/s, 1 s later: the last one's speed",
                   s->particle[5].v[0], trial ? "" : "1 +- 1e-12", trial || fabs(s->particle[5].v[0] - 1.0) < 1e-12);
        demo_check(d, trial ? "  fastest of the others (reported)" : "  fastest of the others", slow, trial ? "" : "< 1e-12", trial || slow < 1e-12);
        demo_check(d, "  most particles in a meta-particle", (double)s->largest, trial ? "" : "2", trial || s->largest == 2u);
        demo_check(d, "  momentum after - before", momentum - 1.0, "|.| < 1e-12", fabs(momentum - 1.0) < 1e-12);
        demo_check(d, "  energy after / before - 1", ms_energy(s) / before - 1.0, "|.| < 1e-12", fabs(ms_energy(s) / before - 1.0) < 1e-12);
    }

    /* 5. The box of 150 discs, 5 s. */
    {
        double before, deepest = 0.0;
        empty(s);
        gas(s);
        before = ms_energy(s);
        for (i = 0u; i < 6000u; ++i) {
            ms_step(s);
            for (k = 0u; k < s->particles && i % 20u == 0u; ++k)
                for (trial = k + 1u; trial < s->particles; ++trial) {
                    const MsParticle *p = &s->particle[k], *q = &s->particle[trial];
                    deepest = fmax(deepest, (p->radius + q->radius - sqrt((q->x[0] - p->x[0]) * (q->x[0] - p->x[0]) + (q->x[1] - p->x[1]) * (q->x[1] - p->x[1]))) / fmin(p->radius, q->radius));
                }
        }
        demo_check(d, "box of 150 discs, 5 s, alpha 1: merges", (double)s->merges, "> 1000", s->merges > 1000u);
        demo_check(d, "  energy after / before - 1", ms_energy(s) / before - 1.0, "|.| < 1e-10", fabs(ms_energy(s) / before - 1.0) < 1e-10);
        demo_check(d, "  deepest overlap seen / the smaller radius (reported)", deepest, "", true);
        demo_check(d, "  steps done twice (reported)", (double)s->second_stages, "", true);
        demo_check(d, "  splits with a sideways term (reported)", (double)s->without_root, "", true);
    }

    /* 6. Figure 3 and 7: the ball on the sheet for 2 s, by each method, with ball particles of 1, 10 and 100
     * times the mass of a sheet particle. The springs of sheet and ball are made stiffer with the ball's mass
     * so that the sheet stretches the same, and the time step shorter to keep them stable. */
    {
        static const struct { MsMethod method; double penalty; const char *label; } k_case[5] = {
            {MS_MERGE, 0.0, "merging-and-splitting"}, {MS_PENALTY, 0.1, "penalty, 0.1 x the sheet's stiffness"}, {MS_PENALTY, 1.0, "penalty, 1 x"},
            {MS_PENALTY, 10.0, "penalty, 10 x"}, {MS_IMPULSE, 0.0, "impulses"}};
        uint32_t ratio;
        for (ratio = 1u; ratio <= 100u; ratio *= 10u)
            for (trial = 0u; trial < 5u; ++trial) {
                const uint32_t steps = ratio == 1u ? 2400u : ratio == 10u ? 8000u : 24000u;
                double         before, deepest = 0.0, fastest = 0.0, lowest = INFINITY;
                char           label[160];
                empty(s);
                sheet(s, SHEET * ratio);
                ball(s, 0.05 * ratio);
                s->dt = 2.0 / steps;
                s->method = k_case[trial].method; s->penalty = k_case[trial].penalty * SHEET * ratio;
                before = ms_energy(s);
                for (i = 0u; i < steps; ++i) {
                    ms_step(s);
                    for (k = 61u; k < s->particles && i % 10u == 0u; ++k) {
                        const MsParticle *p = &s->particle[k];
                        uint32_t          j;
                        for (j = 0u; j < 61u; ++j) {
                            const MsParticle *q = &s->particle[j];
                            deepest = fmax(deepest, (0.1 - sqrt((q->x[0] - p->x[0]) * (q->x[0] - p->x[0]) + (q->x[1] - p->x[1]) * (q->x[1] - p->x[1]))) / 0.05);
                            lowest  = fmin(lowest, q->x[1]);
                        }
                    }
                }
                for (k = 61u; k < s->particles; ++k) fastest = fmax(fastest, sqrt(s->particle[k].v[0] * s->particle[k].v[0] + s->particle[k].v[1] * s->particle[k].v[1]));
                snprintf(label, sizeof label, "ball particles %u x a sheet particle, %s: of 37 under the sheet after 2 s", ratio, k_case[trial].label);
                demo_check(d, label, under(s), trial ? "" : "0", trial || under(s) == 0u);
                demo_check(d, "  deepest overlap of a ball and a sheet particle / radius", deepest, trial ? "" : "< 0.5", trial || deepest < 0.5);
                demo_check(d, "  energy after / before - 1", ms_energy(s) / before - 1.0, trial ? "" : "|.| < 0.03", trial || fabs(ms_energy(s) / before - 1.0) < 0.03);
                demo_check(d, "  fastest ball particle at the end, m/s", fastest, trial ? "" : "< 9", trial || fastest < 9.0);
                if (trial) continue;
                demo_check(d, "  lowest point the sheet reached, m (its ends are at 2.2, the floor is at 0)", lowest, "> 0.1", lowest > 0.1);
                demo_check(d, "  steps done twice, of all", (double)s->second_stages / steps, "", true);
                demo_check(d, "  most particles in a meta-particle", (double)s->largest, "", true);
                demo_check(d, "  splits with a sideways term, of all", (double)s->without_root / (double)s->merges, "", true);
            }
    }

    /* 7. Rest in contact (section 7): a particle laid in the hollow between two others that hang from
     * springs. Nothing closes in at the start of a step, so only the second stage can hold it. alpha 0 and
     * damped springs, so that it comes to rest. */
    {
        double closest = INFINITY, speed = 0.0;
        empty(s);
        s->alpha = 0.0; s->spring_damping = 20.0;
        for (i = 0u; i < 2u; ++i) {
            ms_add(s, 2.95 + 0.1 * i, 2.2, 0.05, 0.05);
            s->particle[ms_add(s, 2.95 + 0.1 * i, 2.6, 0.05, 0.05)].pinned = true;
            ms_link(s, 2u * i, 2u * i + 1u, SHEET);
        }
        ms_link(s, 0u, 2u, SHEET);
        ms_add(s, 3.0, 2.2 + 0.0866, 0.05, 0.05);
        for (i = 0u; i < 4800u; ++i) {
            ms_step(s);
            for (k = 0u; k < 4u; k += 2u) closest = fmin(closest, sqrt((s->particle[k].x[0] - s->particle[4].x[0]) * (s->particle[k].x[0] - s->particle[4].x[0]) + (s->particle[k].x[1] - s->particle[4].x[1]) * (s->particle[k].x[1] - s->particle[4].x[1])) / 0.1);
        }
        for (k = 0u; k < 5u; ++k) speed = fmax(speed, sqrt(s->particle[k].v[0] * s->particle[k].v[0] + s->particle[k].v[1] * s->particle[k].v[1]));
        demo_check(d, "a particle laid on two that hang from springs, alpha 0, 4 s: steps done twice, of 4800", (double)s->second_stages, "> 2400", s->second_stages > 2400u);
        demo_check(d, "  closest it came to either / sum of radii", closest, "> 0.99", closest > 0.99);
        demo_check(d, "  its height above them at the end, m (0.0866 at the start)", s->particle[4].x[1] - s->particle[0].x[1], "0.0866 +- 0.001", fabs(s->particle[4].x[1] - s->particle[0].x[1] - 0.0866) < 0.001);
        demo_check(d, "  how far the two were pulled down, m (weight / stiffness: 9.2e-5)", 2.2 - s->particle[0].x[1], "9.2e-5 +- 1e-5", fabs(2.2 - s->particle[0].x[1] - 9.2e-5) < 1e-5);
        demo_check(d, "  fastest of the five at the end, m/s", speed, "< 1e-3", speed < 1e-3);
    }

    /* 8. Figure 11: 192 loose particles fall on the sheet at 3 m/s, with different limits on a meta-particle,
     * bouncing (alpha 1) and not (alpha 0.3). */
    {
        static const uint32_t k_limit[5] = {2u, 8u, 32u, 64u, 100000u};   /* the paper's largest: 64 */
        for (trial = 0u; trial < 10u; ++trial) {
            double   widest = 0.0, before, mean = 0.0;
            uint32_t deep = 0u, j;
            char     label[160];
            a->scene = 1u;
            seed(a);
            s->limit = k_limit[trial % 5u]; s->alpha = trial < 5u ? 1.0 : 0.3;
            before = ms_energy(s);
            for (i = 0u; i < 2400u; ++i) {
                ms_step(s);
                for (k = 1u; k < 61u; ++k) widest = fmax(widest, sqrt((s->particle[k].x[0] - s->particle[k - 1u].x[0]) * (s->particle[k].x[0] - s->particle[k - 1u].x[0]) + (s->particle[k].x[1] - s->particle[k - 1u].x[1]) * (s->particle[k].x[1] - s->particle[k - 1u].x[1])));
            }
            /* how far the loose particles are inside one another at the end */
            for (k = 61u; k < s->particles; ++k) {
                double nearest = INFINITY;
                for (j = 61u; j < s->particles; ++j)
                    if (j != k) nearest = fmin(nearest, sqrt((s->particle[k].x[0] - s->particle[j].x[0]) * (s->particle[k].x[0] - s->particle[j].x[0]) + (s->particle[k].x[1] - s->particle[j].x[1]) * (s->particle[k].x[1] - s->particle[j].x[1])));
                deep += nearest < 0.05;
                mean += fmax(0.0, 0.1 - nearest) / 0.05 / 192.0;
            }
            snprintf(label, sizeof label, "192 loose particles on the sheet, alpha %.1f, at most %u in a meta-particle: under the sheet after 2 s", s->alpha, k_limit[trial % 5u]);
            demo_check(d, label, under(s), trial % 5u == 3u ? "0" : "", trial % 5u != 3u || under(s) == 0u);
            demo_check(d, "  most particles in a meta-particle", (double)s->largest, "", true);
            demo_check(d, "  loose particles more than half inside another", deep, trial % 5u == 3u ? "0" : "", trial % 5u != 3u || deep == 0u);
            demo_check(d, "  mean overlap with the nearest other / radius", mean, "", true);
            demo_check(d, "  widest spacing of the sheet's particles, m (a particle is 0.1 across)", widest, "", true);
            demo_check(d, "  energy after / before - 1", ms_energy(s) / before - 1.0, "", true);
        }
        a->scene = 0u;
        seed(a);
    }
}

int main(int argc, char **argv) {
    static App app;
    static const DemoOption options[] = {
        {"--scene", .u32 = &app.scene, .lo = 0, .hi = 2, .help = "0 a ball on a sheet, 1 loose particles on a sheet, 2 a box of discs"},
        {"--method", .u32 = &app.method, .lo = 0, .hi = 2, .help = "collisions: 0 merging-and-splitting, 1 penalty force, 2 impulses"},
        {"--alpha", .f32 = &app.alpha, .lo = 0, .hi = 1, .help = "share of a collision's energy that is given back"},
        {"--penalty", .f32 = &app.penalty, .lo = 2, .hi = 6, .help = "log10 of the penalty stiffness, N/m"},
        {"--limit", .u32 = &app.limit, .lo = 2, .hi = 100000, .help = "most particles in one meta-particle"},
        {0}};
    Demo *d = &app.demo;
    app.alpha = 1.0f; app.penalty = 3.9f; app.limit = 64u;
    demo_defaults(d, "mergesplit", "Particle merging-and-splitting");
    demo_open(d, options, argc, argv);
    canvas_open(d, &app.canvas, 60000u, CANVAS_RGB(24, 27, 36));
    seed(&app);
    if (d->headless && !d->frames) d->frames = 90u;
    while (demo_running(d)) {
        if (d->ui) app_ui(&app, d->ui);
        app_frame(&app);
    }
    if (d->check) app_check(&app);
    return demo_close(d);
}
