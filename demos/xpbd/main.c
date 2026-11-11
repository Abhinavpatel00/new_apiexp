/* xpbd — position-based dynamics whose stiffness does not depend on the iteration count.
 *
 *   Draw -> Present          (the method runs in C: method.c)
 *
 * Macklin, Mueller, Chentanez: "XPBD: Position-Based Simulation of Compliant
 * Constrained Dynamics" (2016). Scene 0 is a cloth over a ball, scene 1 three
 * chains that carry the same weight and are solved with 5, 20 and 80
 * iterations, scene 2 a block of tetrahedra with separate stiffness for its
 * edges and its volume. --pbd runs the 2007 method on the same scenes.
 *
 * --check tests the gradients, the stretch of a spring and of a chain against
 * the weight they carry for several iteration counts and time steps, the
 * force estimate, and a swinging chain against implicit Euler by Newton.
 */
#include "canvas.h"
#include "method.h"
#include "softmesh.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define CLOTH      30u
#define LINKS      15u
#define LINK       0.06
#define LINK_MASS  0.05
#define WEIGHT     1.0
#define MAX_BODIES 3u

typedef struct App {
    Demo      demo;   /* first: see demo.h */
    Canvas    canvas;
    DemoOrbit orbit;
    Xpbd      body[MAX_BODIES];
    uint32_t  bodies;
    SoftMesh  block;
    uint32_t  scene, iterations, built;
    float     stiffness, bend, bulk;   /* log10 of N / m, N / m, Pa */
    float     pbd_stiffness, damping;
    bool      pbd, restart;
} App;

/* A body with room for `constraints`; the caller fills x, w and adds the constraints. */
static void body_open(Xpbd *s, uint32_t nodes, uint32_t constraints) {
    free(s->x); free(s->v); free(s->before); free(s->w); free(s->c);
    memset(s, 0, sizeof *s);
    s->nodes  = nodes;
    s->x      = (double *)calloc(nodes * 3u, sizeof(double));
    s->v      = (double *)calloc(nodes * 3u, sizeof(double));
    s->before = (double *)calloc(nodes * 3u, sizeof(double));
    s->w      = (double *)calloc(nodes, sizeof(double));
    s->c      = (XpbdConstraint *)calloc(constraints, sizeof(XpbdConstraint));
    s->dt = 1.0 / 120.0; s->gravity = 9.81; s->floor = -1e9; s->ball[1] = -1e9;
}

static void add_distance(Xpbd *s, uint32_t i, uint32_t j, double compliance) {
    XpbdConstraint *c = &s->c[s->constraints++];
    double          grad[4][3];
    *c = (XpbdConstraint){.kind = XPBD_DISTANCE, .node = {i, j}, .compliance = compliance};
    c->rest = xpbd_value(s, c, grad);
}

/* Three chains, each a row of links with a weight at the end, held at the other end. */
static void chain(Xpbd *s, double x, double compliance, double weight, double angle) {
    uint32_t i;
    body_open(s, LINKS + 1u, LINKS);
    for (i = 0u; i <= LINKS; ++i) {
        s->x[3u * i]      = x + LINK * i * sin(angle);
        s->x[3u * i + 1u] = 2.0 - LINK * i * cos(angle);
        s->w[i]           = i == 0u ? 0.0 : i == LINKS ? 1.0 / weight : 1.0 / LINK_MASS;
        if (i > 0u) add_distance(s, i - 1u, i, compliance);
    }
}

static int pair_order(const void *a, const void *b) { return memcmp(a, b, sizeof(uint32_t[2])); }

