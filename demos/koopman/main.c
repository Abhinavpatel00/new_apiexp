/* koopman — Low-Rank Koopman Deformables with Log-Linear Time Integration: a soft finger whose
 * motion is one matrix.
 *
 *   Draw -> Present          (the method runs in C: method.c)
 *
 * Chang, Chen, Grinspun, Chiaramonte: arXiv 2602.07687. A finger of tetrahedra, held at its base, is
 * simulated with implicit Euler while it is pushed along x, along z and upwards. A matrix that takes
 * each state to the next is fitted to those states and kept as eigenvalues and modes (sections 3
 * and 4). After that the finger is moved by the eigenvalues alone, many steps at a time.
 *
 *   scene 0  a push, then free swinging, three times: implicit Euler with the small step (grey), the
 *            fitted operator taking `many` steps at once (blue), implicit Euler with a step `many`
 *            times as long (orange). The board behind shows their kinetic energies (figure 2)
 *   scene 1  pushes the fit never saw, in other directions and strengths: implicit Euler (grey) and
 *            the operator (blue) (figures 5 and 11)
 *   scene 2  the operator with its eigenvalues scaled by 1, 0.99 and 0.98 (section 7.2)
 *   scene 3  pushes found with the operator so that the tip comes to a goal (white) and to rest
 *            there, given to implicit Euler (grey) and to the operator (blue) (section 7.3)
 *
 * --check tests the elastic body, the decomposition on a system whose eigenvalues are known, and
 * what the paper says about the fitted operator.
 */
#include "canvas.h"
#include "method.h"
#include "softmesh.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SLOTS    3
#define HISTORY  240
#define STEP     0.004    /* s: the step of the data, as in the paper's figure 2 */
#define YOUNG    4e5      /* Pa */
#define POISSON  0.4
#define DENSITY  1000.0   /* kg/m^3 */
#define DAMPING  1.0      /* 1/s */
#define DATA_PUSH 5.0     /* m/s^2: the push of the data */
#define EVERY    3        /* every third step of the data is a pair of the fit */
#define MAX_RANK 60

enum { NONE, FULL, MODEL, COARSE };
static const uint8_t k_kind[4][SLOTS] = {{FULL, MODEL, COARSE}, {FULL, MODEL, NONE}, {MODEL, MODEL, MODEL}, {FULL, MODEL, NONE}};

typedef struct App {
    Demo            demo;   /* first: see demo.h */
    Canvas          canvas;
    DemoOrbit       orbit;
    SoftMesh        mesh;
    int             tip;                /* a node of the top */
    Kd             *model;
    double complex *fitted;             /* the eigenvalues as kd_rank found them */
    double complex *z[SLOTS], *reduced; /* the operator's states; scratch */
    KdBody         *full, *coarse;      /* implicit Euler with the step of the data, and with `many` of them */
    double         *state, *accel, *place, *controls;
    double          held[3], control[2], goal[2], miss, error[2], energy[SLOTS][HISTORY];
    uint32_t        scene, built, steps, rank, many, outside;
    float           push, damp;
    bool            unclamped, pause, restart;
} App;

/* The finger at rest with its base held. */
static KdBody *body(const App *a, double damping) {
    KdBody *b = kd_body_open((int)a->mesh.nodes, a->mesh.rest, (int)a->mesh.tets, a->mesh.tet, YOUNG, POISSON, DENSITY);
    for (uint32_t i = 0u; i < a->mesh.nodes; ++i) b->fixed[i] = a->mesh.rest[3u * i + 1u] < 1e-9;
    b->damping = damping;
    return b;
}

/* The acceleration `acc` for every node that is not held. */
static void spread(const App *a, const double acc[3], double *accel) {
    for (uint32_t i = 0u; i < 3u * a->mesh.nodes; ++i) accel[i] = a->mesh.rest[3u * (i / 3u) + 1u] < 1e-9 ? 0.0 : acc[i % 3u];
}

/* The kick that the acceleration `acc` is for one step (eq. 19), as a state: [0; h^2 acc]. */
static void kick(const App *a, const double acc[3], double *state) {
    const uint32_t n = 3u * a->mesh.nodes;
    memset(state, 0, n * sizeof(double));
    spread(a, acc, state + n);
    for (uint32_t i = n; i < 2u * n; ++i) state[i] *= STEP * STEP;
}

/* The body from rest: `on` steps pushed by acc, then `off` steps free. The state [u; h v] after every
 * step, and rest before the first, in out: (on + off + 1) states. */
static void run(const App *a, double damping, const double acc[3], int on, int off, double *out) {
    const size_t n = 3u * (size_t)a->mesh.nodes;
    KdBody      *b = body(a, damping);
    double      *accel = (double *)malloc(n * sizeof(double));
    spread(a, acc, accel);
    memset(out, 0, 2u * n * sizeof(double));
    for (int s = 1; s <= on + off; ++s) {
        kd_body_step(b, STEP, s <= on ? accel : NULL);
        for (size_t i = 0; i < n; ++i) {
            out[(size_t)s * 2u * n + i]     = b->u[i];
            out[(size_t)s * 2u * n + n + i] = STEP * b->v[i];
        }
    }
    free(accel);
    kd_body_close(b);
}

/* The operator fitted to `episodes` runs from rest, run e pushed by acc[e] for length[e][0] steps and
 * free for length[e][1]. */
static Kd *train(const App *a, int episodes, const double (*acc)[3], const int (*length)[2]) {
    const size_t dim = 6u * (size_t)a->mesh.nodes;
    size_t       pairs = 0u, at = 0u;
    double      *x, *y, *states, *push = (double *)malloc(dim * sizeof(double));
    Kd          *k;

    for (int e = 0; e < episodes; ++e) pairs += (size_t)((length[e][0] + length[e][1] + EVERY - 1) / EVERY);
    x = (double *)malloc(pairs * dim * sizeof(double));
    y = (double *)malloc(pairs * dim * sizeof(double));
    for (int e = 0; e < episodes; ++e) {
        states = (double *)malloc((size_t)(length[e][0] + length[e][1] + 1) * dim * sizeof(double));
        run(a, DAMPING, acc[e], length[e][0], length[e][1], states);
        kick(a, acc[e], push);
        /* a pushed step starts from its state with the kick in it */
        for (int s = 0; s < length[e][0] + length[e][1]; s += EVERY, ++at) {
            for (size_t i = 0; i < dim; ++i) x[at * dim + i] = states[(size_t)s * dim + i] + (s < length[e][0] ? push[i] : 0.0);
            memcpy(&y[at * dim], &states[(size_t)(s + 1) * dim], dim * sizeof(double));
        }
        free(states);
    }
    k = kd_open((int)dim, (int)pairs, x, y);
    free(x); free(y); free(push);
    return k;
}

/* Cuts the model to a->rank and keeps its eigenvalues as fitted. */
static void fit(App *a) {
    kd_rank(a->model, (int)a->rank);
    memcpy(a->fitted, a->model->lambda, a->rank * sizeof(double complex));
    a->outside = 0u;
    for (uint32_t i = 0u; i < a->rank; ++i) a->outside += cabs(a->fitted[i]) > 1.0;
}

