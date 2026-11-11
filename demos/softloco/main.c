/* softloco — Soft Body Locomotion: bodies with no skeleton that move by muscles.
 *
 *   Draw -> Present          (the method runs in C: method.c)
 *
 * Tan, Turk, Liu: "Soft Body Locomotion" (SIGGRAPH 2012). Each scene shows two copies of
 * one body with the same goals. The blue one chooses its muscle lengths together with what
 * every contact does (stay, slide, let go) by the paper's search; the orange one solves the
 * one programme in which whatever touches the ground stays where it is.
 *
 *   scene 0  a column on four feet whose head follows a point that circles
 *   scene 1  a flat body whose two ends are asked forward in turn, on a ground that
 *            holds ten times better against sliding backwards
 *   scene 2  a ring asked for angular momentum, one way and then the other
 *
 * --check tests that muscles are internal forces, the body alone, the programme against
 * steps taken with other lengths, every planned step against the contact conditions, the
 * search against one programme and against all 10^4 of them, and what each scene achieves.
 */
#include "canvas.h"
#include "method.h"
#include "softmesh.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MUSCLES_MOST 64

/* What a body has been asked and has done since the scene was built. */
typedef struct Run {
    uint32_t steps, unplanned, slid, let_go;
    long     solved;
    double   miss, miss_sum, miss_worst;   /* reach: head to target; crawl: the worse end to its target; m */
    double   violation;                    /* largest breach of the contact conditions in a planned step, in the plan's units */
    double   recount;                      /* largest |plan's objective - the same from the step taken| / (1 + objective) */
    double   volume_low, volume_high;      /* / volume at rest */
} Run;

typedef struct App {
    Demo      demo;   /* first: see demo.h */
    Canvas    canvas;
    DemoOrbit orbit;
    SoftMesh  mesh;                /* both bodies */
    Sl       *body[2];             /* 0 searches, 1 solves one programme */
    Run       run[2];
    double   *point[2];            /* shares of the nodes a goal is on: the head; or the rear end and the front end */
    double    from[2][2][3];       /* where those points were when the goals began, for each body */
    double    origin[2][3];        /* centre of mass then */
    double    target[2][2][3];     /* this step's, */
    double    weigh[3], spin;      /* the weight of a point's miss along x, y, z, and the change of angular momentum asked */
    double   *before;              /* positions, then velocities, before the step */
    double    volume, unit;        /* at rest; the spin goal counts in weight x height x dt */
    uint32_t  scene, built, steps, skip, visits;
    bool      restart, pause, cold;
} App;

static double volume_of(const Sl *s) {
    double total = 0.0;
    int    t;
    for (t = 0; t < s->tets; ++t) {
        const double *p = &s->p[3 * s->tet[4 * t]], *q = &s->p[3 * s->tet[4 * t + 1]], *r = &s->p[3 * s->tet[4 * t + 2]], *w = &s->p[3 * s->tet[4 * t + 3]];
        const double  a[3] = {q[0] - p[0], q[1] - p[1], q[2] - p[2]}, b[3] = {r[0] - p[0], r[1] - p[1], r[2] - p[2]}, c[3] = {w[0] - p[0], w[1] - p[1], w[2] - p[2]};
        total += (a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) + a[2] * (b[0] * c[1] - b[1] * c[0])) / 6.0;
    }
    return total;
}

/* The point sum of share[i] * p[i]. */
static void point_at(const Sl *s, const double *share, double at[3]) {
    int i, k;
    at[0] = at[1] = at[2] = 0.0;
    for (i = 0; i < s->nodes; ++i)
        for (k = 0; k < 3; ++k) at[k] += share[i] * s->p[3 * i + k];
}