static void seed(App *a) {
    const double spring = 1.0 / pow(10.0, a->stiffness), bend = 1.0 / pow(10.0, a->bend), bulk = pow(10.0, a->bulk);
    Xpbd        *s = &a->body[0];
    uint32_t     i, j, t, k;
    a->bodies = 1u;
    if (a->scene == 0u) {
        /* A square of cloth held by two corners above a ball: sides and diagonals of every cell
         * resist stretch and shear, and every second node along a row or column resists bending. */
        body_open(s, CLOTH * CLOTH, CLOTH * CLOTH * 6u);
        for (j = 0u; j < CLOTH; ++j)
            for (i = 0u; i < CLOTH; ++i) {
                const uint32_t n = i + j * CLOTH;
                s->x[3u * n]      = 0.05 * i - 0.025 * (CLOTH - 1u);
                s->x[3u * n + 1u] = 1.7;
                s->x[3u * n + 2u] = 0.05 * j - 0.6;
                s->w[n]           = j == 0u && (i == 0u || i == CLOTH - 1u) ? 0.0 : 1.0 / (0.3 * 0.05 * 0.05);
            }
        for (j = 0u; j < CLOTH; ++j)
            for (i = 0u; i < CLOTH; ++i) {
                const uint32_t n = i + j * CLOTH;
                if (i + 1u < CLOTH) add_distance(s, n, n + 1u, spring);
                if (j + 1u < CLOTH) add_distance(s, n, n + CLOTH, spring);
                if (i + 1u < CLOTH && j + 1u < CLOTH) { add_distance(s, n, n + CLOTH + 1u, spring); add_distance(s, n + 1u, n + CLOTH, spring); }
                if (i + 2u < CLOTH) add_distance(s, n, n + 2u, bend);
                if (j + 2u < CLOTH) add_distance(s, n, n + 2u * CLOTH, bend);
            }
        s->floor = 0.0; s->friction = 0.3;
        s->ball[0] = 0.0; s->ball[1] = 0.8; s->ball[2] = 0.1; s->ball[3] = 0.4;
    } else if (a->scene == 1u) {
        a->bodies = 3u;
        for (i = 0u; i < 3u; ++i) chain(&a->body[i], -0.6 + 0.6 * i, spring, WEIGHT, 0.9);
    } else {
        /* A block: every edge of every tetrahedron is a spring, and every tetrahedron keeps its volume. */
        uint32_t (*pairs)[2] = malloc((size_t)a->block.tets * 6u * sizeof *pairs);
        body_open(s, a->block.nodes, a->block.tets * 7u);
        for (i = 0u; i < a->block.nodes; ++i) {
            const double *r = &a->block.rest[3u * i], tilt = 0.5;
            s->x[3u * i]      = cos(tilt) * (r[0] - 0.3) - sin(tilt) * r[1];
            s->x[3u * i + 1u] = sin(tilt) * (r[0] - 0.3) + cos(tilt) * r[1] + 0.7;
            s->x[3u * i + 2u] = r[2] - 0.15;
        }
        for (t = 0u; t < a->block.tets; ++t) {
            const uint32_t *q = &a->block.tet[4u * t];
            XpbdConstraint *c = &s->c[s->constraints++];
            double          grad[4][3];
            *c = (XpbdConstraint){.kind = XPBD_VOLUME, .node = {q[0], q[1], q[2], q[3]}};
            c->rest       = xpbd_value(s, c, grad);
            c->compliance = c->rest / bulk;   /* K (dV / V)^2 V / 2 = dV^2 / (2 V / K) */
            /* a quarter of the tetrahedron's mass to each corner, 200 kg / m^3 */
            for (k = 0u; k < 4u; ++k) s->w[q[k]] += 0.25 * 200.0 * c->rest;
            for (k = 0u; k < 6u; ++k) {
                static const uint8_t k_edge[6][2] = {{0, 1}, {0, 2}, {0, 3}, {1, 2}, {1, 3}, {2, 3}};
                const uint32_t       lo = q[k_edge[k][0]], hi = q[k_edge[k][1]];
                pairs[6u * t + k][0] = lo < hi ? lo : hi;
                pairs[6u * t + k][1] = lo < hi ? hi : lo;
            }
        }
        for (i = 0u; i < s->nodes; ++i) s->w[i] = 1.0 / s->w[i];
        qsort(pairs, (size_t)a->block.tets * 6u, sizeof *pairs, pair_order);
        for (k = 0u; k < a->block.tets * 6u; ++k)
            if (k == 0u || pair_order(pairs[k], pairs[k - 1u])) add_distance(s, pairs[k][0], pairs[k][1], spring);
        free(pairs);
        s->floor = 0.0; s->friction = 0.5;
    }
    a->built = a->scene;
    /* each scene from where it is seen best */
    a->orbit.target[1] = a->scene == 0u ? 0.9f : a->scene == 1u ? 1.35f : 0.25f;
    a->orbit.distance  = a->scene == 0u ? 3.4f : a->scene == 1u ? 2.8f : 1.7f;
}