/* The model's eigenvalues from the fitted ones: those outside the unit circle are put on it (not with
 * a->unclamped), then all are scaled by 1 - damp (section 7.2). */
static void edit(App *a, double damp) {
    for (uint32_t i = 0u; i < a->rank; ++i) {
        const double size = cabs(a->fitted[i]);
        a->model->lambda[i] = (1.0 - damp) * a->fitted[i] / (size > 1.0 && !a->unclamped ? size : 1.0);
    }
}

/* The model from rest under the push of a run (`truth`, from run()), `many` steps at a time: 100 times
 * the sum of |u - u of the run|^2 over the sum of |u of the run|^2, over the whole steps it stops at. */
static double replay(App *a, const double acc[3], int on, int off, double many, const double *truth) {
    const size_t n = 3u * (size_t)a->mesh.nodes;
    double       wrong = 0.0, all = 0.0;

    kick(a, acc, a->state);
    kd_project(a->model, a->state, a->reduced);
    memset(a->z[0], 0, a->rank * sizeof(double complex));
    for (double s = many; s <= on + off + 1e-9; s += many) {
        const double *want = &truth[(size_t)(s + 0.5) * 2u * n];
        kd_advance(a->model, many, s <= on + 1e-9 ? a->reduced : NULL, a->z[0]);
        kd_lift(a->model, a->z[0], a->state);
        for (size_t i = 0; i < n && fabs(s - floor(s + 0.5)) < 1e-9; ++i) {
            wrong += (a->state[i] - want[i]) * (a->state[i] - want[i]);
            all += want[i] * want[i];
        }
    }
    return 100.0 * wrong / all;
}

static void build(App *a) {
    /* with the board, and without */
    static const DemoOrbit k_view[2] = {{.target = {0.0f, 0.56f, -0.2f}, .distance = 2.5f, .yaw = 0.22f, .pitch = 0.14f},
                                        {.target = {0.0f, 0.34f, 0.0f}, .distance = 1.8f, .yaw = 0.3f, .pitch = 0.2f}};
    if (!a->full || a->built != a->scene) {
        a->orbit = k_view[a->scene % 2u];
        a->orbit.fov_deg = 40.0f; a->orbit.znear = 0.05f; a->orbit.zfar = 40.0f; a->orbit.min_distance = 0.4f; a->orbit.max_distance = 12.0f;
    }
    if (a->full) { kd_body_close(a->full); kd_body_close(a->coarse); }
    a->full   = body(a, DAMPING);
    a->coarse = body(a, DAMPING);
    fit(a);
    for (int s = 0; s < SLOTS; ++s) memset(a->z[s], 0, MAX_RANK * sizeof(double complex));
    memset(a->energy, 0, sizeof a->energy);
    memset(a->held, 0, sizeof a->held);
    memset(a->control, 0, sizeof a->control);
    a->error[0] = a->error[1] = 0.0;
    a->steps = 0u;
    a->built = a->scene;
}

static void app_ui(App *a, struct nk_context *ctx) {
    static const char *const k_scenes[4] = {"Step size: small, many at once, long", "Pushes the fit never saw", "Damping by eigenvalues", "Control of the tip"};
    static const char *const k_slots[4][SLOTS] = {{"implicit Euler, 4 ms (grey)", "operator (blue)", "implicit Euler, long step (orange)"},
                                                  {"implicit Euler, 4 ms (grey)", "operator (blue)", ""},
                                                  {"eigenvalues as fitted (blue)", "x 0.99 (green)", "x 0.98 (orange)"},
                                                  {"implicit Euler, 4 ms (grey)", "operator (blue)", ""}};
    if (nk_begin(ctx, "Low-Rank Koopman Deformables", nk_rect(16, 16, 400, 400), NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_TITLE)) {
        int rank = (int)a->rank, many = (int)a->many;
        if (demo_ui_choice(ctx, "Scene", k_scenes, 4u, &a->scene)) a->restart = true;
        nk_layout_row_dynamic(ctx, 22, 1);
        nk_property_int(ctx, "Rank", 2, &rank, MAX_RANK, 1, 1.0f);
        nk_layout_row_dynamic(ctx, 22, 1);
        nk_property_int(ctx, "Steps at once", 1, &many, 100, 1, 1.0f);
        a->restart = a->restart || rank != (int)a->rank;
        a->rank    = (uint32_t)rank;
        a->many    = (uint32_t)many;
        if (a->scene < 3u) demo_ui_float(ctx, "Push (m/s^2)", &a->push, 0.0f, 12.0f);
        if (a->scene != 2u) demo_ui_float(ctx, "Eigenvalues scaled by 1 minus", &a->damp, 0.0f, 0.05f);
        demo_ui_bool(ctx, "Leave eigenvalues outside the unit circle", &a->unclamped);
        demo_ui_bool(ctx, "Pause", &a->pause);
        nk_layout_row_dynamic(ctx, 24, 1);
        if (nk_button_label(ctx, "Restart")) a->restart = true;
        for (int s = 0; s < SLOTS; ++s) demo_ui_text(ctx, "%s", k_slots[a->scene][s]);
        demo_ui_text(ctx, "%d pairs, %u of %u eigenvalues outside the circle", a->model->pairs, a->outside, a->rank);
        if (a->scene != 2u) demo_ui_text(ctx, "operator against grey: %.3f %%", 100.0 * a->error[0] / (a->error[1] + 1e-300));
        if (a->scene == 3u) demo_ui_text(ctx, "push %.1f, %.1f m/s^2; the operator expects to miss by %.1e m", a->control[0], a->control[1], a->miss);
        if (a->scene % 2u == 0u) demo_ui_text(ctx, "board: kinetic energy, the last %.1f s", HISTORY * 4 * STEP);
    }
    nk_end(ctx);
}