/* The scene's body twice, side by side, come to rest on the ground. */
static void build(App *a) {
    static const double k_apart[3] = {1.2, 0.7, 0.8}, k_young[3] = {4e5, 4e5, 1e6};
    static const DemoOrbit k_view[3] = {
        {.target = {0.0f, 0.6f, 0.1f}, .distance = 4.4f, .yaw = 1.25f, .pitch = 0.3f},
        {.target = {0.6f, 0.1f, 0.0f}, .distance = 3.0f, .yaw = 0.25f, .pitch = 0.6f},
        {.target = {0.9f, 0.35f, 0.0f}, .distance = 4.4f, .yaw = 0.2f, .pitch = 0.5f}};
    SoftMesh *m = &a->mesh;
    int       end[2 * MUSCLES_MOST], group[MUSCLES_MOST], muscles = 0, line, i, j, k, b, step;
    double    middle[3] = {0.0, 0.0, 0.0}, *rest;

    /* a new scene has its own view; a restart keeps the one the user has */
    if (!a->body[0] || a->built != a->scene) {
        a->orbit = k_view[a->scene];
        a->orbit.fov_deg = 40.0f; a->orbit.znear = 0.05f; a->orbit.zfar = 50.0f; a->orbit.min_distance = 0.5f; a->orbit.max_distance = 15.0f;
    }
    free(m->rest); free(m->tet); free(m->face); free(a->point[0]); free(a->point[1]); free(a->before);
    for (b = 0; b < 2; ++b) {
        if (a->body[b]) sl_close(a->body[b]);
    }
    if (a->scene == 2u) {
        /* a ring of 16 x 1 x 2 cells; a muscle loop round the inside and one round the outside (figure 10c) */
        softmesh_ring(m, 16u, 1u, 2u, 0.3, 0.12);
        for (j = 0; j < 2; ++j)
            for (i = 0; i < 16; ++i) { end[2 * muscles] = i + 16 * j + 32; end[2 * muscles + 1] = (i + 1) % 16 + 16 * j + 32; group[muscles++] = j; }
        middle[2] = 0.12;
    } else {
        /* a box standing (2 x 6 x 2 cells of 0.2 m) or lying (8 x 1 x 2 of 0.1 m), its four lowest corners 4 cm lower: feet */
        const int    nx = a->scene ? 8 : 2, ny = a->scene ? 1 : 6, nz = 2, sy = nx + 1, sz = (nx + 1) * (ny + 1);
        const double cell = a->scene ? 0.1 : 0.2;
        softmesh_cells(m, (uint32_t)nx, (uint32_t)ny, (uint32_t)nz, cell, NULL);
        for (i = 0; i < (int)m->nodes; ++i) m->rest[3 * i + 1] += 0.04;
        for (line = 0; line < 4; ++line) {
            const int corner = (line % 2) * nx + (line / 2) * nz * sz;
            m->rest[3 * corner + 1] -= 0.04;
            /* four muscles along the body: up the column's edges from the first level, or along the lying body's edges */
            for (i = a->scene ? 0 : 1; i < (a->scene ? nx : ny); ++i) {
                end[2 * muscles] = a->scene ? i + (line % 2) * ny * sy + (line / 2) * nz * sz : corner + i * sy;
                end[2 * muscles + 1] = end[2 * muscles] + (a->scene ? 1 : sy);
                group[muscles++] = 0;
            }
        }
        middle[0] = 0.5 * cell * nx; middle[2] = 0.5 * cell * nz;
    }
    rest = (double *)malloc((size_t)m->nodes * 3u * sizeof(double));
    a->point[0] = (double *)calloc(m->nodes, sizeof(double)); a->point[1] = (double *)calloc(m->nodes, sizeof(double));
    a->before = (double *)malloc((size_t)m->nodes * 6u * sizeof(double));
    for (i = 0; i < (int)m->nodes; ++i) {
        const double *p = &m->rest[3 * i];
        /* the head: the middle of the column's top. The ends: the lying body's two end faces. */
        if (a->scene == 0u && fabs(p[0] - 0.2) + fabs(p[2] - 0.2) < 1e-9 && p[1] > 1.2) a->point[0][i] = 1.0;
        if (a->scene == 1u && p[0] < 1e-9) a->point[0][i] = 1.0 / 6.0;
        if (a->scene == 1u && p[0] > 0.8 - 1e-9) a->point[1][i] = 1.0 / 6.0;
    }
    for (b = 0; b < 2; ++b) {
        Sl *s;
        for (i = 0; i < (int)m->nodes; ++i)
            for (k = 0; k < 3; ++k) rest[3 * i + k] = m->rest[3 * i + k] - middle[k] + (k == 2 ? (b ? 0.5 : -0.5) * k_apart[a->scene] : 0.0);
        s = a->body[b] = sl_open((int)m->nodes, rest, (int)m->tets, m->tet, muscles, end, group, a->scene == 2u ? 0.12 : a->scene ? 0.1 : 0.2);
        s->young = k_young[a->scene]; s->stiffness = 1.2 * s->young; s->relax = 1e-4;
    }
    free(rest);
    a->volume = volume_of(a->body[0]);
    /* the first comes to rest; the second is the same body 'apart' further along z */
    for (step = 0; step < 90; ++step) sl_step(a->body[0], true);
    for (i = 0; i < (int)m->nodes; ++i)
        for (k = 0; k < 3; ++k) {
            a->body[1]->p[3 * i + k] = a->body[0]->p[3 * i + k] + (k == 2 ? k_apart[a->scene] : 0.0);
            a->body[1]->v[3 * i + k] = a->body[0]->v[3 * i + k];
        }
    for (b = 0; b < 2; ++b) {
        Sl *s = a->body[b];
        s->search = b ? 0 : 1;
        /* scene 1: friction that points forwards, against sliding backwards, may be ten times the rest (section 7, crawling) */
        if (a->scene == 1u) s->friction[7] = s->friction[0] = 8.0;
        point_at(s, a->point[0], a->from[b][0]); point_at(s, a->point[1], a->from[b][1]);
        a->unit = sl_centre(s, a->origin[b]) * s->gravity * a->origin[b][1] * s->dt;
        a->run[b] = (Run){.volume_low = 1.0, .volume_high = 1.0};
    }
    a->built = a->scene;
    a->steps = 0u;
}