static void app_ui(App *a, struct nk_context *ctx) {
    static const char *const k_scenes[3] = {"Cloth over a ball", "Three chains: 5, 20, 80 iterations", "Block: edges and volume"};
    if (nk_begin(ctx, "XPBD", nk_rect(16, 16, 330, 300), NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_TITLE)) {
        int iterations = (int)a->iterations;
        if (demo_ui_choice(ctx, "Scene", k_scenes, 3u, &a->scene)) a->restart = true;
        nk_layout_row_dynamic(ctx, 22, 1);
        nk_property_int(ctx, "Iterations", 1, &iterations, 200, 1, 1.0f);
        a->iterations = (uint32_t)iterations;
        demo_ui_bool(ctx, "PBD (2007) instead", &a->pbd);
        demo_ui_float(ctx, "PBD stiffness", &a->pbd_stiffness, 0.01f, 1.0f);
        demo_ui_float(ctx, "log10 spring N/m", &a->stiffness, 1.0f, 7.0f);
        demo_ui_float(ctx, "log10 bending N/m", &a->bend, -1.0f, 5.0f);
        demo_ui_float(ctx, "log10 bulk Pa", &a->bulk, 2.0f, 8.0f);
        demo_ui_float(ctx, "Damping beta, s", &a->damping, 0.0f, 0.05f);
        nk_layout_row_dynamic(ctx, 24, 1);
        if (nk_button_label(ctx, "Restart (new stiffness)")) a->restart = true;
    }
    nk_end(ctx);
}

