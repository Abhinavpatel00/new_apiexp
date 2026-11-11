/* contact — Contact and Friction Simulation for Computer Graphics: rigid bodies that touch.
 *
 *   Draw -> Present          (the method runs in C: method.c)
 *
 * Andrews, Erleben, Ferguson: SIGGRAPH 2022 course notes. Spheres and boxes; where they
 * touch (section 2.1), the contact problem (chapter 1) and iterative solvers for it
 * (sections 3.2 and 3.4).
 *
 *   scene 0  a heavy block on a tower of light ones, three times: solved by Jacobi (orange),
 *            Gauss-Seidel (blue) and NNCG (green) with the same number of iterations; the
 *            board behind shows how each converges on one contact problem, the white line
 *            where a step stops
 *   scene 1  pucks sent off in twelve directions on three grounds: friction as a box, as
 *            Coulomb's cone, and as an ellipse that grips three times better across than along
 *   scene 2  a ramp: boxes that slide or hold, a ball that rolls; balls that spin on the
 *            ground with and without friction against spinning
 *   scene 3  boxes and balls dropped into a pit
 *
 * --check tests the contacts of section 2.1, the closest point of an ellipsoid, the
 * contact conditions at the solutions of every solver, friction against closed forms, and
 * what the notes say about the solvers.
 */
#include "canvas.h"
#include "method.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define WORLDS 3
#define CURVE  200

typedef struct App {
    Demo      demo;   /* first: see demo.h */
    Canvas    canvas;
    DemoOrbit orbit;
    Cf       *world[WORLDS];
    int       worlds;
    uint32_t  scene, built, steps, skip, iterations, method, set, bodies;
    bool      pause, restart, adapt;
    double    curve[WORLDS][CURVE];
    float     from[WORLDS][16][3];   /* scene 1: where each puck started */
} App;

static double noise(uint32_t *state) {
    *state = *state * 1664525u + 1013904223u;
    return (double)(*state >> 8) / 8388608.0 - 1.0;
}

/* A world with a ground whose top is y = 0. */
static Cf *world_open(void) {
    const double half[3] = {30.0, 1.0, 30.0}, at[3] = {0.0, -1.0, 0.0};
    Cf          *c = cf_open();
    cf_add(c, CF_BOX, half, 0.0, at);
    c->body[0].mu[2] = 1.0;
    return c;
}

/* A tower of `count` boxes 0.4 x 0.2 x 0.4 m of 32 kg, the top one `heavy` times that. */
static Cf *tower(int count, double heavy) {
    Cf *c = world_open();
    int i;
    for (i = 0; i < count; ++i) {
        const double half[3] = {0.2, 0.1, 0.2}, at[3] = {0.0, 0.1 + 0.2 * i, 0.0};
        cf_add(c, CF_BOX, half, i == count - 1 ? 1000.0 * heavy : 1000.0, at);
    }
    return c;
}

/* Where a puck sent off at `speed` in direction `angle` stops on the ground of world `w` of scene 1. */
static void puck_stop(int w, double speed, double angle, double gravity, double out[2]) {
    const double mu[2] = {w == 2 ? 0.25 : 0.5, w == 2 ? 0.75 : 0.5};
    double       v[2] = {speed * cos(angle), speed * sin(angle)}, h = 1e-5, size;
    out[0] = out[1] = 0.0;
    /* box: each direction slows by itself. Ellipse: the friction that takes out most energy is
     * -(mu_x^2 v_x, mu_z^2 v_z) g / |(mu_x v_x, mu_z v_z)|, followed in small steps. */
    if (w == 0) { out[0] = v[0] * fabs(v[0]) / (2.0 * mu[0] * gravity); out[1] = v[1] * fabs(v[1]) / (2.0 * mu[1] * gravity); }
    while (w && (size = hypot(mu[0] * v[0], mu[1] * v[1])) > 1e-4) {
        const double a[2] = {-mu[0] * mu[0] * v[0] * gravity / size, -mu[1] * mu[1] * v[1] * gravity / size};
        out[0] += h * (v[0] + 0.5 * h * a[0]); out[1] += h * (v[1] + 0.5 * h * a[1]);
        v[0]   += h * a[0];                    v[1]   += h * a[1];
    }
}

static void build(App *a) {
    static const DemoOrbit k_view[4] = {
        {.target = {0.0f, 0.9f, 0.0f}, .distance = 6.0f, .yaw = 0.35f, .pitch = 0.22f},
        {.target = {0.3f, 0.0f, 0.1f}, .distance = 9.8f, .yaw = 0.0f, .pitch = 1.05f},
        {.target = {0.0f, 0.5f, 0.4f}, .distance = 7.5f, .yaw = 0.5f, .pitch = 0.45f},
        {.target = {0.0f, 0.5f, 0.0f}, .distance = 5.5f, .yaw = 0.6f, .pitch = 0.55f}};
    uint32_t rng = 11u;
    int      w, i, k;

    /* a new scene has its own view; a restart keeps the one the user has */
    if (!a->world[0] || a->built != a->scene) {
        a->orbit = k_view[a->scene];
        a->orbit.fov_deg = 40.0f; a->orbit.znear = 0.05f; a->orbit.zfar = 80.0f; a->orbit.min_distance = 0.5f; a->orbit.max_distance = 30.0f;
    }
    for (w = 0; w < a->worlds; ++w) cf_close(a->world[w]);
    a->worlds = a->scene < 2u ? 3 : 1;
    a->built  = a->scene;
    a->steps  = 0u;
    memset(a->curve, 0, sizeof a->curve);

    for (w = 0; w < a->worlds; ++w) {
        Cf *c = a->world[w] = a->scene == 0u ? tower(6, 50.0) : world_open();
        c->iterations = (int)a->iterations;
        c->set        = (int)a->set;
        c->method     = (int)a->method;
        if (a->scene == 0u) c->method = w;
        if (a->scene == 1u) {
            /* twelve pucks on a ring, each sent outwards at 2.5 m/s; the ground decides the friction */
            c->set           = w ? CF_SET_ELLIPSE : CF_SET_BOX;
            c->body[0].mu[0] = w == 2 ? 0.25 : 0.5;
            c->body[0].mu[1] = w == 2 ? 0.75 : 0.5;
            for (i = 0; i < 12; ++i) {
                const double angle = 0.5235987755982988 * i, half[3] = {0.07, 0.025, 0.07}, at[3] = {0.5 * cos(angle), 0.025, 0.5 * sin(angle)};
                const int    index = cf_add(c, CF_BOX, half, 1000.0, at);   /* first: cf_add may move c->body */
                CfBody      *b = &c->body[index];
                b->mu[0] = b->mu[1] = 1.0;
                b->v[0]  = 2.5 * cos(angle);
                b->v[2]  = 2.5 * sin(angle);
                for (k = 0; k < 3; ++k) a->from[w][i][k] = (float)at[k];
            }
        }
        if (a->scene == 2u) {
            /* a ramp of 20 degrees that goes down towards +x; on it boxes with friction 0.2, 0.3 and 0.5 and a ball;
             * on the ground three balls that spin at 30 rad/s with friction against spinning of 0, 5 mm and 2 cm */
            static const double k_mu[3] = {0.2, 0.3, 0.5}, k_spin[3] = {0.0, 0.005, 0.02};
            const double        slope = 0.3490658503988659, turn[4] = {cos(0.5 * slope), 0.0, 0.0, -sin(0.5 * slope)};
            const double        along[3] = {cos(slope), -sin(slope), 0.0}, up[3] = {sin(slope), cos(slope), 0.0};
            const double        half[3] = {2.2, 0.1, 1.3}, at[3] = {-1.0, 1.0, 0.0};
            const int    index = cf_add(c, CF_BOX, half, 0.0, at);   /* first: cf_add may move c->body */
            CfBody      *ramp = &c->body[index];
            memcpy(ramp->q, turn, sizeof turn);
            ramp->mu[0] = ramp->mu[1] = 1.0;
            cf_place(ramp);
            for (i = 0; i < 4; ++i) {
                const double size[3] = {0.15, 0.15, 0.15}, z = -0.9 + 0.6 * i;
                const double p[3] = {at[0] - 1.8 * along[0] + 0.25 * up[0], at[1] - 1.8 * along[1] + 0.25 * up[1], z};
                const int    index = cf_add(c, i < 3 ? CF_BOX : CF_SPHERE, size, 1000.0, p);   /* first: cf_add may move c->body */
                CfBody      *b = &c->body[index];
                memcpy(b->q, turn, sizeof turn);
                b->mu[0] = b->mu[1] = i < 3 ? k_mu[i] : 1.0;
                cf_place(b);
            }
            for (i = 0; i < 3; ++i) {
                const double size[3] = {0.15, 0.15, 0.15}, p[3] = {-1.6 + 0.8 * i, 0.15, 2.0};
                const int    index = cf_add(c, CF_SPHERE, size, 1000.0, p);   /* first: cf_add may move c->body */
                CfBody      *b = &c->body[index];
                b->w[1]  = 30.0;
                b->mu[2] = k_spin[i];
            }
        }
        if (a->scene == 3u) {
            /* a pit of four walls; boxes and balls in turn above it */
            for (i = 0; i < 4; ++i) {
                const double half[3] = {i < 2 ? 0.1 : 1.1, 0.4, i < 2 ? 1.1 : 0.1};
                const double at[3] = {i < 2 ? (i ? 1.1 : -1.1) : 0.0, 0.4, i < 2 ? 0.0 : (i == 2 ? 1.1 : -1.1)};
                cf_add(c, CF_BOX, half, 0.0, at);
            }
            for (i = 0; i < (int)a->bodies; ++i) {
                const double size = 0.11 + 0.04 * noise(&rng), half[3] = {size, i % 2 ? size : 0.6 * size, i % 2 ? size : 1.3 * size};
                const double at[3] = {-0.6 + 0.4 * (i % 4) + 0.03 * noise(&rng), 0.6 + 0.45 * (i / 8), -0.3 + 0.6 * (i / 4 % 2) + 0.03 * noise(&rng)};
                const int    index = cf_add(c, i % 2 ? CF_SPHERE : CF_BOX, half, 1000.0, at);   /* first: cf_add may move c->body */
                CfBody      *b = &c->body[index];
                for (k = 0; k < 4; ++k) b->q[k] = noise(&rng) + (k == 0 ? 1.5 : 0.0);
                cf_place(b);
            }
        }
    }
}