/* The scene's goals for body b's next step, `time` after they began. */
static void goals(App *a, int b, double time) {
    Sl *s = a->body[b];
    if (a->scene == 0u) {
        /* the head on a circle of radius 0.25 m that passes through where it began, once in 4.2 s; a miss of 1 cm counts as 1 (eq. 17) */
        const double grow = fmin(time, 1.0) * 0.25;
        double      *t = a->target[b][0];
        t[0] = a->from[b][0][0] + grow * sin(1.5 * time); t[1] = a->from[b][0][1]; t[2] = a->from[b][0][2] + grow * (1.0 - cos(1.5 * time));
        a->weigh[0] = a->weigh[2] = 1e4; a->weigh[1] = 0.0;
        sl_goal_point(s, a->weigh, a->point[0], t);
    }
    if (a->scene == 1u) {
        /* each 1.2 s the rear end is asked 0.12 m forward and then the front end (section 7, crawling); a miss of 0.1 m/s x dt counts as 1 */
        const double cycle = floor(time / 1.2), phase = time / 1.2 - cycle;
        const double rear = phase < 0.5 ? 0.5 - 0.5 * cos(6.283185307179586 * phase) : 1.0, front = phase < 0.5 ? 0.0 : 0.5 - 0.5 * cos(6.283185307179586 * (phase - 0.5));
        memcpy(a->target[b], a->from[b], sizeof a->target[b]);
        a->target[b][0][0] += 0.12 * (cycle + rear); a->target[b][1][0] += 0.12 * (cycle + front);
        a->weigh[0] = 100.0 / (s->dt * s->dt); a->weigh[1] = 0.0; a->weigh[2] = 0.1 * a->weigh[0];
        sl_goal_point(s, a->weigh, a->point[0], a->target[b][0]);
        sl_goal_point(s, a->weigh, a->point[1], a->target[b][1]);
    }
    if (a->scene == 2u && fmod(time, 3.0) < 1.5) {
        /* angular momentum about z asked to change at 50 kg m^2 / s^2 for 1.5 s (eq. 14), then nothing for 1.5 s, then the other way */
        const double rate[3] = {0.0, 0.0, fmod(time, 6.0) < 3.0 ? -50.0 : 50.0};
        a->spin = rate[2];
        sl_goal_spin(s, 1.0 / (a->unit * a->unit), rate);
    }
}

/* One step of body b with the scene's goals, and what it did. */
static void advance(App *a, int b) {
    Sl          *s = a->body[b];
    Run         *r = &a->run[b];
    const double time = (a->steps + 1u) * s->dt, narrow = cos(0.39269908169872414);
    double       centre[3], mass = sl_centre(s, centre), again = 0.0, at[3], vol;
    bool         asked;
    int          i, j, k, e;

    memcpy(a->before, s->p, (size_t)s->nodes * 3u * sizeof(double)); memcpy(a->before + 3 * s->nodes, s->v, (size_t)s->nodes * 3u * sizeof(double));
    goals(a, b, time);
    asked = s->goals > 0;
    sl_step(s, true);
    r->steps += 1u; r->solved += s->solved; r->unplanned += asked && !s->planned;
    vol = volume_of(s) / a->volume;
    r->volume_low = fmin(r->volume_low, vol); r->volume_high = fmax(r->volume_high, vol);

    /* how far the goals' points are from where they were asked */
    r->miss = 0.0;
    for (j = 0; j < (a->scene == 2u ? 0 : (int)a->scene + 1); ++j) {
        point_at(s, a->point[j], at);
        r->miss = fmax(r->miss, a->scene ? fabs(at[0] - a->target[b][j][0]) : hypot(at[0] - a->target[b][j][0], at[2] - a->target[b][j][2]));
        for (k = 0; k < 3; ++k) again += a->weigh[k] * (at[k] - a->target[b][j][k]) * (at[k] - a->target[b][j][k]);
    }
    r->miss_sum += r->miss; r->miss_worst = fmax(r->miss_worst, r->miss);
    if (!s->planned) return;

    /* the plan's objective, counted again from the step taken */
    for (k = 0; k < (a->scene == 2u ? 3 : 0); ++k) {
        /* eq. 14 with the positions of before the step: the change about x and y was asked to be 0 */
        const int k1 = (k + 1) % 3, k2 = (k + 2) % 3;
        double    change = k == 2 ? -s->dt * a->spin : 0.0;
        for (i = 0; i < s->nodes; ++i) {
            const double *p = &a->before[3 * i], *v = &a->before[3 * (s->nodes + i)];
            change += s->mass[i] * ((p[k1] - centre[k1]) * (s->v[3 * i + k2] - v[k2]) - (p[k2] - centre[k2]) * (s->v[3 * i + k1] - v[k1]));
        }
        again += change * change / (a->unit * a->unit);
    }
    for (j = 0; j < s->muscles; ++j) again += s->relax * pow(s->wanted[j] / s->length0[j] - 1.0, 2.0);
    r->recount = fmax(r->recount, fabs(again - s->objective) / (1.0 + s->objective));

    /* eq. 11 for every contact, with impulses / (weight x dt) and velocities / (g x dt) */
    for (i = 0; i < s->contacts; ++i) {
        const double *f = &s->impulse[3 * i], *v = &s->v[3 * s->contact_node[i]], y = a->before[3 * s->contact_node[i] + 1];
        const double  unit = mass * s->gravity * s->dt, speed = s->gravity * s->dt, least = y > 0.0 ? -y / s->dt : -0.2 * y / s->dt;
        const double  angle = (s->contact_did[i] - SL_SLIDE + 0.5) * 0.78539816339744831, co = cos(angle), si = sin(angle);
        double        worst = fmax(-f[0] / unit, (least - v[1]) / speed);   /* it pushes, and does not go through the ground */
        for (e = 0; e < 8; ++e) {
            const double turn = (e + 0.5) * 0.78539816339744831;
            worst = fmax(worst, (cos(turn) * f[1] + sin(turn) * f[2] - s->friction[e] * narrow * f[0]) / unit);   /* friction in the cone */
        }
        if (s->contact_did[i] == SL_FREE) worst = fmax(worst, (fabs(f[0]) + fabs(f[1]) + fabs(f[2])) / unit);
        if (s->contact_did[i] != SL_FREE) worst = fmax(worst, fabs(v[1] - least) / speed);
        if (s->contact_did[i] == SL_STATIC) worst = fmax(worst, (fabs(v[0]) + fabs(v[2])) / speed);
        if (s->contact_did[i] >= SL_SLIDE) {
            /* friction on its edge, the velocity straight against it */
            worst = fmax(worst, fabs(co * f[1] + si * f[2] - s->friction[s->contact_did[i] - SL_SLIDE] * narrow * f[0]) / unit);
            worst = fmax(worst, fmax(fabs(-si * v[0] + co * v[2]), co * v[0] + si * v[2]) / speed);
        }
        r->violation = fmax(r->violation, worst);
        r->slid += s->contact_did[i] >= SL_SLIDE; r->let_go += s->contact_did[i] == SL_FREE && y < 1e-3;
    }
}