static void app_frame(App *a) {
    Demo    *d = &a->demo;
    Canvas  *c = &a->canvas;
    uint32_t b, i, j, step;
    float    p[4][3];

    if (a->restart || a->built != a->scene) { seed(a); a->restart = false; }
    for (b = 0u; b < a->bodies; ++b) {
        Xpbd *s = &a->body[b];
        /* the chains differ only in how many iterations they get */
        s->iterations = a->scene == 1u ? (b == 0u ? (a->iterations + 3u) / 4u : b == 1u ? a->iterations : 4u * a->iterations) : a->iterations;
        s->pbd = a->pbd; s->stiffness = a->pbd_stiffness; s->damping = a->damping;
        for (step = 0u; step < 2u; ++step) xpbd_step(s);
    }

    canvas_view_orbit(d, c, &a->orbit);
    /* the floor, with a line every half metre */
    for (i = 0u; i < 2u; ++i) {
        const float q[4][3] = {{-4, 0, -4}, {4, 0, -4}, {4, 0, 4}, {-4, 0, 4}};
        canvas_triangle(c, q[0], q[1 + i], q[2 + i], CANVAS_RGB(92, 98, 110));
    }
    for (i = 0u; i <= 16u; ++i) {
        const float at = -4.0f + 0.5f * (float)i, lines[4][3] = {{at, 0.002f, -4}, {at, 0.002f, 4}, {-4, 0.002f, at}, {4, 0.002f, at}};
        canvas_line(c, lines[0], lines[1], 0.008f, CANVAS_RGB(70, 75, 86));
        canvas_line(c, lines[2], lines[3], 0.008f, CANVAS_RGB(70, 75, 86));
    }
    if (a->scene == 0u) {
        const Xpbd *s = &a->body[0];
        /* the ball, drawn a little smaller than it is so that the cloth on it is not cut by it */
        for (j = 0u; j < 12u; ++j)
            for (i = 0u; i < 24u; ++i) {
                for (b = 0u; b < 4u; ++b) {
                    const float lat = 3.14159265f * (float)(j + (b >> 1)) / 12.0f, lon = 6.2831853f * (float)(i + ((b ^ (b >> 1)) & 1u)) / 24.0f;
                    const float r = (float)s->ball[3] - 0.012f;
                    p[b][0] = (float)s->ball[0] + r * sinf(lat) * cosf(lon);
                    p[b][1] = (float)s->ball[1] + r * cosf(lat);
                    p[b][2] = (float)s->ball[2] + r * sinf(lat) * sinf(lon);
                }
                canvas_lit(c, p[0], p[1], p[2], CANVAS_RGB(150, 96, 60));
                canvas_lit(c, p[0], p[2], p[3], CANVAS_RGB(150, 96, 60));
            }
        for (j = 0u; j + 1u < CLOTH; ++j)
            for (i = 0u; i + 1u < CLOTH; ++i) {
                for (b = 0u; b < 4u; ++b) {
                    const double *x = &s->x[3u * (i + ((b ^ (b >> 1)) & 1u) + (j + (b >> 1)) * CLOTH)];
                    p[b][0] = (float)x[0]; p[b][1] = (float)x[1]; p[b][2] = (float)x[2];
                }
                canvas_lit(c, p[0], p[1], p[2], (i + j) % 2u ? CANVAS_RGB(70, 130, 200) : CANVAS_RGB(225, 228, 235));
                canvas_lit(c, p[0], p[2], p[3], (i + j) % 2u ? CANVAS_RGB(70, 130, 200) : CANVAS_RGB(225, 228, 235));
            }
    } else if (a->scene == 1u) {
        for (b = 0u; b < a->bodies; ++b) {
            const Xpbd *s = &a->body[b];
            for (i = 0u; i <= LINKS; ++i) {
                for (j = 0u; j < 2u; ++j) {
                    const double *x = &s->x[3u * (i > 0u ? i - 1u + j : 0u)];
                    p[j][0] = (float)x[0]; p[j][1] = (float)x[1]; p[j][2] = (float)x[2];
                }
                canvas_line(c, p[0], p[1], 0.012f, CANVAS_RGB(230, 232, 238));
                canvas_dot(c, p[1], i == LINKS ? 0.07f : 0.018f, i == LINKS ? CANVAS_RGB(220, 120, 50) : CANVAS_RGB(120, 170, 230));
            }
        }
    } else {
        softmesh_draw(&a->block, a->body[0].x, c, CANVAS_RGB(210, 150, 70), 0.004f);
    }
    canvas_frame(d, c);
}

static void app_init(App *a) {
    softmesh_cells(&a->block, 6u, 3u, 3u, 0.1, NULL);
    canvas_open(&a->demo, &a->canvas, 90000u, CANVAS_RGB(38, 42, 52));
    seed(a);
    if (a->demo.headless && !a->demo.frames) a->demo.frames = 150u;
}

/* The lowest point of a body, and how far its last node is from its first. */
static double span(const Xpbd *s) {
    const double *p = s->x, *q = &s->x[3u * (s->nodes - 1u)];
    return sqrt((p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]) + (p[2] - q[2]) * (p[2] - q[2]));
}