/* One step of the data's length for everything in the scene. */
static void advance(App *a) {
    static const double k_unseen[4][3] = {{0.7071, 0.0, 0.7071}, {2.0, 0.0, 0.0}, {0.0, 0.0, -0.5}, {-1.2, 0.0, 1.6}};
    static const double k_goal[4][2] = {{0.10, 0.0}, {0.0, 0.12}, {-0.08, -0.08}, {0.0, 0.0}};
    const size_t   n = 3u * (size_t)a->mesh.nodes;
    const uint32_t s = a->steps;
    double         acc[3] = {0.0, 0.0, 0.0};

    /* The push of this step. */
    if (a->scene % 2u == 0u && s % 700u < 100u) acc[0] = a->push;
    for (int k = 0; k < 3 && a->scene == 1u && s % 300u < 100u; ++k) acc[k] = a->push * k_unseen[s / 300u % 4u][k];
    if (a->scene == 3u && s % 40u == 0u) {
        /* section 7.3: the two pushes that, held for 60 steps, bring the tip to the goal and to rest there. They are
         * held for 40 and then found again. */
        const int pick[2] = {3 * a->tip, 3 * a->tip + 2};
        memcpy(a->goal, k_goal[s / 320u % 4u], sizeof a->goal);
        edit(a, a->damp);
        a->miss = kd_control(a->model, 60.0, a->z[1], 2, a->controls, 2, pick, a->goal, 1.0, a->control);
    }
    if (a->scene == 3u) { acc[0] = a->control[0]; acc[2] = a->control[1]; }
    spread(a, acc, a->accel);
    if (k_kind[a->scene][0] == FULL) kd_body_step(a->full, STEP, a->accel);

    /* The steppers that take `many` steps at once hold the push that their step began with. */
    if (s % a->many == 0u) memcpy(a->held, acc, sizeof a->held);
    if ((s + 1u) % a->many == 0u) {
        spread(a, a->held, a->accel);
        if (a->scene == 0u) kd_body_step(a->coarse, a->many * STEP, a->accel);
        kick(a, a->held, a->state);
        kd_project(a->model, a->state, a->reduced);
        for (int slot = 0; slot < SLOTS; ++slot) {
            if (k_kind[a->scene][slot] != MODEL) continue;
            edit(a, a->scene == 2u ? 0.01 * slot : a->damp);
            kd_advance(a->model, a->many, a->reduced, a->z[slot]);
        }
        kd_lift(a->model, a->z[1], a->state);
        for (size_t i = 0; i < n && a->scene != 2u; ++i) {
            a->error[0] += (a->state[i] - a->full->u[i]) * (a->state[i] - a->full->u[i]);
            a->error[1] += a->full->u[i] * a->full->u[i];
        }
    }
    a->steps++;
}

/* The kinetic energy of what is in a slot, and its displacements in *u (for the operator: in a->state). */
static double observe(App *a, int slot, const double **u) {
    const size_t n = 3u * (size_t)a->mesh.nodes;
    const int    kind = k_kind[a->scene][slot];
    double       moving = 0.0;
    if (kind == MODEL) kd_lift(a->model, a->z[slot], a->state);
    *u = kind == FULL ? a->full->u : kind == COARSE ? a->coarse->u : a->state;
    for (size_t i = 0; i < n; ++i) {
        const double v = kind == FULL ? a->full->v[i] : kind == COARSE ? a->coarse->v[i] : a->state[n + i] / STEP;
        moving += 0.5 * a->full->mass[i / 3u] * v * v;
    }
    return moving;
}

static void app_frame(App *a) {
    static const uint32_t k_colour[4][SLOTS] = {{CANVAS_RGB(170, 174, 186), CANVAS_RGB(90, 160, 215), CANVAS_RGB(215, 150, 80)},
                                                {CANVAS_RGB(170, 174, 186), CANVAS_RGB(90, 160, 215), 0u},
                                                {CANVAS_RGB(90, 160, 215), CANVAS_RGB(110, 190, 120), CANVAS_RGB(215, 150, 80)},
                                                {CANVAS_RGB(170, 174, 186), CANVAS_RGB(90, 160, 215), 0u}};
    const size_t n = 3u * (size_t)a->mesh.nodes;
    const int    slots = k_kind[a->scene][2] ? 3 : 2;
    Demo        *d = &a->demo;
    Canvas      *c = &a->canvas;
    double       peak = 1e-12;

    if (a->restart || a->built != a->scene) { build(a); a->restart = false; }
    for (int i = 0; i < (a->pause ? 0 : 4); ++i) advance(a);

    canvas_view_orbit(d, c, &a->orbit);
    for (int i = 0; i < 2; ++i) {
        const float q[4][3] = {{-6, 0, -6}, {6, 0, -6}, {6, 0, 6}, {-6, 0, 6}};
        canvas_triangle(c, q[0], q[1 + i], q[2 + i], CANVAS_RGB(92, 98, 110));
    }
    for (int i = 0; i <= 48; ++i) {
        const float at = -6.0f + 0.25f * (float)i, lines[4][3] = {{at, 0.002f, -6}, {at, 0.002f, 6}, {-6, 0.002f, at}, {6, 0.002f, at}};
        canvas_line(c, lines[0], lines[1], 0.005f, CANVAS_RGB(70, 75, 86));
        canvas_line(c, lines[2], lines[3], 0.005f, CANVAS_RGB(70, 75, 86));
    }

    for (int slot = 0; slot < slots; ++slot) {
        const double  shift = 0.7 * (slot - 0.5 * (slots - 1));
        const double *u;
        const double  moving = observe(a, slot, &u);

        for (size_t i = 0; i < n; ++i) a->place[i] = a->mesh.rest[i] + u[i] + (i % 3u == 0u ? shift : 0.0);
        if (!a->pause) {
            memmove(&a->energy[slot][0], &a->energy[slot][1], (HISTORY - 1) * sizeof(double));
            a->energy[slot][HISTORY - 1] = moving;
        }
        softmesh_draw(&a->mesh, a->place, c, k_colour[a->scene][slot], 0.002f);
        if (a->scene == 3u) {
            /* white: where the tip node should be; yellow: where it is, at the same height */
            const double *rest = &a->mesh.rest[3 * a->tip];
            const float   high = (float)rest[1] + 0.04f;
            const float   want[2][3] = {{(float)(rest[0] + a->goal[0] + shift), (float)rest[1], (float)(rest[2] + a->goal[1])}, {(float)(rest[0] + a->goal[0] + shift), high, (float)(rest[2] + a->goal[1])}};
            const float   is[2][3] = {{(float)a->place[3 * a->tip], (float)a->place[3 * a->tip + 1], (float)a->place[3 * a->tip + 2]}, {(float)a->place[3 * a->tip], high, (float)a->place[3 * a->tip + 2]}};
            canvas_line(c, want[0], want[1], 0.004f, CANVAS_RGB(240, 240, 240));
            canvas_dot(c, want[1], 0.014f, CANVAS_RGB(240, 240, 240));
            canvas_line(c, is[0], is[1], 0.004f, CANVAS_RGB(250, 225, 70));
            canvas_dot(c, is[1], 0.010f, CANVAS_RGB(250, 225, 70));
        }
        for (int i = 0; i < HISTORY; ++i) peak = fmax(peak, a->energy[slot][i]);
    }

    for (int slot = 0; slot < (a->scene % 2u == 0u ? slots : 0); ++slot) {
        /* the board: 2 m wide for the last HISTORY frames, 0.7 m high for the largest energy on it */
        const float z = -0.6f, corner[4][3] = {{-1.0f, 0.5f, z}, {1.0f, 0.5f, z}, {1.0f, 1.2f, z}, {-1.0f, 1.2f, z}};
        for (int i = 0; i < 2 && slot == 0; ++i) canvas_triangle(c, corner[0], corner[1 + i], corner[2 + i], CANVAS_RGB(28, 31, 40));
        for (int i = 0; i + 1 < HISTORY; ++i) {
            float p[2][3];
            for (int k = 0; k < 2; ++k) {
                p[k][0] = -1.0f + 2.0f * (float)(i + k) / (float)(HISTORY - 1);
                p[k][1] = 0.5f + 0.7f * (float)(a->energy[slot][i + k] / peak);
                p[k][2] = z + 0.006f + 0.003f * (float)slot;
            }
            canvas_line(c, p[0], p[1], 0.008f, k_colour[a->scene][slot]);
        }
    }
    canvas_frame(d, c);
}