static void app_ui(App *a, struct nk_context *ctx) {
    static const char *const k_scenes[4] = {"Heavy block on a tower, three solvers", "Pucks: box, cone, ellipse", "Ramp and spinning balls", "Pit"};
    static const char *const k_sets[2] = {"Box (BLCP)", "Ellipse (cone)"};
    static const char *const k_methods[3] = {"Jacobi", "Gauss-Seidel", "NNCG"};
    static const char *const k_worlds[2][3] = {{"Jacobi (orange)", "Gauss-Seidel (blue)", "NNCG (green)"}, {"box (orange)", "cone (blue)", "ellipse 0.25 / 0.75 (green)"}};
    int w;
    if (nk_begin(ctx, "Contact and Friction", nk_rect(16, 16, 400, 380), NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_TITLE)) {
        int iterations = (int)a->iterations;
        if (demo_ui_choice(ctx, "Scene", k_scenes, 4u, &a->scene)) a->restart = true;
        nk_layout_row_dynamic(ctx, 22, 1);
        nk_property_int(ctx, "Iterations a step", 2, &iterations, CURVE, 1, 1.0f);
        a->iterations = (uint32_t)iterations;
        if (a->scene != 1u) demo_ui_choice(ctx, "Friction set", k_sets, 2u, &a->set);
        if (a->scene >= 2u) demo_ui_choice(ctx, "Solver", k_methods, 3u, &a->method);
        if (a->scene == 3u) {
            int bodies = (int)a->bodies;
            nk_layout_row_dynamic(ctx, 22, 1);
            nk_property_int(ctx, "Bodies", 1, &bodies, 96, 1, 1.0f);
            a->restart = a->restart || bodies != (int)a->bodies;
            a->bodies  = (uint32_t)bodies;
        }
        demo_ui_bool(ctx, "Undo iterations that change more (not NNCG)", &a->adapt);
        demo_ui_bool(ctx, "Pause", &a->pause);
        nk_layout_row_dynamic(ctx, 24, 1);
        if (nk_button_label(ctx, "Restart")) a->restart = true;
        for (w = 0; w < a->worlds; ++w) {
            const Cf *c = a->world[w];
            demo_ui_text(ctx, "%s: %d contacts, change %.1e", a->worlds == 1 ? k_methods[c->method] : k_worlds[a->scene][w], c->contacts, c->change);
        }
        if (a->scene == 0u) demo_ui_text(ctx, "board: residual over %d iterations, 1 to 1e-6", CURVE);
        demo_ui_text(ctx, "white lines: normal impulses");
    }
    nk_end(ctx);
}