static void app_check(App *a) {
    Demo    *d = &a->demo;
    Xpbd    *s = &a->body[0], *other = &a->body[1];
    uint32_t i, j, k, step, n, trial;
    uint32_t rng = 2024u;

    /* 1. The gradients of both kinds of constraint against differences. */
    {
        double worst[2] = {0.0, 0.0};
        a->scene = 2u; a->stiffness = 3.5f; a->bulk = 6.0f;
        seed(a);
        for (i = 0u; i < s->nodes * 3u; ++i) { rng = rng * 1664525u + 1013904223u; s->x[i] += 0.02 * ((double)(rng >> 8) / 8388608.0 - 1.0); }
        for (j = 0u; j < s->constraints; j += 7u) {
            XpbdConstraint *c = &s->c[j];
            double          grad[4][3], unused[4][3];
            xpbd_value(s, c, grad);
            for (n = 0u; n < (c->kind == XPBD_DISTANCE ? 2u : 4u); ++n)
                for (k = 0u; k < 3u; ++k) {
                    double *x = &s->x[3u * c->node[n] + k], plus, minus;
                    const double step_size = 1e-6, keep = *x;
                    *x = keep + step_size; plus = xpbd_value(s, c, unused);
                    *x = keep - step_size; minus = xpbd_value(s, c, unused);
                    *x = keep;
                    /* a volume gradient is an area: compare relative to the size of the tetrahedron's faces */
                    worst[c->kind] = fmax(worst[c->kind], fabs((plus - minus) / (2.0 * step_size) - grad[n][k]) / (c->kind == XPBD_VOLUME ? 0.005 : 1.0));
                }
        }
        demo_check(d, "gradient of a length - differences", worst[0], "< 1e-8", worst[0] < 1e-8);
        demo_check(d, "gradient of a volume - differences, / area of a face", worst[1], "< 1e-8", worst[1] < 1e-8);
    }

    /* 2. One spring, compliance 1e-3 m / N, carrying 2 kg: it should stretch by alpha m g = 19.62 mm
     * whatever the iteration count and the time step. PBD with stiffness 0.5 on the same spring. */
    {
        static const uint32_t k_iterations[3] = {1u, 10u, 100u};
        static const double   k_dt[2] = {1.0 / 60.0, 1.0 / 240.0};
        double xpbd_worst = 0.0, pbd_low = 1e30, pbd_high = 0.0;
        for (trial = 0u; trial < 12u; ++trial) {
            const bool pbd = trial >= 6u;
            double     stretch;
            body_open(s, 2u, 1u);
            s->x[4] = -0.5; s->w[1] = 1.0 / 2.0;
            add_distance(s, 0u, 1u, 1e-3);
            s->dt = k_dt[trial % 2u]; s->iterations = k_iterations[trial % 6u / 2u]; s->pbd = pbd; s->stiffness = 0.5; s->damping = 0.02;
            for (step = 0u; step < (uint32_t)(20.0 / s->dt); ++step) xpbd_step(s);
            stretch = -s->x[4] - 0.5;
            if (!pbd) xpbd_worst = fmax(xpbd_worst, fabs(stretch / (1e-3 * 2.0 * 9.81) - 1.0));
            if (pbd) { pbd_low = fmin(pbd_low, stretch); pbd_high = fmax(pbd_high, stretch); }
        }
        demo_check(d, "one spring, 1 / 10 / 100 iterations x dt 1/60, 1/240: stretch / (alpha m g) - 1, worst", xpbd_worst, "< 1e-6", xpbd_worst < 1e-6);
        demo_check(d, "  PBD, stiffness 0.5, the same six runs: largest stretch, mm (19.62 would be right)", 1000.0 * pbd_high, "< 5", pbd_high < 0.005);
        demo_check(d, "  smallest, mm", 1000.0 * pbd_low, "< 1e-6", pbd_low < 1e-9);
    }

    /* 3. A chain of 15 links with 1 kg at the end, at rest: each link stretches by alpha x the weight
     * below it. The passes over the constraints have to carry that weight up the chain every time
     * step, so too few of them leave the chain too long; they never make it stiffer. */
    {
        static const uint32_t k_iterations[4] = {5u, 20u, 80u, 320u};
        double length[2][4], expected = 0.0, force = 0.0;
        for (i = 1u; i <= LINKS; ++i) expected += LINK + 1e-3 * 9.81 * (WEIGHT + LINK_MASS * (LINKS - i));
        for (trial = 0u; trial < 8u; ++trial) {
            chain(s, 0.0, 1e-3, WEIGHT, 0.0);
            s->iterations = k_iterations[trial % 4u]; s->pbd = trial >= 4u; s->stiffness = 0.5; s->damping = 0.02;
            for (step = 0u; step < 12000u; ++step) xpbd_step(s);   /* 100 s: the weight bobs slowly */
            length[trial / 4u][trial % 4u] = span(s);
            if (trial == 3u) force = s->c[0].lambda / (s->dt * s->dt);
        }
        demo_check(d, "chain at rest: length / (rest + alpha x weights) - 1 with 5 iterations (reported)", length[0][0] / expected - 1.0, "", true);
        demo_check(d, "  20 iterations", length[0][1] / expected - 1.0, "< 1e-4", fabs(length[0][1] / expected - 1.0) < 1e-4);
        demo_check(d, "  80 iterations", length[0][2] / expected - 1.0, "< 1e-9", fabs(length[0][2] / expected - 1.0) < 1e-9);
        demo_check(d, "  320 iterations", length[0][3] / expected - 1.0, "< 1e-9", fabs(length[0][3] / expected - 1.0) < 1e-9);
        demo_check(d, "  PBD, stiffness 0.5: its stretch / the right stretch with 5 iterations (reported)", (length[1][0] - LINK * LINKS) / (expected - LINK * LINKS), "", true);
        demo_check(d, "  PBD, 320 iterations (it has gone rigid)", (length[1][3] - LINK * LINKS) / (expected - LINK * LINKS), "< 0.05",
                   (length[1][3] - LINK * LINKS) / (expected - LINK * LINKS) < 0.05);
        demo_check(d, "  force in the top link from lambda / dt^2, / weight of everything below - 1 (320 iterations)",
                   -force / (9.81 * (WEIGHT + LINK_MASS * (LINKS - 1u))) - 1.0, "< 1e-8", fabs(-force / (9.81 * (WEIGHT + LINK_MASS * (LINKS - 1u))) - 1.0) < 1e-8);
    }

    /* 4. With no compliance XPBD is PBD with stiffness 1. */
    {
        double differ = 0.0;
        for (trial = 0u; trial < 2u; ++trial) {
            Xpbd *body = trial ? other : s;
            chain(body, 0.0, 0.0, WEIGHT, 0.9);
            body->iterations = 10u; body->pbd = trial == 1u; body->stiffness = 1.0;
            for (step = 0u; step < 240u; ++step) xpbd_step(body);
        }
        for (i = 0u; i < s->nodes * 3u; ++i) differ = fmax(differ, fabs(s->x[i] - other->x[i]));
        demo_check(d, "compliance 0 against PBD with stiffness 1, a swinging chain, 240 steps: largest difference", differ, "== 0", differ == 0.0);
    }

    /* 5. A swinging chain against implicit Euler on the energy C^2 / (2 alpha), solved by Newton
     * (the comparison of the paper's section 6). */
    {
        static const uint32_t k_iterations[3] = {20u, 200u, 2000u};
        double gap[3] = {0.0, 0.0, 0.0}, travelled = 0.0;
        for (trial = 0u; trial < 3u; ++trial) {
            chain(s, 0.0, 1e-3, WEIGHT, 0.9);
            chain(other, 0.0, 1e-3, WEIGHT, 0.9);
            s->iterations = k_iterations[trial];
            for (step = 0u; step < 120u; ++step) {
                xpbd_step(s);
                xpbd_reference_step(other);
                for (i = 0u; i < s->nodes * 3u; ++i) gap[trial] = fmax(gap[trial], fabs(s->x[i] - other->x[i]));
            }
            travelled = fabs(other->x[3u * LINKS] - LINK * LINKS * sin(0.9));
        }
        demo_check(d, "swinging chain, 1 s: how far the weight travelled sideways, m (reported)", travelled, "", true);
        demo_check(d, "  XPBD - implicit Euler by Newton, largest distance over the run, mm: 20 iterations", 1000.0 * gap[0], "< 5", gap[0] < 0.005);
        demo_check(d, "  200 iterations", 1000.0 * gap[1], "< 5", gap[1] < 0.005);
        demo_check(d, "  2000 iterations (what is left is XPBD's own approximation)", 1000.0 * gap[2], "< 5", gap[2] < 0.005);
    }

    /* 6. The scenes themselves. */
    {
        double low = 1e30, inside = 0.0, stretch = 0.0, volume = 0.0, rest = 0.0, grad[4][3];
        a->scene = 0u; a->stiffness = 3.5f; a->bend = 1.0f;
        seed(a);
        s->iterations = 20u;
        for (step = 0u; step < 600u; ++step) xpbd_step(s);
        for (i = 0u; i < s->nodes; ++i) {
            const double *x = &s->x[3u * i], away = sqrt((x[0] - s->ball[0]) * (x[0] - s->ball[0]) + (x[1] - s->ball[1]) * (x[1] - s->ball[1]) + (x[2] - s->ball[2]) * (x[2] - s->ball[2]));
            low    = fmin(low, x[1]);
            inside = fmax(inside, s->ball[3] - away);
        }
        for (j = 0u; j < s->constraints; ++j)
            if (s->c[j].rest < 0.08) stretch = fmax(stretch, xpbd_value(s, &s->c[j], grad) / s->c[j].rest);
        demo_check(d, "cloth after 5 s: lowest node, m", low, ">= 0", low >= 0.0);
        demo_check(d, "  deepest node inside the ball, mm", 1000.0 * inside, "< 1", inside < 1e-3);
        demo_check(d, "  most stretched side or diagonal, share of its length (reported)", stretch, "", true);

        a->scene = 2u; a->bulk = 9.0f;
        seed(a);
        s->iterations = 40u;
        for (step = 0u; step < 600u; ++step) xpbd_step(s);
        for (j = 0u; j < s->constraints; ++j)
            if (s->c[j].kind == XPBD_VOLUME) { volume += xpbd_value(s, &s->c[j], grad) + s->c[j].rest; rest += s->c[j].rest; }
        demo_check(d, "block dropped on the floor, bulk modulus 1e9 Pa, 5 s: volume / rest volume - 1", volume / rest - 1.0, "< 1e-3", fabs(volume / rest - 1.0) < 1e-3);
        a->bulk = 3.0f;
        seed(a);
        s->iterations = 40u;
        volume = 0.0;
        for (step = 0u; step < 600u; ++step) xpbd_step(s);
        for (j = 0u; j < s->constraints; ++j)
            if (s->c[j].kind == XPBD_VOLUME) volume += xpbd_value(s, &s->c[j], grad) + s->c[j].rest;
        demo_check(d, "  bulk modulus 1e3 Pa: volume / rest volume - 1 (reported)", volume / rest - 1.0, "", true);
    }
}