static void app_ui(App *a, struct nk_context *ctx) {
    static const char *const k_scenes[3] = {"Reach: the head follows a point", "Crawl: the ends go forward in turn", "Roll: angular momentum asked"};
    static const char *const k_names[2] = {"search (blue)", "one programme (orange)"};
    int b;
    if (nk_begin(ctx, "Soft Body Locomotion", nk_rect(16, 16, 380, 300), NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_TITLE)) {
        int visits = (int)a->visits;
        if (demo_ui_choice(ctx, "Scene", k_scenes, 3u, &a->scene)) a->restart = true;
        nk_layout_row_dynamic(ctx, 22, 1);
        nk_property_int(ctx, "Programmes a step, at most", 1, &visits, 64, 1, 1.0f);
        a->visits = (uint32_t)visits;
        demo_ui_bool(ctx, "Start from all static, not the last step", &a->cold);
        demo_ui_bool(ctx, "Pause", &a->pause);
        nk_layout_row_dynamic(ctx, 24, 1);
        if (nk_button_label(ctx, "Restart")) a->restart = true;
        for (b = 0; b < 2; ++b) {
            const Sl  *s = a->body[b];
            const Run *r = &a->run[b];
            double     centre[3];
            sl_centre(s, centre);
            demo_ui_text(ctx, "%s: %d programmes, %d contacts", k_names[b], s->solved, s->contacts);
            if (a->scene == 0u) demo_ui_text(ctx, "  head to target %.3f m, mean %.3f m", r->miss, r->miss_sum / (r->steps + 1e-9));
            if (a->scene != 0u) demo_ui_text(ctx, "  gone %.2f m, %u steps with no plan", centre[0] - a->origin[b][0], r->unplanned);
        }
        demo_ui_text(ctx, "contacts: green static, yellow sliding");
    }
    nk_end(ctx);
}

/* p moved a third of the way to the eye: the same place on the screen, in front of the body. */
static void app_front(const Canvas *c, const double *p, float out[3]) {
    int k;
    for (k = 0; k < 3; ++k) out[k] = (float)p[k] + 0.33f * (c->eye[k] - (float)p[k]);
}