static void app_init(App *a) {
    static const double k_push[3][3] = {{DATA_PUSH, 0.0, 0.0}, {0.0, 0.0, DATA_PUSH}, {0.0, 2.0 * DATA_PUSH, 0.0}};
    static const int    k_length[3][2] = {{125, 125}, {125, 125}, {60, 60}};
    size_t              n;

    /* softmesh_cells makes the long axis z, which keeps the band of the stiffness narrow. The finger stands
     * along y, 0.6 m high, 0.15 m wide at its base and 0.65 of that at its top. */
    softmesh_cells(&a->mesh, 3u, 3u, 12u, 0.05, NULL);
    n = 3u * (size_t)a->mesh.nodes;
    for (uint32_t i = 0u; i < a->mesh.nodes; ++i) {
        double      *p = &a->mesh.rest[3u * i];
        const double box[3] = {p[0], p[1], p[2]}, wide = 1.0 - 0.35 * box[2] / 0.6;
        p[0] = wide * (box[1] - 0.075); p[1] = box[2]; p[2] = wide * (box[0] - 0.075);
    }
    a->tip      = (int)a->mesh.nodes - 11;
    a->fitted   = (double complex *)malloc(MAX_RANK * sizeof(double complex));
    a->reduced  = (double complex *)malloc(MAX_RANK * sizeof(double complex));
    a->state    = (double *)malloc(2u * n * sizeof(double));
    a->accel    = (double *)malloc(n * sizeof(double));
    a->place    = (double *)malloc(n * sizeof(double));
    a->controls = (double *)malloc(4u * n * sizeof(double));
    for (int s = 0; s < SLOTS; ++s) a->z[s] = (double complex *)malloc(MAX_RANK * sizeof(double complex));
    kick(a, k_push[0], a->controls);
    kick(a, k_push[1], a->controls + 2u * n);
    for (size_t i = 0; i < 4u * n; ++i) a->controls[i] /= DATA_PUSH;

    a->model = train(a, 3, k_push, k_length);
    canvas_open(&a->demo, &a->canvas, 120000u, CANVAS_RGB(38, 42, 52));
    build(a);
    if (a->demo.headless && !a->demo.frames) a->demo.frames = 60u;
}

static double noise(uint32_t *state) {
    *state = *state * 1664525u + 1013904223u;
    return (double)(*state >> 8) / 8388608.0 - 1.0;
}