static void app_frame(App *a) {
    static const uint32_t k_colour[WORLDS] = {CANVAS_RGB(215, 150, 80), CANVAS_RGB(90, 160, 215), CANVAS_RGB(110, 190, 120)};
    static const uint32_t k_restart[4] = {720u, 300u, 600u, 900u};
    const float spacing = a->scene == 0u ? 1.6f : 3.4f;
    Demo    *d = &a->demo;
    Canvas  *c = &a->canvas;
    uint32_t n;
    int      w, i, j, k;

    if (a->restart || a->built != a->scene || a->steps >= k_restart[a->scene]) { build(a); a->restart = false; }
    for (n = 0u; n < (a->pause ? 0u : 1u + a->skip); ++n) {
        for (w = 0; w < a->worlds; ++w) {
            Cf *s = a->world[w];
            s->iterations = (int)a->iterations;
            if (a->scene != 1u) s->set = (int)a->set;
            if (a->scene >= 2u) s->method = (int)a->method;
            s->adapt = a->adapt;
            cf_step(s);
        }
        a->steps++;
    }
    a->skip = 0u;
    for (w = 0; w < (a->scene == 0u && a->steps ? WORLDS : 0); ++w) {
        /* the board: this step's problem of the last world, from no impulses, by each of the three solvers */
        Cf    *s = a->world[WORLDS - 1];
        static double keep[4 * 512];
        const double  change = s->change;
        const int     method = s->method, contacts = s->contacts < 512 ? s->contacts : 512;
        memcpy(keep, s->lambda, 4u * (size_t)contacts * sizeof(double));
        memset(s->lambda, 0, 4u * (size_t)s->contacts * sizeof(double));
        s->method = w; s->iterations = CURVE;
        cf_solve(s, a->curve[w]);
        s->method = method; s->change = change; s->iterations = (int)a->iterations;
        memcpy(s->lambda, keep, 4u * (size_t)contacts * sizeof(double));
    }

    canvas_view_orbit(d, c, &a->orbit);
    for (i = 0; i < 2; ++i) {
        const float q[4][3] = {{-12, 0, -12}, {12, 0, -12}, {12, 0, 12}, {-12, 0, 12}};
        canvas_triangle(c, q[0], q[1 + i], q[2 + i], CANVAS_RGB(92, 98, 110));
    }
    for (i = 0; i <= 48; ++i) {
        const float at = -12.0f + 0.5f * (float)i, lines[4][3] = {{at, 0.003f, -12}, {at, 0.003f, 12}, {-12, 0.003f, at}, {12, 0.003f, at}};
        canvas_line(c, lines[0], lines[1], 0.01f, CANVAS_RGB(70, 75, 86));
        canvas_line(c, lines[2], lines[3], 0.01f, CANVAS_RGB(70, 75, 86));
    }

    for (w = 0; w < a->worlds; ++w) {
        const Cf   *s = a->world[w];
        const float shift = ((float)w - 0.5f * (float)(a->worlds - 1)) * spacing;
        double      lightest = 1e300;
        for (i = 1; i < s->bodies; ++i) {
            const CfBody  *b = &s->body[i];
            const bool     heavy = a->scene == 0u && i == s->bodies - 1;
            const uint32_t still = CANVAS_RGB(130, 136, 150), tint = heavy ? CANVAS_RGB(70, 72, 84) : b->mass_inv > 0.0 ? k_colour[a->worlds == 1 ? (i % 3) : w] : still;
            const uint32_t dark = CANVAS_RGB((tint & 255u) * 3u / 5u, (tint >> 8 & 255u) * 3u / 5u, (tint >> 16 & 255u) * 3u / 5u);
            float          p[8][3];
            lightest = b->mass_inv > 0.0 ? fmin(lightest, 1.0 / b->mass_inv) : lightest;
            for (j = 0; j < (b->shape == CF_BOX ? 8 : 0); ++j) {
                const double local[3] = {(j & 1 ? 1.0 : -1.0) * b->half[0], (j & 2 ? 1.0 : -1.0) * b->half[1], (j & 4 ? 1.0 : -1.0) * b->half[2]};
                for (k = 0; k < 3; ++k) p[j][k] = (float)(b->p[k] + b->r[3 * k] * local[0] + b->r[3 * k + 1] * local[1] + b->r[3 * k + 2] * local[2]) + (k == 0 ? shift : 0.0f);
            }
            for (j = 0; j < (b->shape == CF_BOX ? 6 : 0); ++j) {
                /* face j: the corners that have bit `axis` set (odd j) or clear */
                const int axis = j / 2, u = 1 << (axis + 1) % 3, v = 1 << (axis + 2) % 3, base = j % 2 ? 1 << axis : 0;
                canvas_lit(c, p[base], p[base | u], p[base | u | v], tint);
                canvas_lit(c, p[base], p[base | u | v], p[base | v], tint);
                canvas_line(c, p[base], p[base | u], 0.004f, dark);
                canvas_line(c, p[base | v], p[base | u | v], 0.004f, dark);
            }
            for (j = 0; j < (b->shape == CF_SPHERE ? 8 * 16 : 0); ++j) {
                /* a ball of 8 x 16 patches, its eight octants in two tints so that turning shows */
                const int    ring = j / 16, slice = j % 16;
                const double t0 = 0.39269908169872414 * ring, t1 = t0 + 0.39269908169872414, f0 = 0.39269908169872414 * slice, f1 = f0 + 0.39269908169872414;
                const double corner[4][3] = {{sin(t0) * cos(f0), cos(t0), sin(t0) * sin(f0)}, {sin(t1) * cos(f0), cos(t1), sin(t1) * sin(f0)},
                                             {sin(t1) * cos(f1), cos(t1), sin(t1) * sin(f1)}, {sin(t0) * cos(f1), cos(t0), sin(t0) * sin(f1)}};
                const bool   light = ((ring / 4) + (slice / 4)) % 2;
                int          v;
                for (v = 0; v < 4; ++v) {
                    for (k = 0; k < 3; ++k) p[v][k] = (float)(b->p[k] + b->half[0] * (b->r[3 * k] * corner[v][0] + b->r[3 * k + 1] * corner[v][1] + b->r[3 * k + 2] * corner[v][2])) + (k == 0 ? shift : 0.0f);
                }
                canvas_lit(c, p[0], p[1], p[2], light ? tint : dark);
                canvas_lit(c, p[0], p[2], p[3], light ? tint : dark);
            }
        }
        for (i = 0; i < s->contacts; ++i) {
            /* a contact and its normal impulse, drawn 12 cm nearer the eye so that the faces it lies in do not hide it;
             * the length is 5 cm for the impulse that carries the lightest body, and grows with the square root */
            const CfContact *q = &s->contact[i];
            const double     length = 0.05 * sqrt(s->lambda[4 * i] / (lightest * s->gravity * s->dt));
            const float      at[3] = {(float)q->point[0] + shift, (float)q->point[1], (float)q->point[2]};
            const float      far = sqrtf((c->eye[0] - at[0]) * (c->eye[0] - at[0]) + (c->eye[1] - at[1]) * (c->eye[1] - at[1]) + (c->eye[2] - at[2]) * (c->eye[2] - at[2]));
            float            p[2][3];
            for (k = 0; k < 3; ++k) {
                p[0][k] = at[k] + 0.12f / far * (c->eye[k] - at[k]);
                p[1][k] = p[0][k] + (float)(length * q->frame[k]);
            }
            canvas_line(c, p[0], p[1], 0.004f, CANVAS_RGB(245, 245, 245));
            canvas_dot(c, p[0], 0.008f, CANVAS_RGB(250, 225, 70));
        }
        for (i = 0; i < (a->scene == 1u ? 12 : 0); ++i) {
            /* a puck's way so far, and in white where it should stop */
            const CfBody *b = &s->body[1 + i];
            const float   now[3] = {(float)b->p[0] + shift, 0.004f, (float)b->p[2]}, from[3] = {a->from[w][i][0] + shift, 0.004f, a->from[w][i][2]};
            double        stop[2][2];
            float         mark[2][3];
            puck_stop(w, 2.5, 0.5235987755982988 * i, s->gravity, stop[0]);
            puck_stop(w, 2.5, 0.5235987755982988 * (i + 1), s->gravity, stop[1]);
            for (k = 0; k < 2; ++k) {
                mark[k][0] = a->from[w][(i + k) % 12][0] + (float)stop[k][0] + shift;
                mark[k][1] = 0.006f;
                mark[k][2] = a->from[w][(i + k) % 12][2] + (float)stop[k][1];
            }
            canvas_line(c, from, now, 0.012f, k_colour[w]);
            canvas_line(c, mark[0], mark[1], 0.012f, CANVAS_RGB(240, 240, 240));
        }
    }

    for (w = 0; w < (a->scene == 0u ? WORLDS : 0); ++w) {
        /* the board: 3.6 m wide for the iterations, 2.4 m high for a residual of 1 down to 1e-6 of the heavy block's
         * weight for a step */
        const float z = -1.4f, corner[4][3] = {{-1.8f, 0.1f, z}, {1.8f, 0.1f, z}, {1.8f, 2.5f, z}, {-1.8f, 2.5f, z}};
        for (i = 0; i < 2 && w == 0; ++i) canvas_triangle(c, corner[0], corner[1 + i], corner[2 + i], CANVAS_RGB(28, 31, 40));
        for (i = 0; i <= 6 && w == 0; ++i) {
            const float y = 0.1f + 2.4f * (float)i / 6.0f, line[2][3] = {{-1.8f, y, z + 0.005f}, {1.8f, y, z + 0.005f}};
            canvas_line(c, line[0], line[1], 0.004f, CANVAS_RGB(60, 66, 80));
        }
        for (i = 0; i + 1 < CURVE; ++i) {
            float p[2][3];
            for (k = 0; k < 2; ++k) {
                const double value = fmin(0.0, fmax(-6.0, log10(a->curve[w][i + k] / (1600.0 * 9.81 / 60.0) + 1e-300)));
                p[k][0] = -1.8f + 3.6f * (float)(i + k) / (float)(CURVE - 1);
                p[k][1] = 0.1f + 2.4f * (float)(value + 6.0) / 6.0f;
                p[k][2] = z + 0.01f + 0.004f * (float)w;
            }
            canvas_line(c, p[0], p[1], 0.03f, k_colour[w]);
        }
        if (w == 0) {
            /* where a step stops */
            const float x = -1.8f + 3.6f * (float)(a->iterations - 1u) / (float)(CURVE - 1), mark[2][3] = {{x, 0.1f, z + 0.008f}, {x, 2.5f, z + 0.008f}};
            canvas_line(c, mark[0], mark[1], 0.012f, CANVAS_RGB(235, 235, 235));
        }
    }
    canvas_frame(d, c);
}

static void app_init(App *a) {
    canvas_open(&a->demo, &a->canvas, 300000u, CANVAS_RGB(38, 42, 52));
    build(a);
    if (a->demo.headless && !a->demo.frames) a->demo.frames = 2u;
}

/* The distance from point p to box b; 0 inside. */
static double box_distance(const CfBody *b, const double p[3]) {
    double sum = 0.0;
    int    k;
    for (k = 0; k < 3; ++k) {
        const double local = b->r[k] * (p[0] - b->p[0]) + b->r[3 + k] * (p[1] - b->p[1]) + b->r[6 + k] * (p[2] - b->p[2]);
        const double out = fmax(0.0, fabs(local) - b->half[k]);
        sum += out * out;
    }
    return sqrt(sum);
}

/* How deep the deepest of 65 points on each edge of box a lies inside box b; 0 if none does. Two convex
 * polyhedra overlap exactly when an edge of one goes through the other. */