int main(int argc, char **argv) {
    static App app;
    static const DemoOption options[] = {
        {"--scene", .u32 = &app.scene, .lo = 0, .hi = 2, .help = "0 cloth over a ball, 1 three chains (iterations / 4, x 1, x 4), 2 block of tetrahedra"},
        {"--iterations", .u32 = &app.iterations, .lo = 1, .hi = 200, .help = "constraint passes per time step (1/120 s)"},
        {"--pbd", .flag = &app.pbd, .help = "position-based dynamics of 2007 instead: no compliance, --pbd-stiffness"},
        {"--pbd-stiffness", .f32 = &app.pbd_stiffness, .lo = 0.01, .hi = 1, .help = "PBD's stiffness, which means something different for every iteration count"},
        {"--stiffness", .f32 = &app.stiffness, .lo = 1, .hi = 7, .help = "log10 of the spring stiffness, N/m"},
        {"--bend", .f32 = &app.bend, .lo = -1, .hi = 5, .help = "log10 of the cloth's bending springs, N/m"},
        {"--bulk", .f32 = &app.bulk, .lo = 2, .hi = 9, .help = "log10 of the block's bulk modulus, Pa"},
        {"--damping", .f32 = &app.damping, .lo = 0, .hi = 0.05, .help = "beta of the paper's constraint damping, s"},
        {0}};
    Demo *d = &app.demo;
    app.iterations = 20u; app.stiffness = 3.5f; app.bend = 1.0f; app.bulk = 5.0f; app.pbd_stiffness = 0.5f;
    app.orbit = (DemoOrbit){.target = {0.0f, 0.9f, 0.0f}, .distance = 3.6f, .yaw = 0.6f, .pitch = 0.25f,
                            .fov_deg = 40.0f, .znear = 0.05f, .zfar = 50.0f, .min_distance = 1.0f, .max_distance = 15.0f};
    demo_defaults(d, "xpbd", "XPBD - compliant position-based dynamics");
    demo_open(d, options, argc, argv);
    app_init(&app);
    while (demo_running(d)) {
        if (d->ui) app_ui(&app, d->ui);
        app_frame(&app);
    }
    if (d->check) app_check(&app);
    return demo_close(d);
}