static void app_check(App *a) {
    static const uint32_t k_ranks[8] = {4u, 8u, 12u, 16u, 20u, 30u, 40u, 60u};
    const size_t n = 3u * (size_t)a->mesh.nodes, dim = 2u * n;
    const double lame_mu = YOUNG / (2.0 * (1.0 + POISSON)), lame_lambda = YOUNG * POISSON / ((1.0 + POISSON) * (1.0 - 2.0 * POISSON));
    Demo        *d = &a->demo;
    double      *truth = (double *)malloc(1001u * dim * sizeof(double)), *other = (double *)malloc(1001u * dim * sizeof(double));
    double      *x = (double *)malloc(dim * sizeof(double)), *y = (double *)malloc(dim * sizeof(double));
    double       full_ms, one_step[8], long_run[8], kept[SLOTS];
    uint32_t     rng = 5u;
    char         label[200];

    a->scene = 0u; a->rank = 20u; a->many = 10u; a->push = (float)DATA_PUSH; a->damp = 0.0f; a->unclamped = false;

    /* 1. The elastic body. */
    {
        KdBody *b = body(a, DAMPING);
        double *u = (double *)calloc(n, sizeof(double)), *way = (double *)malloc(n * sizeof(double)), *force = (double *)malloc(n * sizeof(double));
        double *change = (double *)malloc(n * sizeof(double)), *plus = (double *)malloc(n * sizeof(double)), *minus = (double *)malloc(n * sizeof(double));
        double  up, down, slope = 0.0, wrong = 0.0, all = 0.0, volume = 0.0, largest = 0.0, residual = 0.0, rise = -1e300, before, start;
        int     newton = 0, cg = 0;
        clock_t began;

        for (size_t i = 0; i < n; ++i) { u[i] = 0.01 * noise(&rng); way[i] = noise(&rng); }
        kd_body_energy(b, u, force, way, change);
        for (size_t i = 0; i < n; ++i) u[i] += 1e-6 * way[i];
        up = kd_body_energy(b, u, plus, NULL, NULL);
        for (size_t i = 0; i < n; ++i) u[i] -= 2e-6 * way[i];
        down = kd_body_energy(b, u, minus, NULL, NULL);
        for (size_t i = 0; i < n; ++i) {
            slope -= force[i] * way[i];
            wrong += ((plus[i] - minus[i]) / 2e-6 - change[i]) * ((plus[i] - minus[i]) / 2e-6 - change[i]);
            all += change[i] * change[i];
        }
        demo_check(d, "nodes moved at random by up to 1 cm: (energy difference along a direction) / (-force . direction) - 1", (up - down) / 2e-6 / slope - 1.0, "within 1e-7", fabs((up - down) / 2e-6 / slope - 1.0) < 1e-7);
        demo_check(d, "    |force difference - change of the force| / |change of the force|", sqrt(wrong / all), "< 1e-6", sqrt(wrong / all) < 1e-6);

        memset(u, 0, n * sizeof(double));
        up = kd_body_energy(b, u, force, NULL, NULL);
        for (size_t i = 0; i < n; ++i) largest = fmax(largest, fabs(force[i]));
        for (int t = 0; t < b->tets; ++t) volume += b->volume[t];
        demo_check(d, "at rest: energy (J)", up, "< 1e-9", fabs(up) < 1e-9);
        demo_check(d, "    largest force (N)", largest, "< 1e-9", largest < 1e-9);
        for (size_t i = 0; i < n; ++i) u[i] = i % 3u == 0u ? 1e-4 * a->mesh.rest[i] : 0.0;
        up = kd_body_energy(b, u, NULL, NULL, NULL) / (0.5 * (lame_lambda + 2.0 * lame_mu) * 1e-8 * volume);
        demo_check(d, "stretched by 1e-4 along x, held across: energy / (volume (lambda + 2 mu) / 2 x strain^2), Lame's lambda and mu", up, "within 1e-3 of 1", fabs(up - 1.0) < 1e-3);
        for (size_t i = 0; i < n; ++i) u[i] = i % 3u == 0u ? 1e-4 * a->mesh.rest[i + 1u] : 0.0;
        up = kd_body_energy(b, u, NULL, NULL, NULL) / (0.5 * lame_mu * 1e-8 * volume);
        demo_check(d, "sheared by 1e-4: energy / (volume mu / 2 x shear^2)", up, "within 1e-3 of 1", fabs(up - 1.0) < 1e-3);

        /* 40 pushed steps, 200 free ones */
        spread(a, (const double[3]){DATA_PUSH, 0.0, 0.0}, way);
        began = clock();
        for (int s = 0; s < 240; ++s) {
            double moving = 0.0, wrong_here = 0.0, all_here = 0.0, energy;
            memcpy(plus, b->u, n * sizeof(double));
            memcpy(minus, b->v, n * sizeof(double));
            before = s ? before : 0.0;
            kd_body_step(b, STEP, s < 40 ? way : NULL);
            newton += b->newton;
            cg += b->cg;
            energy = kd_body_energy(b, b->u, force, NULL, NULL);
            for (size_t i = 0; i < n; ++i) {
                /* mass ((1 + h c) (u' - u) / h - v - h a) / h = force(u') */
                const double left = b->fixed[i / 3u] ? 0.0 : b->mass[i / 3u] * ((1.0 + STEP * DAMPING) * (b->u[i] - plus[i]) / STEP - minus[i] - (s < 40 ? STEP * way[i] : 0.0)) / STEP;
                wrong_here += b->fixed[i / 3u] ? 0.0 : (left - force[i]) * (left - force[i]);
                all_here += b->fixed[i / 3u] ? 0.0 : force[i] * force[i];
                moving += 0.5 * b->mass[i / 3u] * b->v[i] * b->v[i];
            }
            residual = fmax(residual, sqrt(wrong_here / all_here));
            if (s == 39) start = energy + moving;
            if (s > 39) rise = fmax(rise, (energy + moving - before) / start);
            before = energy + moving;
        }
        full_ms = 1000.0 * (double)(clock() - began) / CLOCKS_PER_SEC / 240.0;
        demo_check(d, "40 pushed steps and 200 free ones: largest |mass x (change of velocity, damping, push) - force| / |force| after a step", residual, "< 1e-8", residual < 1e-8);
        demo_check(d, "    Newton iterations a step", newton / 240.0, "< 4", newton / 240.0 < 4.0);
        demo_check(d, "    conjugate gradient iterations a Newton iteration", (double)cg / newton, "< 25", (double)cg / newton < 25.0);
        demo_check(d, "    most that kinetic + elastic energy rises in a free step, over the energy when the push ends", rise, "< 0", rise < 0.0);
        demo_check(d, "    time of a step (ms), with the check's own work", full_ms, "(reported)", true);
        free(u); free(way); free(force); free(change); free(plus); free(minus);
        kd_body_close(b);
    }

    /* 2. The decomposition on a system whose eigenvalues are known: 6 hidden numbers turned and shrunk by three
     * 2 x 2 blocks, seen through a random 40 x 6 matrix. */
    {
        static const double k_size[3] = {0.99, 0.95, 1.0}, k_angle[3] = {0.3, 1.1, 2.0};
        double         see[40 * 6], hidden[6], xs[40 * 24], ys[40 * 24], state[40], back[40], real[12 * 12];
        double         worst = 0.0, unreal;
        double complex z[6], one[6], push[6];
        Kd            *k;

        for (int i = 0; i < 240; ++i) see[i] = noise(&rng);
        for (int i = 0; i < 6; ++i) hidden[i] = noise(&rng);
        for (int t = 0; t <= 24; ++t) {
            for (int i = 0; i < 40; ++i) {
                double sum = 0.0;
                for (int j = 0; j < 6; ++j) sum += see[6 * i + j] * hidden[j];
                if (t < 24) xs[40 * t + i] = sum;
                if (t > 0) ys[40 * (t - 1) + i] = sum;
            }
            for (int j = 0; j < 3; ++j) {
                const double p = hidden[2 * j], q = hidden[2 * j + 1];
                hidden[2 * j]     = k_size[j] * (cos(k_angle[j]) * p - sin(k_angle[j]) * q);
                hidden[2 * j + 1] = k_size[j] * (sin(k_angle[j]) * p + cos(k_angle[j]) * q);
            }
        }
        k = kd_open(40, 24, xs, ys);
        for (int i = 0; i < 24; ++i)
            for (int j = 0; j < 24; ++j) {
                double left = 0.0, right = 0.0;
                for (int e = 0; e < 40; ++e) left += k->left[40 * i + e] * k->left[40 * j + e];
                for (int e = 0; e < 24; ++e) right += k->right[24 * i + e] * k->right[24 * j + e];
                worst = fmax(worst, fmax(i < 6 && j < 6 ? fabs(left - (i == j)) : 0.0, fabs(right - (i == j))));
            }
        demo_check(d, "40 numbers that follow 6 hidden ones, 24 pairs: largest entry of U'U - 1 (6 columns) and V'V - 1", worst, "< 1e-12", worst < 1e-12);
        worst = 0.0;
        for (int t = 0; t < 24; ++t)
            for (int i = 0; i < 40; ++i) {
                double sum = 0.0;
                for (int e = 0; e < 24; ++e) sum += k->left[40 * e + i] * k->sigma[e] * k->right[24 * e + t];
                worst = fmax(worst, fabs(sum - xs[40 * t + i]));
            }
        demo_check(d, "    largest entry of U S V' - X", worst, "< 1e-12", worst < 1e-12);
        demo_check(d, "    seventh singular value over the first", k->sigma[6] / k->sigma[0], "< 1e-12", k->sigma[6] / k->sigma[0] < 1e-12);
        kd_rank(k, 6);
        worst = 0.0;
        for (int j = 0; j < 6; ++j) {
            const double complex want = k_size[j / 2] * cexp((j % 2 ? -I : I) * k_angle[j / 2]);
            double               nearest = 1e300;
            for (int i = 0; i < 6; ++i) nearest = fmin(nearest, cabs(k->lambda[i] - want));
            worst = fmax(worst, nearest);
        }
        demo_check(d, "    rank 6: farthest that one of the six eigenvalues size x exp(+-i angle) is from a found one", worst, "< 1e-10", worst < 1e-10);
        worst = 0.0;
        for (int i = 0; i < 6; ++i)
            for (int j = 0; j < 6; ++j) {
                double complex image = 0.0, unit = 0.0;
                for (int e = 0; e < 6; ++e) {
                    image += k->reduced[6 * i + e] * k->modes[6 * e + j];
                    unit += k->modes[6 * i + e] * k->inverse[6 * e + j];
                }
                worst = fmax(worst, fmax(cabs(image - k->modes[6 * i + j] * k->lambda[j]), cabs(unit - (i == j))));
            }
        demo_check(d, "    largest entry of K' phi - phi Lambda and of phi phi^-1 - 1", worst, "< 1e-12", worst < 1e-12);
        demo_check(d, "    QR sweeps for the eigenvalues", k->sweeps, "(reported)", true);

        memcpy(state, xs, sizeof state);
        kd_project(k, state, z);
        memcpy(one, z, sizeof one);
        kd_advance(k, 24.0, NULL, z);
        unreal = kd_lift(k, z, back);
        worst = 0.0;
        for (int i = 0; i < 40; ++i) worst = fmax(worst, fabs(back[i] - ys[40 * 23 + i]));
        demo_check(d, "    24 steps at once from the first state (eq. 20): largest error of the last", worst, "< 1e-10", worst < 1e-10);
        demo_check(d, "        largest imaginary part before it is dropped", unreal, "< 1e-10", unreal < 1e-10);
        worst = 0.0;
        for (int t = 0; t < 24; ++t) kd_advance(k, 1.0, NULL, one);
        for (int i = 0; i < 6; ++i) worst = fmax(worst, cabs(one[i] - z[i]));
        demo_check(d, "    against 24 single steps", worst, "< 1e-12", worst < 1e-12);
        kd_project(k, state, z);
        memcpy(one, z, sizeof one);
        kd_advance(k, 0.5, NULL, z); kd_advance(k, 0.5, NULL, z);
        kd_advance(k, 1.0, NULL, one);
        worst = 0.0;
        for (int i = 0; i < 6; ++i) worst = fmax(worst, cabs(one[i] - z[i]));
        demo_check(d, "    two steps of half the size (eq. 21) against one", worst, "< 1e-12", worst < 1e-12);

        /* the same kick before each of 7 steps */
        kd_project(k, &xs[40 * 5], push);
        memcpy(one, z, sizeof one);
        for (int t = 0; t < 7; ++t)
            for (int i = 0; i < 6; ++i) one[i] = k->lambda[i] * (one[i] + push[i]);
        kd_advance(k, 7.0, push, z);
        worst = 0.0;
        for (int i = 0; i < 6; ++i) worst = fmax(worst, cabs(one[i] - z[i]));
        demo_check(d, "    a kick before each of 7 steps: the closed sum (eq. 25) against 7 times kick and step", worst, "< 1e-12", worst < 1e-12);

        /* the real operator */
        kd_real(k, 5.0, real);
        memcpy(one, z, sizeof one);
        kd_advance(k, 5.0, NULL, one);
        worst = 0.0;
        for (int i = 0; i < 12; ++i) {
            double sum = 0.0;
            for (int j = 0; j < 12; ++j) sum += real[12 * i + j] * (j < 6 ? creal(z[j]) : cimag(z[j - 6]));
            worst = fmax(worst, fabs(sum - (i < 6 ? creal(one[i]) : cimag(one[i - 6]))));
        }
        demo_check(d, "    the real operator of section 5.2 for 5 steps against Lambda^5, on a state that came from a real one", worst, "< 1e-12", worst < 1e-12);
        kd_close(k);
    }

    /* 3. The fitted operator against runs it was not fitted to. `other`: a push along the diagonal of x and z. */
    {
        const double diagonal[3] = {0.7071 * DATA_PUSH, 0.0, 0.7071 * DATA_PUSH}, along[3] = {DATA_PUSH, 0.0, 0.0}, across[3] = {0.0, 0.0, DATA_PUSH};
        const double half[3] = {0.5 * DATA_PUSH, 0.0, 0.0}, twice[3] = {2.0 * DATA_PUSH, 0.0, 0.0};
        double       value, bare, whole;
        int          rises = 0, pairs = 0;
        Kd          *bare_model, *whole_model;
        double      *xs, *ys;

        demo_check(d, "the fit: pairs of states", a->model->pairs, "(reported)", true);
        demo_check(d, "    singular value 10 over the first", a->model->sigma[9] / a->model->sigma[0], "(reported)", true);
        demo_check(d, "    singular value 20 over the first", a->model->sigma[19] / a->model->sigma[0], "(reported)", true);
        demo_check(d, "    singular value 60 over the first", a->model->sigma[59] / a->model->sigma[0], "(reported)", true);
        run(a, DAMPING, along, 125, 875, truth);
        run(a, DAMPING, diagonal, 125, 125, other);
        kick(a, diagonal, y);
        for (int r = 0; r < 8; ++r) {
            double wrong = 0.0, all = 0.0, largest = 0.0;
            a->rank = k_ranks[r];
            fit(a);
            for (uint32_t i = 0u; i < a->rank; ++i) largest = fmax(largest, cabs(a->fitted[i]));
            for (int s = 0; s < 250; ++s) {
                for (size_t i = 0; i < dim; ++i) x[i] = other[(size_t)s * dim + i] + (s < 125 ? y[i] : 0.0);
                kd_project(a->model, x, a->reduced);
                kd_advance(a->model, 1.0, NULL, a->reduced);
                kd_lift(a->model, a->reduced, x);
                for (size_t i = 0; i < dim; ++i) {
                    wrong += (x[i] - other[(size_t)(s + 1) * dim + i]) * (x[i] - other[(size_t)(s + 1) * dim + i]);
                    all += other[(size_t)(s + 1) * dim + i] * other[(size_t)(s + 1) * dim + i];
                }
            }
            one_step[r] = 100.0 * wrong / all;
            rises += r && one_step[r] > one_step[r - 1];
            snprintf(label, sizeof label, "rank %u: error of one step on a run pushed along the diagonal, which the fit never saw (%%)", a->rank);
            demo_check(d, label, one_step[r], "(reported)", true);
            demo_check(d, "    eigenvalues outside the unit circle", a->outside, "(reported)", true);
            demo_check(d, "    largest |eigenvalue|", largest, "(reported)", true);
            a->unclamped = true;
            edit(a, 0.0);
            demo_check(d, "    a push along x as in the data, then free, 1000 steps in all (the data stops at 250): error as fitted (%)", replay(a, along, 125, 875, 1.0, truth), "(reported)", true);
            a->unclamped = false;
            edit(a, 0.0);
            long_run[r] = replay(a, along, 125, 875, 1.0, truth);
            demo_check(d, "    with the eigenvalues outside put on the circle (%)", long_run[r], r > 1 ? "< 1" : "(reported)", r < 2 || long_run[r] < 1.0);
        }
        demo_check(d, "figure 9: error of one step at rank 20 over that at rank 4", one_step[4] / one_step[0], "< 0.01", one_step[4] / one_step[0] < 0.01);
        demo_check(d, "    times it rises from one of the eight ranks to the next", rises, "(reported)", true);

        /* The same with less data: one run, pushed along x for all of its 250 steps, no free steps. */
        {
            static const double k_one[1][3] = {{DATA_PUSH, 0.0, 0.0}};
            static const int    k_all[1][2] = {{250, 0}};
            Kd                 *keep = a->model;
            a->model = train(a, 1, k_one, k_all);
            for (int r = 2; r < 7; ++r) {
                double largest = 0.0;
                a->rank = k_ranks[r];
                fit(a);
                for (uint32_t i = 0u; i < a->rank; ++i) largest = fmax(largest, cabs(a->fitted[i]));
                snprintf(label, sizeof label, "fitted to one pushed run of 250 steps only, rank %u: eigenvalues outside the unit circle", a->rank);
                demo_check(d, label, a->outside, "(reported)", true);
                demo_check(d, "    largest |eigenvalue|", largest, "(reported)", true);
                a->unclamped = true;
                edit(a, 0.0);
                demo_check(d, "    the 1000 steps of above, as fitted (%)", replay(a, along, 125, 875, 1.0, truth), "(reported)", true);
                a->unclamped = false;
                edit(a, 0.0);
                demo_check(d, "    with the eigenvalues outside put on the circle (%)", replay(a, along, 125, 875, 1.0, truth), "(reported)", true);
            }
            kd_close(a->model);
            a->model = keep;
        }

        a->rank = 20u;
        fit(a);
        edit(a, 0.0);
        value = replay(a, along, 125, 125, 1.0, truth);
        demo_check(d, "rank 20, eigenvalues on the circle. The run along x of the data again: error (%)", value, "< 0.5", value < 0.5);
        value = replay(a, diagonal, 125, 125, 1.0, other);
        demo_check(d, "    a push along the diagonal of x and z, not in the data (figure 11) (%)", value, "< 2", value < 2.0);
        run(a, DAMPING, across, 125, 125, other);
        value = replay(a, across, 125, 125, 1.0, other);
        demo_check(d, "    the run along z of the data again (%)", value, "< 0.5", value < 0.5);
        run(a, DAMPING, half, 125, 125, other);
        value = replay(a, half, 125, 125, 1.0, other);
        demo_check(d, "    half the push (figure 5) (%)", value, "< 1", value < 1.0);
        run(a, DAMPING, twice, 125, 125, other);
        value = replay(a, twice, 125, 125, 1.0, other);
        demo_check(d, "    twice the push (%)", value, "< 5", value < 5.0);
        value = 0.0;
        for (int s = 0; s <= 250; ++s) value = fmax(value, fabs(other[(size_t)s * dim + 3u * (size_t)a->tip]));
        demo_check(d, "        the tip then moves by up to (m; the finger is 0.6 m high)", value, "(reported)", true);

        /* Figure 4: a state without its second half. Both fits here see only the free steps 125 to 374 of the run
         * along x and go on from step 125. */
        xs = (double *)malloc(84u * dim * sizeof(double));
        ys = (double *)malloc(84u * dim * sizeof(double));
        for (int s = 125; s < 375; s += EVERY, ++pairs) {
            memcpy(&xs[(size_t)pairs * n], &truth[(size_t)s * dim], n * sizeof(double));
            memcpy(&ys[(size_t)pairs * n], &truth[(size_t)(s + 1) * dim], n * sizeof(double));
        }
        bare_model = kd_open((int)n, pairs, xs, ys);
        for (int p = 0; p < pairs; ++p) {
            memcpy(&xs[(size_t)p * dim], &truth[(size_t)(125 + EVERY * p) * dim], dim * sizeof(double));
            memcpy(&ys[(size_t)p * dim], &truth[(size_t)(126 + EVERY * p) * dim], dim * sizeof(double));
        }
        whole_model = kd_open((int)dim, pairs, xs, ys);
        for (int which = 0; which < 2; ++which) {
            Kd    *k = which ? whole_model : bare_model;
            double wrong = 0.0, all = 0.0, largest = 0.0;
            kd_rank(k, 20);
            for (int i = 0; i < 20; ++i) largest = fmax(largest, cabs(k->lambda[i]));
            kd_project(k, &truth[125u * dim], a->reduced);
            for (int s = 126; s <= 1000; ++s) {
                kd_advance(k, 1.0, NULL, a->reduced);
                kd_lift(k, a->reduced, x);
                for (size_t i = 0; i < n; ++i) {
                    wrong += (x[i] - truth[(size_t)s * dim + i]) * (x[i] - truth[(size_t)s * dim + i]);
                    all += truth[(size_t)s * dim + i] * truth[(size_t)s * dim + i];
                }
            }
            snprintf(label, sizeof label, "figure 4: fitted to 84 free steps, rank 20, as fitted, 875 steps on: %s: error (%%)", which ? "the state [u; u - u before] (eq. 18)" : "the state u alone");
            demo_check(d, label, 100.0 * wrong / all, "(reported)", true);
            demo_check(d, "    largest |eigenvalue|", largest, "(reported)", true);
            *(which ? &whole : &bare) = 100.0 * wrong / all;
        }
        demo_check(d, "    error with u alone over error with both halves", bare / whole, "> 1", bare > whole);
        kd_close(bare_model); kd_close(whole_model);
        free(xs); free(ys);
    }

    /* 4. Figure 2: scene 0 for 500 steps, a push for 100 of them. */
    {
        double         early[SLOTS] = {0.0, 0.0, 0.0}, late[SLOTS] = {0.0, 0.0, 0.0}, worst = 0.0, unreal, ten, single, model_us;
        double complex z[MAX_RANK], one[MAX_RANK];
        double        *real = (double *)malloc(4u * 20u * 20u * sizeof(double));
        const double   along[3] = {DATA_PUSH, 0.0, 0.0};
        const double  *u;
        int            negative = 0;
        clock_t        began;

        a->scene = 0u; a->rank = 20u; a->many = 10u;
        build(a);
        for (int s = 1; s <= 500; ++s) {
            advance(a);
            for (int slot = 0; slot < SLOTS && s % 10 == 0; ++slot) {
                const double moving = observe(a, slot, &u);
                early[slot] += s > 100 && s <= 200 ? moving : 0.0;
                late[slot] += s > 400 ? moving : 0.0;
            }
        }
        for (int slot = 0; slot < SLOTS; ++slot) kept[slot] = late[slot] / early[slot];
        ten = 100.0 * a->error[0] / a->error[1];
        demo_check(d, "a push for 0.4 s, then free. Kinetic energy in the fifth 0.4 s over that in the second: implicit Euler, step 4 ms", kept[0], "(reported)", true);
        demo_check(d, "    the operator, 10 steps at once", kept[1], "within 5 % of the line above", fabs(kept[1] / kept[0] - 1.0) < 0.05);
        demo_check(d, "    implicit Euler, step 40 ms", kept[2], "< 0.2 of the first line", kept[2] < 0.2 * kept[0]);
        demo_check(d, "    the operator against the 4 ms run, over every tenth step (%)", ten, "< 0.5", ten < 0.5);
        run(a, DAMPING, along, 100, 400, truth);
        edit(a, 0.0);
        single = replay(a, along, 100, 400, 1.0, truth);
        demo_check(d, "    stepping one at a time instead, over every step (%)", single, "< 0.5", single < 0.5);
        single = replay(a, along, 100, 400, 2.5, truth);
        demo_check(d, "    with a step of 2.5 x 4 ms (eq. 21), over every second of its steps (%)", single, "< 0.5", single < 0.5);

        /* ten steps at once against ten steps, from a state of that run */
        kd_project(a->model, &truth[150u * dim], z);
        memcpy(one, z, sizeof one);
        kd_advance(a->model, 10.0, NULL, z);
        for (int t = 0; t < 10; ++t) kd_advance(a->model, 1.0, NULL, one);
        for (int i = 0; i < 20; ++i) worst = fmax(worst, cabs(one[i] - z[i]) / (cabs(z[i]) + 1e-300));
        demo_check(d, "    10 steps at once against 10 single steps: largest relative difference of a reduced coordinate", worst, "< 1e-12", worst < 1e-12);
        unreal = kd_lift(a->model, z, x);
        demo_check(d, "    largest imaginary part of the lifted state, over the tip's displacement", unreal / fabs(truth[160u * dim + 3u * (size_t)a->tip]), "< 1e-10", unreal / fabs(truth[160u * dim + 3u * (size_t)a->tip]) < 1e-10);
        for (int i = 0; i < 20; ++i) negative += fabs(cimag(a->model->lambda[i])) < 1e-12 && creal(a->model->lambda[i]) < 0.0;
        demo_check(d, "    negative real eigenvalues (their fractional powers are not real)", negative, "(reported)", true);
        kd_project(a->model, &truth[150u * dim], z);
        kd_real(a->model, 10.0, real);
        worst = 0.0;
        for (int i = 0; i < 40; ++i) {
            double sum = 0.0;
            for (int j = 0; j < 40; ++j) sum += real[40 * i + j] * (j < 20 ? creal(z[j]) : cimag(z[j - 20]));
            worst = fmax(worst, fabs(sum - (i < 20 ? creal(one[i]) : cimag(one[i - 20]))) / (cabs(one[i % 20]) + 1e-300));
        }
        demo_check(d, "    the real operator of section 5.2 for 10 steps against Lambda^10: largest relative difference", worst, "< 1e-9", worst < 1e-9);

        began = clock();
        for (int t = 0; t < 20000; ++t) {
            kd_advance(a->model, 10.0, a->reduced, z);
            kd_lift(a->model, z, x);
        }
        model_us = 1e6 * (double)(clock() - began) / CLOCKS_PER_SEC / 20000.0;
        demo_check(d, "    time of 10 steps at once with the lift to all nodes (microseconds)", model_us, "(reported)", true);
        demo_check(d, "    time of 10 implicit Euler steps over that", 10.0 * 1000.0 * full_ms / model_us, "> 100", 10.0 * 1000.0 * full_ms / model_us > 100.0);
        free(real);
    }

    /* 5. Section 7.2: scaling the eigenvalues by 1 - mu takes mu of the swing away each step, which a damping
     * of 2 mu / h in the body also does. */
    for (int percent = 1; percent <= 2; ++percent) {
        const double along[3] = {DATA_PUSH, 0.0, 0.0};
        double       as_fitted, edited;
        run(a, DAMPING + 2.0 * 0.01 * percent / STEP, along, 100, 400, truth);
        edit(a, 0.0);
        as_fitted = replay(a, along, 100, 400, 1.0, truth);
        edit(a, 0.01 * percent);
        edited = replay(a, along, 100, 400, 1.0, truth);
        snprintf(label, sizeof label, "eigenvalues x %.2f against implicit Euler with %g/s more damping, a push and 1.6 s free (%%)", 1.0 - 0.01 * percent, 2.0 * 0.01 * percent / STEP);
        demo_check(d, label, edited, "(reported)", true);
        demo_check(d, "    the operator as fitted against that run (%)", as_fitted, "more than 10 times the line above", as_fitted > 10.0 * edited);
    }

    /* 6. Section 7.3: scene 3 for four goals of 320 steps each. */
    {
        double per_step[1280], size = 0.0, most = 0.0, mean = 0.0, under = 0.0, hardest = 0.0;
        a->scene = 3u; a->many = 10u; a->damp = 0.0f;
        build(a);
        for (int s = 1; s <= 1280; ++s) {
            const double *u;
            double        wrong = 0.0;
            advance(a);
            hardest = fmax(hardest, hypot(a->control[0], a->control[1]));
            if (s % 10) continue;
            observe(a, 1, &u);
            for (size_t i = 0; i < n; ++i) {
                wrong += (u[i] - a->full->u[i]) * (u[i] - a->full->u[i]);
                size += a->full->u[i] * a->full->u[i] / 128.0;
            }
            per_step[s / 10 - 1] = wrong;
            if (s % 320 == 80 || s % 320 == 0) {   /* a->goal is still the one of the 320 steps that end here */
                const double *tip = &a->full->u[3 * a->tip];
                const double  miss = hypot(tip[0] - a->goal[0], tip[2] - a->goal[1]), speed = hypot(a->full->v[3 * a->tip], a->full->v[3 * a->tip + 2]);
                snprintf(label, sizeof label, "goal (%.2f, %.2f) m, pushes from the operator given to implicit Euler: its tip misses after %s by (m)", a->goal[0], a->goal[1], s % 320 == 80 ? "0.32 s" : "1.28 s");
                demo_check(d, label, miss, s % 320 == 0 ? "< 0.003" : "(reported)", s % 320 != 0 || miss < 0.003);
                demo_check(d, "    speed of the tip then (m/s)", speed, s % 320 == 0 ? "< 0.05" : "(reported)", s % 320 != 0 || speed < 0.05);
            }
        }
        for (int i = 0; i < 128; ++i) {
            most = fmax(most, 100.0 * per_step[i] / size);
            mean += 100.0 * per_step[i] / size / 128.0;
            under += 100.0 * per_step[i] / size < 2.0 ? 1.0 / 128.0 : 0.0;
        }
        demo_check(d, "    hardest push (m/s^2; the data has 5)", hardest, "(reported)", true);
        demo_check(d, "    operator against implicit Euler under those pushes, |difference|^2 of a step over the mean |u|^2: mean (%)", mean, "< 2", mean < 2.0);
        demo_check(d, "    largest (%); the paper's finger, which is moved slowly: under 6", most, "(reported)", true);
        demo_check(d, "    share of the steps under 2 %; the paper: most", under, "> 0.5", under > 0.5);
    }
    free(truth); free(other); free(x); free(y);
}