static double pierce(const CfBody *a, const CfBody *b) {
    double deepest = 0.0;
    int    edge, i, k;
    for (edge = 0; edge < 12; ++edge) {
        const int axis = edge / 4, u = (axis + 1) % 3, v = (axis + 2) % 3;
        for (i = 0; i <= 64; ++i) {
            double local[3], p[3], inside = 1e300;
            local[axis] = a->half[axis] * ((double)i / 32.0 - 1.0);
            local[u]    = edge & 1 ? a->half[u] : -a->half[u];
            local[v]    = edge & 2 ? a->half[v] : -a->half[v];
            for (k = 0; k < 3; ++k) p[k] = a->p[k] + a->r[3 * k] * local[0] + a->r[3 * k + 1] * local[1] + a->r[3 * k + 2] * local[2];
            for (k = 0; k < 3; ++k) {
                const double in_b = b->r[k] * (p[0] - b->p[0]) + b->r[3 + k] * (p[1] - b->p[1]) + b->r[6 + k] * (p[2] - b->p[2]);
                inside = fmin(inside, b->half[k] - fabs(in_b));
            }
            deepest = fmax(deepest, inside);
        }
    }
    return deepest;
}

static void app_check(App *a) {
    static const char *const k_method[3] = {"Jacobi", "Gauss-Seidel", "NNCG"};
    static const char *const k_set[2] = {"box", "ellipse"};
    Demo    *d = &a->demo;
    uint32_t rng = 7u;
    int      i, j, k, trial, w, set, method;
    char     label[160];

    /* 1. Section 2.1: sphere against sphere and against a box. */
    {
        double worst_gap = 0.0, worst_normal = 0.0, worst_box = -1e300, worst_deep = 0.0, worst_on = 0.0, slack = 0.0;
        int    found = 0;
        for (trial = 0; trial < 400; ++trial) {
            Cf          *c = cf_open();
            const double radius[3] = {0.1 + 0.2 * fabs(noise(&rng)), 0.0, 0.0}, half[3] = {0.1 + 0.3 * fabs(noise(&rng)), 0.1 + 0.3 * fabs(noise(&rng)), 0.1 + 0.3 * fabs(noise(&rng))};
            const double at[3] = {0.5 * noise(&rng), 0.5 * noise(&rng), 0.5 * noise(&rng)}, origin[3] = {0, 0, 0};
            CfContact    q[8];
            CfBody      *ball, *other, *box;
            double       distance = 1e300, local[3], least = 1e300;
            cf_add(c, CF_SPHERE, radius, 1000.0, at);
            cf_add(c, CF_SPHERE, half, 1000.0, origin);
            cf_add(c, CF_BOX, half, 1000.0, origin);
            ball = &c->body[0]; other = &c->body[1]; box = &c->body[2];
            for (k = 0; k < 4; ++k) box->q[k] = noise(&rng);
            cf_place(box);
            found += cf_collide(ball, other, 1e9, q);
            worst_gap    = fmax(worst_gap, fabs(q[0].gap - (sqrt(at[0] * at[0] + at[1] * at[1] + at[2] * at[2]) - radius[0] - half[0])));
            worst_normal = fmax(worst_normal, fabs(q[0].frame[0] * at[0] + q[0].frame[1] * at[1] + q[0].frame[2] * at[2] + sqrt(at[0] * at[0] + at[1] * at[1] + at[2] * at[2])));
            found += cf_collide(ball, box, 1e9, q);
            /* outside: the contact point is on the box, gap + r from the centre, and none of 41 x 41 points on each
             * face is nearer; inside: the notes' depth to the nearest face, and here the radius more */
            for (k = 0; k < 3; ++k) {
                local[k] = box->r[k] * at[0] + box->r[3 + k] * at[1] + box->r[6 + k] * at[2];
                least    = fmin(least, box->half[k] - fabs(local[k]));
            }
            for (i = 0; i < 6 * 41 * 41 && least < 0.0; ++i) {
                const int    axis = i / (2 * 41 * 41), u = (axis + 1) % 3, v = (axis + 2) % 3;
                double       p[3];
                p[axis] = (i / (41 * 41)) % 2 ? box->half[axis] : -box->half[axis];
                p[u]    = box->half[u] * ((double)(i % 41) / 20.0 - 1.0);
                p[v]    = box->half[v] * ((double)(i / 41 % 41) / 20.0 - 1.0);
                distance = fmin(distance, sqrt((p[0] - local[0]) * (p[0] - local[0]) + (p[1] - local[1]) * (p[1] - local[1]) + (p[2] - local[2]) * (p[2] - local[2])));
            }
            if (least < 0.0) {
                const double from[3] = {q[0].point[0] - at[0], q[0].point[1] - at[1], q[0].point[2] - at[2]};
                double       edge = 1e300;
                for (k = 0; k < 3; ++k) edge = fmin(edge, box->half[k] - fabs(box->r[k] * q[0].point[0] + box->r[3 + k] * q[0].point[1] + box->r[6 + k] * q[0].point[2]));
                worst_on  = fmax(worst_on, fmax(fabs(edge), fabs(sqrt(from[0] * from[0] + from[1] * from[1] + from[2] * from[2]) - q[0].gap - radius[0])));
                worst_box = fmax(worst_box, q[0].gap + radius[0] - distance);
                slack     = fmax(slack, distance - q[0].gap - radius[0]);
            }
            if (least >= 0.0) worst_deep = fmax(worst_deep, fabs(q[0].gap + least + radius[0]));
            cf_close(c);
        }
        demo_check(d, "400 spheres against a sphere and a box, no limit on the gap: contacts", found, "= 800", found == 800);
        demo_check(d, "sphere - sphere: largest error of the gap |c2 - c1| - r1 - r2", worst_gap, "< 1e-12", worst_gap < 1e-12);
        demo_check(d, "    largest error of the normal against the line of centres", worst_normal, "< 1e-12", worst_normal < 1e-12);
        demo_check(d, "sphere - box, centre outside: the contact point is off the box's surface, or off gap + r from the centre, by", worst_on, "< 1e-12", worst_on < 1e-12);
        demo_check(d, "    most that one of 10086 points on the box is nearer the centre than gap + r", worst_box, "< 1e-12", worst_box < 1e-12);
        demo_check(d, "    most that the nearest of them is farther", slack, "< 0.01", slack < 0.01);
        demo_check(d, "    centre inside: gap + (depth to the nearest face + r)", worst_deep, "< 1e-12", worst_deep < 1e-12);
    }

    /* 2. Box against box: the 15 axes decide the same as edges that pierce; moved apart by the overlap they no longer touch. */
    {
        int    overlaps = 0, apart = 0, wrong_apart = 0, missed = 0, still = 0, edges = 0, most = 0;
        double worst_point = 0.0, worst_unit = 0.0, worst_side = 1e300, worst_left = 0.0;
        for (trial = 0; trial < 3000; ++trial) {
            Cf          *c = cf_open();
            const double half_a[3] = {0.1 + 0.3 * fabs(noise(&rng)), 0.1 + 0.3 * fabs(noise(&rng)), 0.1 + 0.3 * fabs(noise(&rng))};
            const double half_b[3] = {0.1 + 0.3 * fabs(noise(&rng)), 0.1 + 0.3 * fabs(noise(&rng)), 0.1 + 0.3 * fabs(noise(&rng))};
            const double at[3] = {0.5 * noise(&rng), 0.5 * noise(&rng), 0.5 * noise(&rng)}, origin[3] = {0, 0, 0};
            CfContact    q[8];
            CfBody      *box_a, *box_b;
            double       deepest = 0.0, inside;
            int          count;
            cf_add(c, CF_BOX, half_a, 1000.0, origin);
            cf_add(c, CF_BOX, half_b, 1000.0, at);
            box_a = &c->body[0]; box_b = &c->body[1];
            for (k = 0; k < 4; ++k) { box_a->q[k] = noise(&rng); box_b->q[k] = noise(&rng); }
            cf_place(box_a); cf_place(box_b);
            count  = cf_collide(box_a, box_b, 0.0, q);
            inside = fmax(pierce(box_a, box_b), pierce(box_b, box_a));
            for (i = 0; i < count; ++i) {
                const double *n = q[i].frame;
                deepest     = fmin(deepest, q[i].gap);
                worst_unit  = fmax(worst_unit, fabs(n[0] * n[0] + n[1] * n[1] + n[2] * n[2] - 1.0));
                worst_side  = fmin(worst_side, n[0] * at[0] + n[1] * at[1] + n[2] * at[2]);
                worst_point = fmax(worst_point, fmax(box_distance(box_a, q[i].point), box_distance(box_b, q[i].point)) + q[i].gap);
            }
            most = count > most ? count : most;
            edges += count == 1;
            if (count == 0) { apart++; wrong_apart += inside > 0.0; }
            if (count > 0) {
                overlaps++;
                missed += deepest < -0.02 && inside == 0.0;
                /* b moved along the normal by the overlap and a micrometre */
                for (k = 0; k < 3; ++k) box_b->p[k] += (1e-6 - deepest) * q[0].frame[k];
                still += fmax(pierce(box_a, box_b), pierce(box_b, box_a)) > 0.0;
                count = cf_collide(box_a, box_b, 1e-3, q);
                for (i = 0; i < count; ++i) worst_left = fmax(worst_left, -q[i].gap);
            }
            cf_close(c);
        }
        snprintf(label, sizeof label, "box - box: of 3000 random pairs %d overlap; of the %d others, those with an edge inside the other", overlaps, apart);
        demo_check(d, label, wrong_apart, "= 0", wrong_apart == 0);
        demo_check(d, "    overlaps deeper than 2 cm with no edge point inside the other", missed, "= 0", missed == 0);
        demo_check(d, "    moved apart along the normal by the overlap: pairs with an edge point still inside", still, "= 0", still == 0);
        demo_check(d, "        largest overlap the 15 axes still report", worst_left, "< 1e-9", worst_left < 1e-9);
        demo_check(d, "    largest | |n| - 1 |", worst_unit, "< 1e-12", worst_unit < 1e-12);
        demo_check(d, "    least n . (centre of b - centre of a)", worst_side, "> 0", worst_side > 0.0);
        demo_check(d, "    largest (distance of a contact point from either box) - overlap", worst_point, "< 1e-9", worst_point < 1e-9);
        demo_check(d, "    pairs with one contact (edge against edge, or one corner)", edges, "(reported)", true);
        demo_check(d, "    most contacts of a pair", most, "<= 8", most <= 8);
    }
    {
        /* a box flat on a larger one: the four corners; turned by 45 degrees on one of its own size: an octagon */
        Cf          *c = cf_open();
        const double half[3] = {0.2, 0.1, 0.2}, wide[3] = {0.5, 0.1, 0.5}, low[3] = {0, 0, 0}, high[3] = {0.05, 0.199, -0.1};
        CfContact    q[8];
        double       spread = 0.0, corner = 0.0;
        int          count;
        cf_add(c, CF_BOX, wide, 0.0, low);
        cf_add(c, CF_BOX, half, 1000.0, high);
        count = cf_collide(&c->body[0], &c->body[1], 0.01, q);
        for (i = 0; i < count; ++i) {
            spread = fmax(spread, fabs(q[i].gap + 0.001));
            corner = fmax(corner, fmax(fabs(fabs(q[i].point[0] - 0.05) - 0.2), fabs(fabs(q[i].point[2] + 0.1) - 0.2)));
        }
        demo_check(d, "a box 1 mm into a wider one: contacts", count, "= 4", count == 4);
        demo_check(d, "    largest |gap + 1 mm|", spread, "< 1e-12", spread < 1e-12);
        demo_check(d, "    farthest a contact is from a corner of the upper box", corner, "< 1e-12", corner < 1e-12);
        memcpy(c->body[0].half, half, sizeof half);
        c->body[1].p[0] = c->body[1].p[2] = 0.0;
        c->body[1].q[0] = cos(0.39269908169872414); c->body[1].q[2] = sin(0.39269908169872414);
        cf_place(&c->body[1]);
        count = cf_collide(&c->body[0], &c->body[1], 0.01, q);
        demo_check(d, "the same box turned 45 degrees on one of its own size: contacts", count, "= 8", count == 8);
        cf_close(c);
    }

    /* 3. Algorithm 13 against a search over the surface. */
    {
        double worst = 0.0, worst_in = 0.0, worst_flat = 0.0;
        for (trial = 0; trial < 300; ++trial) {
            const double half[3] = {0.2 + fabs(noise(&rng)), 0.2 + fabs(noise(&rng)), trial % 5 ? 0.2 + fabs(noise(&rng)) : 0.0};
            const double z[3] = {2.0 * noise(&rng), 2.0 * noise(&rng), 2.0 * noise(&rng)};
            const bool   in = half[2] > 0.0 && z[0] * z[0] / (half[0] * half[0]) + z[1] * z[1] / (half[1] * half[1]) + z[2] * z[2] / (half[2] * half[2]) <= 1.0;
            double       x[3] = {z[0], z[1], z[2]}, best = 1e300, centre[2] = {1.5707963267948966, 3.141592653589793}, span = 3.141592653589793, found, on = 0.0;
            int          level;
            cf_ellipsoid(half, x);
            found = sqrt((x[0] - z[0]) * (x[0] - z[0]) + (x[1] - z[1]) * (x[1] - z[1]) + (x[2] - z[2]) * (x[2] - z[2]));
            /* the nearest surface point by a 41 x 41 grid of angles, eight times around the best with a third of the span;
             * a flat ellipsoid is the ellipse in the plane z = 0 with its inside, which the grid's inner points cover */
            for (level = 0; level < 12; ++level) {
                double at[2] = {centre[0], centre[1]};
                for (i = 0; i < 41 * 41; ++i) {
                    const double t = centre[0] + span * ((double)(i % 41) / 40.0 - 0.5), f = centre[1] + 2.0 * span * ((double)(i / 41) / 40.0 - 0.5);
                    const double p[3] = {half[0] * sin(t) * cos(f), half[1] * sin(t) * sin(f), half[2] * cos(t)};
                    const double distance = sqrt((p[0] - z[0]) * (p[0] - z[0]) + (p[1] - z[1]) * (p[1] - z[1]) + (p[2] - z[2]) * (p[2] - z[2]));
                    if (distance < best) { best = distance; at[0] = t; at[1] = f; }
                }
                centre[0] = at[0]; centre[1] = at[1]; span /= 3.0;
            }
            for (k = 0; k < 3; ++k) on += half[k] > 0.0 ? x[k] * x[k] / (half[k] * half[k]) : 0.0;
            if (in) worst_in = fmax(worst_in, found);
            if (!in && half[2] > 0.0) worst = fmax(worst, fabs(found - best));
            if (half[2] == 0.0) worst_flat = fmax(worst_flat, fmax(fabs(found - best), fmax(fabs(x[2]), on - 1.0)));
        }
        demo_check(d, "closest point of an ellipsoid (algorithm 13): largest |distance - searched distance|, 300 points", worst, "< 1e-6", worst < 1e-6);
        demo_check(d, "    a point inside is moved by", worst_in, "< 1e-15", worst_in < 1e-15);
        demo_check(d, "    one half axis 0 (no friction against spinning): the same, and the point is in the ellipse", worst_flat, "< 1e-6", worst_flat < 1e-6);
    }

    /* 4. Twelve bodies in the pit after 2 s: each solver's impulses against the contact conditions, read off the bodies' velocities. */
    for (set = 0; set < 2; ++set) {
        for (method = 0; method < 3; ++method) {
            Cf    *c;
            double out[5];
            a->scene = 3u; a->set = (uint32_t)set; a->method = CF_NNCG; a->iterations = 40u; a->bodies = 12u; build(a);
            c = a->world[0];
            for (i = 0; i < 120; ++i) cf_step(c);
            /* every third body is pushed sideways and spun so that contacts slide */
            for (i = 5; i < c->bodies; i += 3) { c->body[i].v[0] += 1.0; c->body[i].w[1] += 20.0; }
            c->method = method; c->iterations = 20000; c->tolerance = 1e-13; c->warm = false;
            cf_detect(c); cf_problem(c); cf_solve(c, NULL); cf_apply(c);
            /* the conditions from the bodies' velocities alone. out: 0 the most a contact closes faster than its gap
             * allows, 1 the largest lambda_n times the speed it opens with beyond that, 2 the most a friction impulse is
             * outside its set, 3 for sliding contacts the largest difference between the friction and the one that
             * takes out most energy, 4 how many contacts slide; as speeds or as shares of lambda_n */
            out[0] = out[1] = out[2] = out[3] = out[4] = 0.0;
            for (i = 0; i < c->contacts; ++i) {
                const CfContact *q = &c->contact[i];
                const CfBody    *lower = &c->body[q->a], *upper = &c->body[q->b];
                const double    *l = &c->lambda[4 * i], *n = q->frame;
                const double     mu[3] = {q->mu[0], q->mu[1], q->mu[2] > 0.0 ? 1.0 : 0.0};
                double           v[3], slide[3], want[3], size = 0.0, in = 0.0;
                for (k = 0; k < 3; ++k) {
                    const int    u = (k + 1) % 3, t = (k + 2) % 3;
                    const double ra[3] = {q->point[0] - lower->p[0], q->point[1] - lower->p[1], q->point[2] - lower->p[2]};
                    const double rb[3] = {q->point[0] - upper->p[0], q->point[1] - upper->p[1], q->point[2] - upper->p[2]};
                    v[k] = upper->v[k] + upper->w[u] * rb[t] - upper->w[t] * rb[u] - lower->v[k] - lower->w[u] * ra[t] + lower->w[t] * ra[u];
                }
                slide[0] = v[0] * q->frame[3] + v[1] * q->frame[4] + v[2] * q->frame[5];
                slide[1] = v[0] * q->frame[6] + v[1] * q->frame[7] + v[2] * q->frame[8];
                slide[2] = q->mu[2] * ((upper->w[0] - lower->w[0]) * n[0] + (upper->w[1] - lower->w[1]) * n[1] + (upper->w[2] - lower->w[2]) * n[2]);
                {
                    const double open = v[0] * n[0] + v[1] * n[1] + v[2] * n[2] + fmax(0.0, q->gap) / c->dt;
                    out[0] = fmax(out[0], -open);
                    out[1] = fmax(out[1], l[0] * fabs(open) / (c->gravity * c->dt));
                }
                for (k = 0; k < 3; ++k) {
                    size += mu[k] * mu[k] * slide[k] * slide[k];
                    in   += mu[k] > 0.0 ? l[1 + k] * l[1 + k] / (mu[k] * mu[k]) : 0.0;
                    if (c->set == CF_SET_BOX) out[2] = fmax(out[2], fabs(l[1 + k]) - mu[k] * l[0]);
                }
                if (c->set == CF_SET_ELLIPSE) out[2] = fmax(out[2], sqrt(in) - l[0]);
                for (k = 0; k < 3; ++k) {
                    /* box: each direction that slides is at its bound against the sliding. Ellipse: -lambda_n mu^2 v / |mu v| */
                    want[k] = c->set == CF_SET_BOX ? (slide[k] > 0.0 ? -mu[k] * l[0] : mu[k] * l[0]) : -l[0] * mu[k] * mu[k] * slide[k] / sqrt(size);
                    if (l[0] > 1e-9 && (c->set == CF_SET_BOX ? fabs(slide[k]) : sqrt(size)) > 1e-4) out[3] = fmax(out[3], fabs(l[1 + k] - want[k]) / l[0]);
                }
                out[4] += l[0] > 1e-9 && sqrt(slide[0] * slide[0] + slide[1] * slide[1]) > 1e-4;
            }
            snprintf(label, sizeof label, "pit, %d contacts, %s, %s: iterations used", c->contacts, k_set[set], k_method[method]);
            demo_check(d, label, c->used, "(reported)", true);
            demo_check(d, "    residual (cf_merit) over the weight of a 1 kg body for a step", cf_merit(c) / (c->gravity * c->dt), "(reported)", true);
            demo_check(d, "    contacts that carry an impulse and slide", out[4], "> 3", out[4] > 3.0);
            demo_check(d, "    fastest a contact closes beyond its gap (m/s)", out[0], "< 1e-6", out[0] < 1e-6);
            demo_check(d, "    largest lambda_n x (speed it opens with), same unit as the residual", out[1], "< 1e-6", out[1] < 1e-6);
            demo_check(d, "    most a friction impulse is outside its set", out[2], "< 1e-9", out[2] < 1e-9);
            demo_check(d, "    sliding contacts: largest |friction - the one that takes out most energy| / lambda_n", out[3], "< 1e-4", out[3] < 1e-4);
        }
    }
    /* 5. Friction against closed forms. Pucks: every one stops where its friction set says (the white rings). */
    {
        static const char *const k_ground[3] = {"box, mu 0.5", "cone, mu 0.5", "ellipse, mu 0.25 along x and 0.75 along z"};
        a->scene = 1u; a->iterations = 40u; build(a);
        for (i = 0; i < 150; ++i) { for (w = 0; w < 3; ++w) cf_step(a->world[w]); }
        for (w = 0; w < 3; ++w) {
            const Cf *c = a->world[w];
            double    worst = 0.0, moving = 0.0, way[3] = {0, 0, 0};
            for (i = 0; i < 12; ++i) {
                const CfBody *b = &c->body[1 + i];
                double        stop[2];
                puck_stop(w, 2.5, 0.5235987755982988 * i, c->gravity, stop);
                worst  = fmax(worst, hypot(b->p[0] - a->from[w][i][0] - stop[0], b->p[2] - a->from[w][i][2] - stop[1]));
                moving = fmax(moving, sqrt(b->v[0] * b->v[0] + b->v[1] * b->v[1] + b->v[2] * b->v[2]));
                if (i < 4 && i != 2) way[i == 1 ? 2 : i / 3] = hypot(b->p[0] - a->from[w][i][0], b->p[2] - a->from[w][i][2]);   /* i = 0: along x; 3: along z; 1: 30 degrees */
            }
            snprintf(label, sizeof label, "12 pucks at 2.5 m/s on the %s: farthest from where it should stop (m)", k_ground[w]);
            demo_check(d, label, worst, "< 0.01", worst < 0.01);   /* the ellipse's 7 mm fall to 1.7 mm with a step of 1/240 s */
            demo_check(d, "    fastest after 2.5 s (m/s)", moving, "< 1e-6", moving < 1e-6);
            demo_check(d, "    way along x (m); v^2 / (2 mu g) is 0.637 for mu 0.5, 1.274 for 0.25", way[0], "(reported)", true);
            demo_check(d, "    way along z (m); 0.425 for mu 0.75", way[1], "(reported)", true);
            demo_check(d, "    way at 30 degrees (m)", way[2], "(reported)", true);
        }
        {
            /* the box holds Coulomb's cone: on the diagonal it has sqrt(2) times the friction */
            double diagonal[2] = {0, 0};
            for (i = 0; i < 2; ++i) {
                Cf          *c = world_open();
                const double half[3] = {0.07, 0.025, 0.07}, at[3] = {0, 0.025, 0};
                cf_add(c, CF_BOX, half, 1000.0, at);
                c->set = i;
                c->body[1].v[0] = c->body[1].v[2] = 2.5 * 0.7071067811865476;
                for (j = 0; j < 150; ++j) cf_step(c);
                diagonal[i] = hypot(c->body[1].p[0], c->body[1].p[2]);
                cf_close(c);
            }
            demo_check(d, "a puck at 45 degrees: way on the box / way on the cone (1 / sqrt 2 = 0.7071)", diagonal[0] / diagonal[1], "within 0.005", fabs(diagonal[0] / diagonal[1] - 0.7071067811865476) < 0.005);
        }
    }
    {
        /* the ramp of 20 degrees after 1 s: a box slides with g (sin - mu cos) or holds; a ball rolls with 5/7 g sin */
        static const double k_mu[3] = {0.2, 0.3, 0.5};
        const double        slope = 0.3490658503988659;
        Cf                 *c;
        double              start[7][3];
        for (set = 0; set < 2; ++set) {
            a->scene = 2u; a->set = (uint32_t)set; a->method = CF_NNCG; a->iterations = 40u; build(a);
            c = a->world[0];
            for (i = 0; i < 7; ++i) memcpy(start[i], c->body[2 + i].p, sizeof start[i]);
            for (i = 0; i < 60; ++i) cf_step(c);
            for (i = 0; i < 4; ++i) {
                const CfBody *b = &c->body[2 + i];
                const double  way = (b->p[0] - start[i][0]) * cos(slope) - (b->p[1] - start[i][1]) * sin(slope);
                const double  rate = i < 3 ? fmax(0.0, c->gravity * (sin(slope) - k_mu[i] * cos(slope))) : 5.0 / 7.0 * c->gravity * sin(slope);
                if (i < 3) snprintf(label, sizeof label, "ramp of 20 degrees (tan = 0.364), %s: box with mu %.1f, way down in 1 s less a t^2 / 2 = %.4f m", k_set[set], k_mu[i], 0.5 * rate);
                if (i == 3) snprintf(label, sizeof label, "    ball, mu 1: way down less (5/7) g sin t^2 / 2 = %.4f m", 0.5 * rate);
                demo_check(d, label, way - 0.5 * rate, "within 0.005", fabs(way - 0.5 * rate) < 0.005);
            }
            /* the three balls on the ground turn at 30 rad/s; friction against spinning mu_tau slows them by mu_tau g / (2/5 r^2) */
            for (i = 0; i < 3; ++i) {
                static const double k_spin[3] = {0.0, 0.005, 0.02};
                const CfBody       *b = &c->body[6 + i];
                const double        want = fmax(0.0, 30.0 - k_spin[i] * c->gravity / (0.4 * 0.15 * 0.15));
                snprintf(label, sizeof label, "    ball that spins at 30 rad/s, mu_tau %.3f m: spin after 1 s less %.3f", k_spin[i], want);
                demo_check(d, label, b->w[1] - want, "within 0.05", fabs(b->w[1] - want) < 0.05);
                demo_check(d, "        it has moved by (m)", hypot(b->p[0] - start[4 + i][0], b->p[2] - start[4 + i][2]), "< 1e-6", hypot(b->p[0] - start[4 + i][0], b->p[2] - start[4 + i][2]) < 1e-6);
            }
        }
    }
    {
        /* a ball that slides at 2 m/s ends rolling at 5/7 of that */
        Cf          *c = world_open();
        const double radius[3] = {0.2, 0.2, 0.2}, at[3] = {0, 0.2, 0};
        cf_add(c, CF_SPHERE, radius, 1000.0, at);
        c->body[1].v[0] = 2.0;
        for (i = 0; i < 120; ++i) cf_step(c);
        demo_check(d, "a ball set sliding at 2 m/s: speed once it rolls, over 2 (5/7 = 0.714286)", c->body[1].v[0] / 2.0, "within 1e-4", fabs(c->body[1].v[0] / 2.0 - 5.0 / 7.0) < 1e-4);
        demo_check(d, "    its speed + radius x spin (0 when it rolls)", c->body[1].v[0] + 0.2 * c->body[1].w[2], "within 1e-9", fabs(c->body[1].v[0] + 0.2 * c->body[1].w[2]) < 1e-9);
        cf_close(c);
    }

    /* 6. The solvers. One contact problem: a tower of three boxes at rest, from no impulses. */
    {
        Cf    *c = tower(3, 1.0);
        double reference[4 * 64], *matrix, worst = 0.0, velocity[3][6 * 4], spread = 0.0, curve[3][CURVE];
        int    rows, sweep;
        for (i = 0; i < 60; ++i) cf_step(c);
        c->warm = false;
        cf_detect(c);
        /* eq. 147 with the bounds of eq. 158: with A written out, lambda_i <- clamp(lambda_i - (A lambda + b)_i / A_ii),
         * 25 sweeps; against Gauss-Seidel on the box with r = 1 / A_ii */
        c->set = CF_SET_BOX; c->method = CF_GAUSS_SEIDEL; c->iterations = 25;
        cf_problem(c);
        rows   = 4 * c->contacts;
        MU_ASSERT(rows <= 4 * 64);
        matrix = calloc((size_t)rows * (size_t)rows, sizeof(double));
        for (i = 0; i < rows; ++i) {
            for (j = 0; j < rows; ++j) {
                const int body_i[2] = {c->contact[i / 4].a, c->contact[i / 4].b}, body_j[2] = {c->contact[j / 4].a, c->contact[j / 4].b};
                int       s, t;
                for (s = 0; s < 2; ++s) {
                    for (t = 0; t < 2; ++t) {
                        for (k = 0; k < 6 && body_i[s] == body_j[t]; ++k) matrix[i * rows + j] += c->row[12 * i + 6 * s + k] * c->back[12 * j + 6 * t + k];
                    }
                }
            }
        }
        memset(reference, 0, sizeof reference);
        for (sweep = 0; sweep < 25; ++sweep) {
            for (i = 0; i < rows; ++i) {
                const double bound = i % 4 ? c->contact[i / 4].mu[i % 4 - 1] * reference[i - i % 4] : 1e300;
                double       speed = c->bias[i];
                for (j = 0; j < rows; ++j) speed += matrix[i * rows + j] * reference[j];
                if (matrix[i * rows + i] > 0.0) reference[i] = fmax(i % 4 ? -bound : 0.0, fmin(bound, reference[i] - speed / matrix[i * rows + i]));
            }
        }
        cf_solve(c, NULL);
        for (i = 0; i < rows; ++i) worst = fmax(worst, fabs(c->lambda[i] - reference[i]));
        snprintf(label, sizeof label, "tower of 3, %d contacts: Gauss-Seidel on the box, r = 1 / A_ii, against projected Gauss-Seidel on A (eq. 147, 158): largest difference", c->contacts);
        demo_check(d, label, worst / (32.0 * c->gravity * c->dt), "< 1e-12", worst / (32.0 * c->gravity * c->dt) < 1e-12);
        free(matrix);

        /* r does not change the solution (section 3.4.4): the bodies' velocities after 3000 iterations */
        for (set = 0; set < 2; ++set) {
            for (trial = 0; trial < 3; ++trial) {
                static const double k_omega[3] = {0.5, 1.0, 1.5};
                c->set = set; c->omega = k_omega[trial]; c->iterations = 3000;
                cf_problem(c); cf_solve(c, NULL);
                for (i = 0; i < 6 * 4; ++i) velocity[trial][i] = c->push[i];
                for (i = 0; i < 6 * 4; ++i) spread = fmax(spread, fabs(velocity[trial][i] - velocity[0][i]));
            }
        }
        demo_check(d, "    r = 0.5, 1 and 1.5 / A_ii, both sets, 3000 iterations: largest difference of a body's velocity", spread, "< 1e-9", spread < 1e-9);
        c->omega = 1.0;
        cf_close(c);

        /* the heavy tower of scene 0: residual of each solver over the iterations, from no impulses */
        c = tower(6, 50.0);
        c->method = CF_NNCG;
        for (i = 0; i < 60; ++i) cf_step(c);
        c->warm = false;
        cf_detect(c);
        for (method = 0; method < 3; ++method) {
            c->method = method; c->iterations = CURVE;
            cf_problem(c); cf_solve(c, curve[method]);
            for (i = 0; i < 3; ++i) {
                static const int k_at[3] = {10, 40, CURVE};
                if (i == 0) snprintf(label, sizeof label, "tower of 6 with the top box 50 times heavier, %s: residual / the top box's weight for a step, after %d iterations", k_method[method], k_at[i]);
                if (i) snprintf(label, sizeof label, "    after %d", k_at[i]);
                demo_check(d, label, curve[method][k_at[i] - 1] / (1600.0 * c->gravity * c->dt), "(reported)", true);
            }
        }
        demo_check(d, "    after 40 iterations: NNCG / Gauss-Seidel (the notes: NNCG copes better with large mass ratios)", curve[2][39] / curve[1][39], "< 1", curve[2][39] < curve[1][39]);
        demo_check(d, "    after 40 iterations: Gauss-Seidel / Jacobi", curve[1][39] / curve[0][39], "< 1", curve[1][39] < curve[0][39]);
        demo_check(d, "    after 200 iterations: NNCG / Gauss-Seidel", curve[2][CURVE - 1] / curve[1][CURVE - 1], "< 1", curve[2][CURVE - 1] < curve[1][CURVE - 1]);
        cf_close(c);
    }
    for (method = 0; method < 3; ++method) {
        /* the same tower stepped for 10 s with 40 iterations a step */
        Cf    *c = tower(6, 50.0);
        double lowest = 1e300;
        c->method = method;
        for (i = 0; i < 600; ++i) { cf_step(c); lowest = fmin(lowest, c->body[6].p[1]); }
        snprintf(label, sizeof label, "that tower for 10 s, 40 iterations a step, %s: height of the top box at the end (1.1 m at the start)", k_method[method]);
        demo_check(d, label, c->body[6].p[1], method == 2 ? "> 1.08" : method == 0 ? "< 0.5" : "(reported)", method == 2 ? c->body[6].p[1] > 1.08 : method == 0 ? c->body[6].p[1] < 0.5 : true);
        demo_check(d, "    its lowest height on the way", lowest, "(reported)", true);
        cf_close(c);
    }
    {
        /* warm start: the plain tower of 6 at rest, residual after 10 iterations from the last step's impulses and from none */
        Cf    *c = tower(6, 1.0);
        double curve[2][10];
        c->method = CF_NNCG;
        for (i = 0; i < 120; ++i) cf_step(c);
        c->method = CF_GAUSS_SEIDEL; c->iterations = 10;
        cf_detect(c);   /* the same contacts again: the bodies are where the last step looked for them, within 1e-13 */
        for (i = 0; i < 2; ++i) {
            c->warm = i == 0;
            cf_problem(c); cf_solve(c, curve[i]);
        }
        demo_check(d, "tower of 6 at rest, Gauss-Seidel: residual after 10 iterations from no impulses / a box's weight for a step", curve[1][9] / (32.0 * c->gravity * c->dt), "(reported)", true);
        demo_check(d, "    from the last step's impulses, over that", curve[0][9] / curve[1][9], "< 0.01", curve[0][9] < 0.01 * curve[1][9]);
        cf_close(c);
    }
    {
        /* section 3.4.4: an r four times too large for Jacobi; with the rollback it converges, without it does not */
        Cf    *c = tower(3, 1.0);
        double curve[2][CURVE];
        for (i = 0; i < 60; ++i) cf_step(c);
        c->warm = false; c->method = CF_JACOBI; c->omega = 4.0; c->iterations = CURVE;
        cf_detect(c);
        for (i = 0; i < 2; ++i) {
            c->adapt = i == 1;
            cf_problem(c); cf_solve(c, curve[i]);
            if (i == 0) snprintf(label, sizeof label, "tower of 3, Jacobi with r = 4 / (largest eigenvalue): residual after 200 iterations / a box's weight for a step");
            if (i) snprintf(label, sizeof label, "    with iterations that change more than the last undone and r halved");
            demo_check(d, label, curve[i][CURVE - 1] / (32.0 * c->gravity * c->dt), i ? "< 0.05" : "> 100", i ? curve[i][CURVE - 1] / (32.0 * c->gravity * c->dt) < 0.05 : curve[i][CURVE - 1] / (32.0 * c->gravity * c->dt) > 100.0);
        }
        demo_check(d, "    iterations undone", c->rollbacks, "> 0", c->rollbacks > 0);
        cf_close(c);
    }
    {
        /* the same rule where r was not too large: 48 bodies in the pit, Gauss-Seidel, 1000 iterations */
        Cf    *c;
        double merit[2];
        int    undone = 0;
        a->scene = 3u; a->set = CF_SET_ELLIPSE; a->method = CF_NNCG; a->iterations = 40u; a->bodies = 48u; build(a);
        c = a->world[0];
        for (i = 0; i < 120; ++i) cf_step(c);
        c->warm = false; c->method = CF_GAUSS_SEIDEL; c->iterations = 1000;
        cf_detect(c);
        for (i = 0; i < 2; ++i) {
            c->adapt = i == 1;
            cf_problem(c); cf_solve(c, NULL);
            merit[i] = cf_merit(c);
            undone   = c->rollbacks;
        }
        snprintf(label, sizeof label, "pit, %d contacts, Gauss-Seidel with r = 1 / A_ii, 1000 iterations: residual / a 1 kg body's weight for a step", c->contacts);
        demo_check(d, label, merit[0] / (c->gravity * c->dt), "(reported)", true);
        demo_check(d, "    with the rollback rule", merit[1] / (c->gravity * c->dt), "(reported)", true);
        demo_check(d, "    iterations undone", undone, "(reported)", true);
    }
    {
        /* section 3.2.3: two boxes, the order of the contacts in a sweep; 500 random orders, projected Gauss-Seidel on the box */
        Cf *c = tower(2, 1.0);
        int least = 1 << 30, most = 0, identity = 0;
        for (i = 0; i < 60; ++i) cf_step(c);
        c->warm = false; c->set = CF_SET_BOX; c->method = CF_GAUSS_SEIDEL; c->iterations = 5000; c->tolerance = 1e-8;
        cf_detect(c);
        for (trial = 0; trial < 500; ++trial) {
            cf_problem(c);
            for (i = c->contacts - 1; i > 0 && trial; --i) {
                const int other = (int)((noise(&rng) * 0.5 + 0.5) * (double)(i + 1)) % (i + 1), keep = c->order[i];
                c->order[i] = c->order[other]; c->order[other] = keep;
            }
            cf_solve(c, NULL);
            least    = c->used < least ? c->used : least;
            most     = c->used > most ? c->used : most;
            identity = trial ? identity : c->used;
        }
        snprintf(label, sizeof label, "two boxes, %d contacts, 500 orders of a sweep: fewest iterations to a change below 1e-8 of the largest impulse", c->contacts);
        demo_check(d, label, least, "(reported)", true);
        demo_check(d, "    most", most, "(reported)", true);
        demo_check(d, "    in the order the contacts were found", identity, "(reported)", true);
        demo_check(d, "    fewest / most (the notes: less than half for such a stack)", (double)least / (double)most, "< 1", least < most);
        cf_close(c);
    }
    {
        /* overlap: a box 2 cm in the ground. Section 3.4.6 moves it out; section 1.9 gives it speed to get out */
        for (i = 0; i < 2; ++i) {
            Cf          *c = world_open();
            const double half[3] = {0.2, 0.1, 0.2}, at[3] = {0, 0.08, 0};
            double       fastest = 0.0;
            cf_add(c, CF_BOX, half, 1000.0, at);
            c->project = i ? 0 : 10;
            for (j = 0; j < 120; ++j) { cf_step(c); fastest = fmax(fastest, c->body[1].v[1]); }
            snprintf(label, sizeof label, "a box 2 cm in the ground, %s: overlap after 2 s (m); 2 mm are left alone", i ? "a share of the overlap added to the speed (section 1.9)" : "moved out after the step (section 3.4.6)");
            demo_check(d, label, 0.1 - c->body[1].p[1], "within 1e-4 of 0.002", fabs(0.1 - c->body[1].p[1] - 0.002) < 1e-4);
            demo_check(d, "    fastest it rose (m/s)", fastest, i ? "> 0.1" : "< 1e-5", i ? fastest > 0.1 : fastest < 1e-5);
            cf_close(c);
        }
    }
}