static void app_frame(App *a) {
    static const uint32_t k_colour[2] = {CANVAS_RGB(90, 160, 215), CANVAS_RGB(215, 150, 80)};
    Demo    *d = &a->demo;
    Canvas  *c = &a->canvas;
    uint32_t i;
    int      b, j;

    /* a scene starts again after 24 s, the crawl after 12 s */
    if (a->restart || a->built != a->scene || a->steps >= (a->scene == 1u ? 720u : 1440u)) { build(a); a->restart = false; }
    for (i = 0u; i < (a->pause ? 0u : 1u + a->skip); ++i) {
        for (b = 0; b < 2; ++b) {
            a->body[b]->visits = (int)a->visits; a->body[b]->warm = !a->cold;
            advance(a, b);
        }
        a->steps++;
    }
    a->skip = 0u;

    canvas_view_orbit(d, c, &a->orbit);
    for (i = 0u; i < 2u; ++i) {
        const float q[4][3] = {{-4, 0, -4}, {4, 0, -4}, {4, 0, 4}, {-4, 0, 4}};
        canvas_triangle(c, q[0], q[1 + i], q[2 + i], CANVAS_RGB(92, 98, 110));
    }
    for (i = 0u; i <= 32u; ++i) {
        const float at = -4.0f + 0.25f * (float)i, lines[4][3] = {{at, 0.002f, -4}, {at, 0.002f, 4}, {-4, 0.002f, at}, {4, 0.002f, at}};
        canvas_line(c, lines[0], lines[1], 0.004f, CANVAS_RGB(70, 75, 86));
        canvas_line(c, lines[2], lines[3], 0.004f, CANVAS_RGB(70, 75, 86));
    }
    for (b = 0; b < 2; ++b) {
        const Sl *s = a->body[b];
        softmesh_draw(&a->mesh, s->p, c, k_colour[b], 0.0025f);
        for (j = 0; j < s->muscles; ++j) {
            /* a muscle: grey at its rest length, red when it wants half of it */
            const float want = (float)(2.0 * (1.0 - s->wanted[j] / s->length0[j]));
            float       p[2][3];
            app_front(c, &s->p[3 * s->end[2 * j]], p[0]); app_front(c, &s->p[3 * s->end[2 * j + 1]], p[1]);
            canvas_line(c, p[0], p[1], 0.004f + 0.006f * want, CANVAS_RGB(200 + (int)(55.0f * want), 200 - (int)(150.0f * want), 200 - (int)(160.0f * want)));
        }
        for (j = 0; j < s->contacts; ++j) {
            float p[3];
            app_front(c, &s->p[3 * s->contact_node[j]], p);
            if (s->contact_did[j] != SL_FREE) canvas_dot(c, p, 0.012f, s->contact_did[j] == SL_STATIC ? CANVAS_RGB(90, 220, 110) : CANVAS_RGB(250, 225, 70));
        }
        for (j = 0; j < (a->scene == 2u ? 0 : (int)a->scene + 1); ++j) {
            /* where the goal's point is asked to be, and the point */
            double at[3];
            float  p[2][3];
            point_at(s, a->point[j], at);
            if (a->scene == 1u) a->target[b][j][1] = at[1];
            app_front(c, a->target[b][j], p[0]); app_front(c, at, p[1]);
            canvas_line(c, p[0], p[1], 0.004f, CANVAS_RGB(240, 240, 240));
            canvas_dot(c, p[0], 0.018f, CANVAS_RGB(240, 90, 90));
        }
    }
    canvas_frame(d, c);
}

static void app_init(App *a) {
    canvas_open(&a->demo, &a->canvas, 60000u, CANVAS_RGB(38, 42, 52));
    build(a);
    if (a->demo.headless && !a->demo.frames) a->demo.frames = 2u;
}

static double noise(uint32_t *state) {
    *state = *state * 1664525u + 1013904223u;
    return (double)(*state >> 8) / 8388608.0 - 1.0;
}