int main(int argc, char **argv) {
    static App app;
    static const DemoOption options[] = {
        {"--scene", .u32 = &app.scene, .lo = 0, .hi = 3, .help = "0 step size, 1 pushes the fit never saw, 2 damping by eigenvalues, 3 control of the tip"},
        {"--rank", .u32 = &app.rank, .lo = 2, .hi = MAX_RANK, .help = "eigenvalues kept"},
        {"--many", .u32 = &app.many, .lo = 1, .hi = 100, .help = "steps of 4 ms the operator takes at once"},
        {"--push", .f32 = &app.push, .lo = 0, .hi = 12, .help = "m/s^2 (scenes 0 to 2; the data has 5)"},
        {"--damp", .f32 = &app.damp, .lo = 0, .hi = 0.05, .help = "the eigenvalues are scaled by 1 minus this (not scene 2)"},
        {"--unclamped", .flag = &app.unclamped, .help = "leave eigenvalues outside the unit circle where they are"},
        {0}};
    Demo *d = &app.demo;
    app.rank  = 20u;
    app.many  = 10u;
    app.push  = (float)DATA_PUSH;
    demo_defaults(d, "koopman", "Low-Rank Koopman Deformables");
    demo_open(d, options, argc, argv);
    app_init(&app);
    while (demo_running(d)) {
        if (d->ui) app_ui(&app, d->ui);
        app_frame(&app);
    }
    if (d->check) app_check(&app);
    return demo_close(d);
}