int main(int argc, char **argv) {
    static App app;
    static const DemoOption options[] = {
        {"--scene", .u32 = &app.scene, .lo = 0, .hi = 3, .help = "0 heavy block on a tower, 1 pucks, 2 ramp and spin, 3 pit"},
        {"--iterations", .u32 = &app.iterations, .lo = 2, .hi = CURVE, .help = "solver iterations a step"},
        {"--set", .u32 = &app.set, .lo = 0, .hi = 1, .help = "friction: 0 box, 1 ellipse (not scene 1)"},
        {"--method", .u32 = &app.method, .lo = 0, .hi = 2, .help = "0 Jacobi, 1 Gauss-Seidel, 2 NNCG (scenes 2 and 3)"},
        {"--bodies", .u32 = &app.bodies, .lo = 1, .hi = 96, .help = "bodies dropped into the pit (scene 3)"},
        {"--adapt", .flag = &app.adapt, .help = "undo an iteration that changes more than the last and make r smaller (Jacobi, Gauss-Seidel)"},
        {"--skip", .u32 = &app.skip, .lo = 0, .hi = 900, .help = "steps of 1/60 s taken before the first picture"},
        {0}};
    Demo *d = &app.demo;
    app.iterations = 40u;
    app.bodies     = 48u;
    app.method     = CF_NNCG;
    app.set        = CF_SET_ELLIPSE;
    demo_defaults(d, "contact", "Contact and Friction Simulation");
    demo_open(d, options, argc, argv);
    app_init(&app);
    while (demo_running(d)) {
        if (d->ui) app_ui(&app, d->ui);
        app_frame(&app);
    }
    if (d->check) app_check(&app);
    return demo_close(d);
}