static void app_check(App *a) {
    static const char *const k_scene[3] = {"reach", "crawl", "roll"};
    Demo    *d = &a->demo;
    uint32_t rng = 5u, scene, step;
    int      i, j, k, b, trial;
    char     label[160];

    /* 1. A muscle is an internal force: every column of A (eq. 6) adds up to no force and no torque. */
    for (scene = 0u; scene < 3u; ++scene) {
        Sl    *s;
        double force = 0.0, torque = 0.0, size = 0.0;
        a->scene = scene; build(a);
        s = a->body[0];
        sl_step(s, false);   /* A where the body is now */
        for (j = 0; j < s->muscles; ++j) {
            double sum[3] = {0, 0, 0}, turn[3] = {0, 0, 0}, largest = 0.0;
            for (i = 0; i < s->nodes; ++i) {
                const double *p = &s->p[3 * i], f[3] = {s->a[(size_t)(3 * i) * s->muscles + j], s->a[(size_t)(3 * i + 1) * s->muscles + j], s->a[(size_t)(3 * i + 2) * s->muscles + j]};
                for (k = 0; k < 3; ++k) { sum[k] += f[k]; largest = fmax(largest, fabs(f[k])); }
                turn[0] += p[1] * f[2] - p[2] * f[1]; turn[1] += p[2] * f[0] - p[0] * f[2]; turn[2] += p[0] * f[1] - p[1] * f[0];
            }
            force = fmax(force, (fabs(sum[0]) + fabs(sum[1]) + fabs(sum[2])) / largest);
            torque = fmax(torque, (fabs(turn[0]) + fabs(turn[1]) + fabs(turn[2])) / largest);
            size += largest / s->muscles;
        }
        snprintf(label, sizeof label, "%s, %d muscles: largest |sum of a muscle's forces| / its largest force", k_scene[scene], s->muscles);
        demo_check(d, label, force, "< 1e-10", force < 1e-10 && size > 0.0);
        demo_check(d, "  |sum of their torques| / (largest force x 1 m)", torque, "< 1e-10", torque < 1e-10);
    }

    /* 2. The column alone (section 4): it stands, shortens, bends, and keeps its volume to 15 percent. */
    {
        Sl    *s;
        double top, fastest = 0.0, head[3], squashed, bent[3], low = 1.0, used = 0.0;
        a->scene = 0u; build(a);
        s = a->body[0];
        for (step = 0u; step < 200u; ++step) sl_step(s, true);
        for (i = 0; i < 3 * s->nodes; ++i) fastest = fmax(fastest, fabs(s->v[i]));
        point_at(s, a->point[0], head); top = head[1];
        demo_check(d, "column, no goals, 4.8 s: fastest node, m / s", fastest, "< 1e-4", fastest < 1e-4);
        demo_check(d, "  height of the head / at rest (1.24 m)", top / 1.24, "0.95 .. 1", top / 1.24 > 0.95 && top / 1.24 < 1.0);
        demo_check(d, "  volume / at rest", volume_of(s) / a->volume, "0.99 .. 1", volume_of(s) / a->volume > 0.99 && volume_of(s) / a->volume < 1.0);
        for (i = 0; i < s->contacts; ++i) {
            const double *f = &s->impulse[3 * i];
            for (k = 0; k < 8 && f[0] > 0.0; ++k) used = fmax(used, (cos((k + 0.5) * 0.78539816339744831) * f[1] + sin((k + 0.5) * 0.78539816339744831) * f[2]) / (s->friction[k] * cos(0.39269908169872414) * f[0]));
        }
        demo_check(d, "  friction of a foot / the most its cone allows, largest (reported)", used, "", true);
        for (j = 0; j < s->muscles; ++j) s->wanted[j] = 0.5 * s->length0[j];
        for (step = 0u; step < 200u; ++step) sl_step(s, true);
        point_at(s, a->point[0], head); squashed = head[1] / top;
        low = fmin(low, volume_of(s) / a->volume);
        demo_check(d, "all 20 muscles asked for half their length: height / before", squashed, "< 0.85", squashed < 0.85);
        for (j = 0; j < s->muscles; ++j) s->wanted[j] = s->length0[j] * (j / 5 == 1 ? 0.7 : 1.0);
        for (step = 0u; step < 300u; ++step) sl_step(s, true);
        point_at(s, a->point[0], bent);
        low = fmin(low, volume_of(s) / a->volume);
        demo_check(d, "only those of the edge at +x, -z, for 0.7 of their length: the head moves towards +x, m", bent[0] - a->from[0][0][0], "> 0.05", bent[0] - a->from[0][0][0] > 0.05);
        demo_check(d, "  and towards -z, m", bent[2] - a->from[0][0][2], "< -0.05", bent[2] - a->from[0][0][2] < -0.05);
        demo_check(d, "  height of the head / standing (it has not fallen)", bent[1] / top, "> 0.85", bent[1] / top > 0.85);
        demo_check(d, "  smallest volume / at rest in both (the paper: changes stay below 15 percent)", low, "> 0.85", low > 0.85);
    }

    /* 3. One programme against steps really taken with other lengths. In the air only the muscles' range binds,
     * and the head is asked where a step with known lengths takes it. On the ground it is asked 2 mm sideways,
     * and the steps that count are those that leave the feet static. */
    for (trial = 0; trial < 2; ++trial) {
        Sl          *s;
        const double weight[3] = {1e4, 1e4, 1e4};
        double      *state, *chosen, target[3], best, made = 0.0, below = 1e300, at[3];
        int          kept = 0, tried = 0, at_bound = 0;
        bool         planned;
        a->scene = 0u; build(a);
        s = a->body[0]; s->search = 0;
        state = (double *)malloc((size_t)s->nodes * 6u * sizeof(double)); chosen = (double *)malloc((size_t)s->muscles * sizeof(double));
        for (i = 0; i < s->nodes && trial == 0; ++i) s->p[3 * i + 1] += 1.0;
        memcpy(state, s->p, (size_t)s->nodes * 3u * sizeof(double)); memcpy(state + 3 * s->nodes, s->v, (size_t)s->nodes * 3u * sizeof(double));
        for (j = 0; j < s->muscles; ++j) {
            s->wanted[j] = s->length0[j] * (0.87 + 0.12 * noise(&rng));
            made += s->relax * pow(s->wanted[j] / s->length0[j] - 1.0, 2.0);
        }
        sl_step(s, true);
        point_at(s, a->point[0], target);
        memcpy(s->p, state, (size_t)s->nodes * 3u * sizeof(double)); memcpy(s->v, state + 3 * s->nodes, (size_t)s->nodes * 3u * sizeof(double));
        if (trial == 1) { point_at(s, a->point[0], target); target[0] += 0.002; target[2] -= 0.001; }
        sl_goal_point(s, weight, a->point[0], target);
        sl_step(s, false);
        best = s->objective; planned = s->planned;
        memcpy(chosen, s->wanted, (size_t)s->muscles * sizeof(double));
        for (j = 0; j < s->muscles; ++j) at_bound += chosen[j] < 0.5001 * s->length0[j] || chosen[j] > 0.9999 * s->length0[j];
        for (step = 0u; step < 300u; ++step) {
            double value = 0.0;
            bool   same = true;
            memcpy(s->p, state, (size_t)s->nodes * 3u * sizeof(double)); memcpy(s->v, state + 3 * s->nodes, (size_t)s->nodes * 3u * sizeof(double));
            /* the first 100 anywhere in the range, the rest near the chosen lengths */
            for (j = 0; j < s->muscles; ++j)
                s->wanted[j] = fmin(fmax(step < 100u ? s->length0[j] * (0.75 + 0.25 * noise(&rng)) : chosen[j] + 0.01 * s->length0[j] * noise(&rng), 0.5 * s->length0[j]), s->length0[j]);
            sl_step(s, true);
            for (i = 0; i < s->contacts; ++i) same &= s->contact_did[i] == (s->p[3 * s->contact_node[i] + 1] < 1e-3 && state[3 * s->contact_node[i] + 1] < 1e-3 ? SL_STATIC : SL_FREE);
            point_at(s, a->point[0], at);
            for (k = 0; k < 3; ++k) value += weight[k] * (at[k] - target[k]) * (at[k] - target[k]);
            for (j = 0; j < s->muscles; ++j) value += s->relax * pow(s->wanted[j] / s->length0[j] - 1.0, 2.0);
            tried += 1; kept += same;
            if (same) below = fmin(below, (value - best) / best);
        }
        if (trial == 0) demo_check(d, "column in the air, head asked where known lengths take it: objective / theirs", best / made, "<= 1", planned && best <= made * (1.0 + 1e-9));
        if (trial == 1) demo_check(d, "column on its feet, all static, head asked 2 mm sideways: objective (reported)", best, "", planned);
        demo_check(d, "  muscles it leaves at an end of their range, of 20 (reported)", at_bound, "", true);
        snprintf(label, sizeof label, "  of %d steps taken with other lengths, those with the same contact cases", tried);
        demo_check(d, label, kept, trial ? "> 100" : "= 300", trial ? kept > 100 : kept == 300);
        demo_check(d, "  least (their objective - the programme's) / the programme's", below, "> -1e-6", below > -1e-6);
        free(state); free(chosen);
    }

    /* 4. The three scenes: what the search and the one programme achieve, and every planned step against eq. 11. */
    for (scene = 0u; scene < 3u; ++scene) {
        static const uint32_t k_steps[3] = {480u, 288u, 360u};
        double gone[2], half[2] = {0.0, 0.0}, centre[3];
        a->scene = scene; build(a);
        for (a->steps = 0u; a->steps < k_steps[scene]; ++a->steps)
            for (b = 0; b < 2; ++b) {
                advance(a, b);
                sl_centre(a->body[b], centre); gone[b] = centre[0] - a->origin[b][0];
                if (a->steps == 179u) half[b] = gone[b];
            }
        if (scene == 0u) {
            const double mean[2] = {a->run[0].miss_sum / a->run[0].steps, a->run[1].miss_sum / a->run[1].steps};
            demo_check(d, "reach, 8 s, circle of radius 0.25 m: mean distance head to target with the search, m", mean[0], "< 0.08", mean[0] < 0.08);
            demo_check(d, "  with one programme, m", mean[1], "> 2 x the search's", mean[1] > 2.0 * mean[0]);
            demo_check(d, "  worst with the search, m (reported)", a->run[0].miss_worst, "", true);
            demo_check(d, "  worst with one programme, m (reported)", a->run[1].miss_worst, "", true);
        }
        if (scene == 1u) {
            demo_check(d, "crawl, 4.8 s, 0.48 m asked: centre of mass gone forward with the search, m", gone[0], "0.42 .. 0.52", gone[0] > 0.42 && gone[0] < 0.52);
            demo_check(d, "  with one programme, m", gone[1], "< 0.05", fabs(gone[1]) < 0.05);
            demo_check(d, "  worst distance of an end from its target with the search, m", a->run[0].miss_worst, "< 0.05", a->run[0].miss_worst < 0.05);
        }
        if (scene == 2u) {
            demo_check(d, "roll, asked for 1.5 s, at 3 s: centre of mass gone with the search, m", half[0], "> 0.5", half[0] > 0.5);
            demo_check(d, "  with one programme, m (reported)", half[1], "", true);
            demo_check(d, "  asked the other way from 3 s to 4.5 s, at 6 s: with the search, m", gone[0], "< at 3 s - 0.5", gone[0] < half[0] - 0.5);
            demo_check(d, "  with one programme, m (reported)", gone[1], "", true);
        }
        for (b = 0; b < 2; ++b) {
            const Run *r = &a->run[b];
            snprintf(label, sizeof label, "  %s: programmes a step (reported)", b ? "one programme" : "search");
            demo_check(d, label, (double)r->solved / r->steps, "", true);
            demo_check(d, "    steps with goals and no programme that can be met (reported)", r->unplanned, "", true);
            demo_check(d, "    contacts that slid, summed over the steps (reported)", r->slid, "", true);
            demo_check(d, "    contacts on the ground that let go (reported)", r->let_go, "", true);
            demo_check(d, "    planned steps: largest breach of eq. 11, impulses / (weight x dt), speeds / (g x dt)", r->violation, "< 1e-7", r->violation < 1e-7);
            demo_check(d, "    largest |objective - the same counted from the step taken| / (1 + objective)", r->recount, "< 1e-7", r->recount < 1e-7);
            demo_check(d, "    volume / at rest, least", r->volume_low, "> 0.85", r->volume_low > 0.85);
            demo_check(d, "    volume / at rest, most", r->volume_high, "< 1.15", r->volume_high < 1.15);
        }
    }

    /* The roll again with other limits on the search: how far it goes depends on them. */
    for (trial = 0; trial < 2; ++trial) {
        double centre[3];
        a->scene = 2u; build(a);
        a->body[0]->visits = trial ? 64 : 8;
        for (a->steps = 0u; a->steps < 180u; ++a->steps) advance(a, 0);
        sl_centre(a->body[0], centre);
        snprintf(label, sizeof label, "roll at 3 s with at most %d programmes a step: gone, m (reported)", a->body[0]->visits);
        demo_check(d, label, centre[0] - a->origin[0][0], "", true);
    }

    /* 5. The search against one programme and against all 10^4 (section 8, table 2): the column on four feet,
     * every eighth step of its own run that has 4 contacts, the first 5 of them. */
    {
        static const int k_search[4] = {0, 1, 1, 2}, k_visits[4] = {1, 16, 64, 16};
        double sum[4] = {0, 0, 0, 0}, worse = 0.0, closed[2] = {0.0, 0.0};
        int    states = 0, equal[2] = {0, 0}, visited[4] = {0, 0, 0, 0}, first_fails = 0;
        a->scene = 0u; build(a);
        for (a->steps = 0u; states < 5 && a->steps < 240u; ++a->steps) {
            Sl    *s = a->body[0];
            double value[4];
            if (a->steps % 8u == 7u && s->contacts == 4) {
                for (k = 0; k < 4; ++k) {
                    s->search = k_search[k]; s->visits = k_visits[k]; s->warm = false;
                    goals(a, 0, (a->steps + 1u) * s->dt);
                    sl_step(s, false);
                    value[k] = s->objective; sum[k] += value[k]; visited[k] += s->solved;
                    first_fails += k == 0 && !s->planned;
                }
                s->search = 1; s->visits = 16; s->warm = true;
                states += 1;
                for (k = 0; k < 2; ++k) {
                    equal[k] += value[k + 1] < value[3] * (1.0 + 1e-6) + 1e-12;
                    closed[k] += (value[0] - value[k + 1]) / (value[0] - value[3] + 1e-300);
                    worse = fmax(worse, (value[3] - value[k + 1]) / (1.0 + value[3]));
                }
            }
            advance(a, 0);
        }
        demo_check(d, "column on four feet, states with 4 contacts taken from its run: compared", states, "= 5", states == 5);
        demo_check(d, "  one programme: mean objective", sum[0] / states, "", first_fails == 0);
        demo_check(d, "  search of at most 16 programmes: mean objective", sum[1] / states, "<= one programme's", sum[1] <= sum[0]);
        demo_check(d, "    programmes solved, mean", (double)visited[1] / states, "<= 16", visited[1] <= 16 * states);
        demo_check(d, "    mean share of (one programme - best of all) it gains (reported)", closed[0] / states, "", true);
        demo_check(d, "    states in which it finds the best of all (reported)", equal[0], "", true);
        demo_check(d, "  search of at most 64 programmes: mean objective", sum[2] / states, "<= that of 16", sum[2] <= sum[1] * (1.0 + 1e-9));
        demo_check(d, "    programmes solved, mean", (double)visited[2] / states, "<= 64", visited[2] <= 64 * states);
        demo_check(d, "    mean share of (one programme - best of all) it gains (reported)", closed[1] / states, "", true);
        demo_check(d, "    states in which it finds the best of all (reported)", equal[1], "", true);
        demo_check(d, "  all 10^4 programmes: mean objective", sum[3] / states, "<= the searches'", sum[3] <= sum[2] * (1.0 + 1e-9));
        demo_check(d, "    largest (it - a search) / (1 + it)", worse, "< 1e-9", worse < 1e-9);
    }
}

int main(int argc, char **argv) {
    static App app;
    static const DemoOption options[] = {
        {"--scene", .u32 = &app.scene, .lo = 0, .hi = 2, .help = "0 reach, 1 crawl, 2 roll"},
        {"--visits", .u32 = &app.visits, .lo = 1, .hi = 64, .help = "programmes the search may solve in a step"},
        {"--skip", .u32 = &app.skip, .lo = 0, .hi = 1440, .help = "steps of 1/60 s taken before the first picture"},
        {"--cold", .flag = &app.cold, .help = "start each search from all static instead of the last step's choice"},
        {0}};
    Demo *d = &app.demo;
    app.visits = 16u;
    demo_defaults(d, "softloco", "Soft Body Locomotion");
    demo_open(d, options, argc, argv);
    app_init(&app);
    while (demo_running(d)) {
        if (d->ui) app_ui(&app, d->ui);
        app_frame(&app);
    }
    if (d->check) app_check(&app);
    return demo_close(d);
}
