/* avbd — Augmented Vertex Block Descent: rigid boxes, contacts, friction, joints, springs.
 *
 *   Draw -> Present          (the method runs in C: method.c)
 *
 * Giles, Diaz, Yuksel: "Augmented Vertex Block Descent" (SIGGRAPH 2025), in
 * the plane, after the authors' 2-D reference demo. Scene 0 a pyramid of
 * boxes, scene 1 a rope of hard joints carrying a box 200 times a link's
 * mass, scene 2 the paper's figure 2 (two springs, stiffness ratio 10 000;
 * AVBD left, plain VBD right), scene 3 boxes of different friction on a slope.
 *
 * --check tests the contact routine, the derivatives, free fall, the rope
 * (joint gaps and the force in the top joint), figure 2, a stack, friction
 * against Coulomb's law, and what happens to a box that starts inside the
 * ground.
 */
#include "canvas.h"
#include "method.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct App {
    Demo     demo;   /* first: see demo.h */
    Canvas   canvas;
    Avbd     world[2];   /* the second is the plain-VBD copy of scene 2 */
    uint32_t scene, built, iterations;
    bool     vbd, no_post, restart;
} App;

static void empty(Avbd *s) {
    free(s->body); free(s->force);
    memset(s, 0, sizeof *s);
    avbd_defaults(s);
}

/* The rope of scene 1 hanging straight down from a fixed box: `links` of 0.5 x 1 and a square box of side `weight`. */
static void rope(Avbd *s, int links, double weight, bool sideways) {
    const double hard[3] = {INFINITY, INFINITY, 0.0};
    int          i, before = avbd_box(s, 1.0, 1.0, 0.0, 0.5, 0.0, 12.0, 0.0);
    for (i = 1; i <= links; ++i) {
        const bool   last = i == links;
        const double half = last ? 0.5 * weight : 0.5, along = (double)i - 0.5 + (last ? half : 0.5);
        const double low[2] = {0.0, -0.5}, high[2] = {0.0, half}, right[2] = {0.5, 0.0}, left[2] = {-half, 0.0};
        const int    now = sideways ? avbd_box(s, last ? weight : 1.0, last ? weight : 0.5, 1.0, 0.5, along, 12.0, 0.0)
                                    : avbd_box(s, last ? weight : 0.5, last ? weight : 1.0, 1.0, 0.5, 0.0, 12.0 - along, 0.0);
        if (sideways) avbd_joint(s, before, now, right, left, hard);
        else avbd_joint(s, before, now, low, high, hard);
        before = now;
    }
}

/* Figure 2: a fixed block, a weak spring, a block, a spring `ratio` times stiffer, a block, with both springs at their rest length. */
static void two_springs(Avbd *s, double x, double ratio) {
    const double low[2] = {0.0, -0.25}, high[2] = {0.0, 0.25};
    const int    top = avbd_box(s, 1.0, 0.5, 0.0, 0.5, x, 10.0, 0.0);
    const int    middle = avbd_box(s, 1.0, 0.5, 1.0, 0.5, x, 8.5, 0.0), bottom = avbd_box(s, 1.0, 0.5, 1.0, 0.5, x, 7.0, 0.0);
    avbd_spring(s, top, middle, low, high, 1000.0, 1.0);
    avbd_spring(s, middle, bottom, low, high, 1000.0 * ratio, 1.0);
}

static void seed(App *a) {
    Avbd *s = &a->world[0];
    int   x, y;
    empty(s); empty(&a->world[1]);
    if (a->scene == 0u) {
        avbd_box(s, 100.0, 0.5, 0.0, 0.5, 0.0, -0.25, 0.0);
        for (y = 0; y < 14; ++y)
            for (x = 0; x < 14 - y; ++x) avbd_box(s, 1.0, 0.5, 1.0, 0.5, (double)x * 1.1 + (double)y * 0.55 - 7.0, 0.3 + (double)y * 0.6, 0.0);
    }
    if (a->scene == 1u) rope(s, 16, 10.0, true);
    if (a->scene == 2u) {
        two_springs(s, -2.0, 1e4);
        two_springs(&a->world[1], 2.0, 1e4);
        a->world[1].vbd = true;
    }
    if (a->scene == 3u) {
        const double slope = 0.35, c = cos(slope), sn = sin(slope);
        avbd_box(s, 40.0, 0.5, 0.0, 1.0, 0.0, 0.0, -slope);
        /* friction of a pair is the root of the product, and the slope has 1: 0.1 to 0.7 here; tan(slope) = 0.365 */
        for (x = 0; x < 7; ++x) {
            const double mu = 0.1 + 0.1 * (double)x, along = 12.0 - 3.0 * (double)x;
            avbd_box(s, 1.0, 0.5, 1.0, mu * mu, along * c + 0.5 * sn, -along * sn + 0.5 * c, -slope);
        }
    }
    a->built = a->scene;
}

static void app_ui(App *a, struct nk_context *ctx) {
    static const char *const k_scenes[4] = {"Pyramid", "Rope and a heavy box", "Two springs: AVBD, VBD", "Friction on a slope"};
    if (nk_begin(ctx, "Augmented VBD", nk_rect(16, 16, 320, 200), NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_TITLE)) {
        int iterations = (int)a->iterations;
        if (demo_ui_choice(ctx, "Scene", k_scenes, 4u, &a->scene)) a->restart = true;
        nk_layout_row_dynamic(ctx, 22, 1);
        nk_property_int(ctx, "Iterations", 1, &iterations, 100, 1, 1.0f);
        a->iterations = (uint32_t)iterations;
        demo_ui_bool(ctx, "Plain VBD", &a->vbd);
        demo_ui_bool(ctx, "No post-stabilisation", &a->no_post);
        nk_layout_row_dynamic(ctx, 24, 1);
        if (nk_button_label(ctx, "Restart")) a->restart = true;
    }
    nk_end(ctx);
}

/* A point of a body's frame, in the world. */
static void world_point(const AvbdBody *b, const double local[2], float at[3], float z) {
    const double c = cos(b->x[2]), s = sin(b->x[2]);
    at[0] = (float)(b->x[0] + c * local[0] - s * local[1]);
    at[1] = (float)(b->x[1] + s * local[0] + c * local[1]);
    at[2] = z;
}

static void draw(Canvas *c, const Avbd *s) {
    static const uint32_t k_colours[6] = {CANVAS_RGB(90, 160, 215), CANVAS_RGB(225, 150, 70), CANVAS_RGB(120, 195, 120),
                                          CANVAS_RGB(200, 110, 170), CANVAS_RGB(215, 200, 100), CANVAS_RGB(130, 140, 220)};
    uint32_t i;
    int      k;
    for (i = 0u; i < s->bodies; ++i) {
        const AvbdBody *b = &s->body[i];
        const double    h[2] = {0.5 * b->size[0], 0.5 * b->size[1]}, corner[4][2] = {{-h[0], -h[1]}, {h[0], -h[1]}, {h[0], h[1]}, {-h[0], h[1]}};
        float           p[4][3];
        for (k = 0; k < 4; ++k) world_point(b, corner[k], p[k], 0.1f);
        canvas_triangle(c, p[0], p[1], p[2], b->mass > 0.0 ? k_colours[i % 6u] : CANVAS_RGB(96, 102, 114));
        canvas_triangle(c, p[0], p[2], p[3], b->mass > 0.0 ? k_colours[i % 6u] : CANVAS_RGB(96, 102, 114));
        for (k = 0; k < 4; ++k) {
            p[k][2] = 0.2f;
            p[(k + 1) % 4][2] = 0.2f;
            canvas_line(c, p[k], p[(k + 1) % 4], 0.03f, CANVAS_RGB(24, 26, 32));
        }
    }
    for (i = 0u; i < s->forces; ++i) {
        const AvbdForce *f = &s->force[i];
        float            p[3], q[3];
        for (k = 0; k < f->contacts; ++k) {
            world_point(&s->body[f->a], f->contact[k].ra, p, 0.4f);
            canvas_dot(c, p, 0.05f, f->contact[k].stick ? CANVAS_RGB(240, 240, 240) : CANVAS_RGB(240, 80, 60));
        }
        if (f->kind == AVBD_CONTACT) continue;
        world_point(&s->body[f->a], f->ra, p, 0.3f);
        world_point(&s->body[f->b], f->rb, q, 0.3f);
        canvas_line(c, p, q, 0.05f, f->kind == AVBD_SPRING ? CANVAS_RGB(240, 220, 90) : CANVAS_RGB(240, 80, 60));
        canvas_dot(c, q, 0.06f, CANVAS_RGB(240, 80, 60));
    }
}

static void app_frame(App *a) {
    static const float k_view[4][3] = {{0.0f, 3.6f, 5.2f}, {5.0f, 3.5f, 11.0f}, {0.0f, 7.0f, 3.8f}, {0.0f, 0.5f, 6.5f}};
    Canvas *c = &a->canvas;
    Avbd   *s = &a->world[0];
    if (a->restart || a->built != a->scene) { seed(a); a->restart = false; }
    s->iterations = a->world[1].iterations = a->iterations;
    s->vbd            = a->vbd;
    s->post_stabilize = !a->no_post;
    avbd_step(s);
    if (a->scene == 2u) avbd_step(&a->world[1]);
    canvas_view_flat(&a->demo, c, k_view[a->scene][0], k_view[a->scene][1], k_view[a->scene][2]);
    draw(c, s);
    draw(c, &a->world[1]);
    canvas_frame(&a->demo, c);
}

/* Length of a spring now. */
static double spring_length(const Avbd *s, const AvbdForce *f) {
    float p[3], q[3];
    world_point(&s->body[f->a], f->ra, p, 0.0f);
    world_point(&s->body[f->b], f->rb, q, 0.0f);
    return hypot((double)p[0] - (double)q[0], (double)p[1] - (double)q[1]);
}

static void app_check(App *a) {
    Demo    *d = &a->demo;
    Avbd     w = {0};
    Avbd    *s = &w;
    uint32_t i, step;
    int      k, row;

    /* 1. the contact routine */
    {
        AvbdContact c[2];
        int         count;
        double      worst = 0.0;
        empty(s);
        avbd_box(s, 4.0, 1.0, 0.0, 0.5, 0.0, 0.0, 0.0);
        avbd_box(s, 1.0, 1.0, 1.0, 0.5, 0.3, 0.99, 0.0);
        avbd_box(s, 1.0, 1.0, 1.0, 0.5, 0.0, 0.5 + sqrt(0.5) - 0.02, 0.785398163397448);
        avbd_box(s, 1.0, 1.0, 1.0, 0.5, 0.0, 1.01, 0.0);
        count = avbd_collide(&s->body[1], &s->body[0], c);
        for (k = 0; k < count; ++k) {
            /* points on the upper box at its lower corners, on the lower box 0.01 above them, normal up */
            worst = fmax(worst, fabs(fabs(c[k].ra[0]) - 0.5) + fabs(c[k].ra[1] + 0.5));
            worst = fmax(worst, fabs(c[k].rb[1] - 0.5) + fabs(c[k].rb[0] - (0.3 + c[k].ra[0])));
            worst = fmax(worst, fabs(c[k].normal[0]) + fabs(c[k].normal[1] - 1.0));
        }
        demo_check(d, "box resting 0.01 deep on a box: contact points", (double)count, "= 2", count == 2);
        demo_check(d, "  their positions and normal, largest error", worst, "< 1e-12", worst < 1e-12);
        count = avbd_collide(&s->body[2], &s->body[0], c);
        demo_check(d, "box on its corner, 0.02 deep: contact points", (double)count, "= 1", count == 1);
        demo_check(d, "  the point on the lower box against the corner", fabs(c[0].rb[0]) + fabs(c[0].rb[1] - 0.5), "< 1e-9", fabs(c[0].rb[0]) + fabs(c[0].rb[1] - 0.5) < 1e-9);
        count = avbd_collide(&s->body[3], &s->body[0], c);
        demo_check(d, "box 0.01 above: contact points", (double)count, "= 0", count == 0);
    }

    /* 2. gradients J against differences of C, second derivatives H against differences of J */
    {
        const double ra[2] = {0.3, -0.2}, rb[2] = {-0.4, 0.1}, stiff[3] = {INFINITY, INFINITY, INFINITY};
        double       worst_j = 0.0, worst_h = 0.0;
        empty(s);
        avbd_box(s, 1.0, 0.5, 1.0, 0.5, 0.2, 0.1, 0.4);
        avbd_box(s, 0.8, 0.6, 1.0, 0.5, 1.3, -0.7, -0.9);
        avbd_spring(s, 0, 1, ra, rb, 100.0, 0.5);
        avbd_joint(s, 0, 1, ra, rb, stiff);
        for (i = 0u; i < 2u; ++i)
            for (k = 0; k < 2; ++k) {
                AvbdForce *f = &s->force[i];
                double     j[AVBD_ROWS][3], h[AVBD_ROWS][3][3], jp[AVBD_ROWS][3], jm[AVBD_ROWS][3], hp[AVBD_ROWS][3][3];
                int        axis, col;
                avbd_derivatives(s, f, k, j, h);
                for (axis = 0; axis < 3; ++axis) {
                    const double keep = s->body[k].x[axis], step_size = 1e-6;
                    double       plus[AVBD_ROWS], minus[AVBD_ROWS];
                    s->body[k].x[axis] = keep + step_size; avbd_constraint(s, f, 0.0); memcpy(plus, f->c, sizeof plus);  avbd_derivatives(s, f, k, jp, hp);
                    s->body[k].x[axis] = keep - step_size; avbd_constraint(s, f, 0.0); memcpy(minus, f->c, sizeof minus); avbd_derivatives(s, f, k, jm, hp);
                    s->body[k].x[axis] = keep;
                    for (row = 0; row < f->rows; ++row) {
                        worst_j = fmax(worst_j, fabs((plus[row] - minus[row]) / (2.0 * step_size) - j[row][axis]));
                        for (col = 0; col < 3; ++col) worst_h = fmax(worst_h, fabs((jp[row][col] - jm[row][col]) / (2.0 * step_size) - h[row][col][axis]));
                    }
                }
            }
        demo_check(d, "spring and joint, both bodies: gradient against differences", worst_j, "< 1e-8", worst_j < 1e-8);
        demo_check(d, "  second derivatives against differences of the gradient", worst_h, "< 1e-8", worst_h < 1e-8);
    }

    /* 3. nothing but gravity: implicit Euler's fall, y0 - g dt^2 n (n + 1) / 2 */
    {
        empty(s);
        avbd_box(s, 1.0, 0.5, 1.0, 0.5, 0.0, 50.0, 0.3);
        for (step = 0u; step < 60u; ++step) avbd_step(s);
        demo_check(d, "free fall, 60 steps: height - (y0 - g dt^2 n (n + 1) / 2)", s->body[0].x[1] - (50.0 - 10.0 / 3600.0 * 60.0 * 61.0 / 2.0), "|.| < 1e-11",
                   fabs(s->body[0].x[1] - (50.0 - 10.0 / 3600.0 * 60.0 * 61.0 / 2.0)) < 1e-11);
    }

    /* 4. hard joints and a mass ratio of 200: a rope of 10 links (0.5 kg) carrying a box of 100 kg. Hanging at rest
     * for 10 s, and let go sideways and swinging for 10 s. */
    {
        double gap[2][3] = {{0}}, widest[2][3] = {{0}}, force = 0.0;
        int    trial, sideways;
        for (sideways = 0; sideways < 2; ++sideways)
            for (trial = 0; trial < 3; ++trial) {
                empty(s);
                rope(s, 11, 10.0, sideways == 1);
                s->vbd = trial == 2; s->iterations = trial == 1 ? 30u : 10u;
                for (step = 0u; step < 600u; ++step) {
                    avbd_step(s);
                    gap[sideways][trial] = 0.0;
                    for (i = 0u; i < s->forces; ++i) {
                        avbd_constraint(s, &s->force[i], 0.0);
                        gap[sideways][trial] = fmax(gap[sideways][trial], hypot(s->force[i].c[0], s->force[i].c[1]));
                    }
                    widest[sideways][trial] = fmax(widest[sideways][trial], gap[sideways][trial]);
                }
                if (trial == 0 && sideways == 0) force = hypot(s->force[0].lambda[0], s->force[0].lambda[1]);
            }
        demo_check(d, "rope of 10 links and a box 200 times a link's mass, hanging 10 s: widest joint gap at the end, m, AVBD, 10 iterations", gap[0][0], "< 0.01", gap[0][0] < 0.01);
        demo_check(d, "  AVBD, 30 iterations", gap[0][1], "< 0.005", gap[0][1] < 0.005);
        demo_check(d, "  plain VBD, stiffness 1e6, 10 iterations: weight / stiffness = 0.00105 (reported)", gap[0][2], "", true);
        demo_check(d, "  AVBD: force in the top joint / weight below - 1", force / 1050.0 - 1.0, "|.| < 1e-3", fabs(force / 1050.0 - 1.0) < 1e-3);
        demo_check(d, "the same rope swinging 10 s: widest gap at any time, m, AVBD, 10 iterations", widest[1][0], "< 0.05", widest[1][0] < 0.05);
        demo_check(d, "  AVBD, 30 iterations (reported)", widest[1][1], "", true);
        demo_check(d, "  plain VBD, stiffness 1e6, 10 iterations (reported)", widest[1][2], "", true);
    }

    /* 5. figure 2: a fixed block, a spring of 1e3 N/m, a block, a spring of 1e7 N/m, a block, let go with the springs
     * at their rest lengths. The weak spring carries 10 N and should end 0.01 m long. */
    {
        static const uint32_t k_iterations[5] = {5u, 1u, 5u, 100u, 1000u};
        double                longest[5] = {0}, last[5], stiff = 0.0;
        int                   trial;
        for (trial = 0; trial < 5; ++trial) {
            empty(s);
            two_springs(s, 0.0, 1e4);
            s->vbd        = trial >= 2;
            s->iterations = k_iterations[trial];
            for (step = 0u; step < 600u; ++step) {
                avbd_step(s);
                longest[trial] = fmax(longest[trial], spring_length(s, &s->force[0]) - 1.0);
            }
            last[trial] = spring_length(s, &s->force[0]) - 1.0;
            if (trial == 0) stiff = spring_length(s, &s->force[1]) - 1.0;
        }
        demo_check(d, "two springs, stiffness ratio 10 000, 10 s: weak spring's stretch at the end / (weight / stiffness) - 1, AVBD, 5 iterations", last[0] / 0.01 - 1.0, "|.| < 0.01", fabs(last[0] / 0.01 - 1.0) < 0.01);
        demo_check(d, "  its longest stretch on the way, m (0.01 at rest)", longest[0], "< 0.02", longest[0] < 0.02);
        demo_check(d, "  AVBD, 1 iteration: longest stretch, m", longest[1], "< 0.03", longest[1] < 0.03);
        demo_check(d, "  plain VBD, 5 iterations: longest stretch, m (the sagging of the paper's figure 2)", longest[2], "> 0.5", longest[2] > 0.5);
        demo_check(d, "  plain VBD, 100 iterations (reported)", longest[3], "", true);
        demo_check(d, "  plain VBD, 1000 iterations: stretch at the end / (weight / stiffness) - 1", last[4] / 0.01 - 1.0, "|.| < 0.01", fabs(last[4] / 0.01 - 1.0) < 0.01);
        demo_check(d, "  AVBD, 5 iterations: the stiff spring's stretch, m; 5e-7 would be right, its stiffness is still being raised (reported)", stiff, "", true);
    }

    /* 6. a stack of 10 boxes at rest */
    {
        double fastest = 0.0;
        empty(s);
        avbd_box(s, 100.0, 1.0, 0.0, 0.5, 0.0, -0.5, 0.0);
        for (k = 0; k < 10; ++k) avbd_box(s, 1.0, 1.0, 1.0, 0.5, 0.0, 0.5 + (double)k, 0.0);
        for (step = 0u; step < 600u; ++step) avbd_step(s);
        for (i = 1u; i < s->bodies; ++i) fastest = fmax(fastest, hypot(s->body[i].v[0], s->body[i].v[1]) + fabs(s->body[i].v[2]));
        demo_check(d, "stack of 10 boxes, 10 s: fastest box, m/s", fastest, "< 1e-3", fastest < 1e-3);
        demo_check(d, "  top box: height - 9.5, m (each contact aims at 0.0005 of overlap)", s->body[10].x[1] - 9.5, "|.| < 0.01", fabs(s->body[10].x[1] - 9.5) < 0.01);
        demo_check(d, "  top box: sideways drift, m", s->body[10].x[0], "|.| < 1e-3", fabs(s->body[10].x[0]) < 1e-3);
    }

    /* 7. Coulomb friction on a slope of 0.35 rad (tan = 0.365): mu 0.5 holds, mu 0.2 slides at g (sin - mu cos) */
    {
        const double slope = 0.35, c = cos(slope), sn = sin(slope);
        double       moved[2];
        int          trial;
        for (trial = 0; trial < 2; ++trial) {
            const double mu = trial == 0 ? 0.5 : 0.2;
            double       start[2];
            empty(s);
            avbd_box(s, 60.0, 0.5, 0.0, 1.0, 0.0, 0.0, -slope);
            avbd_box(s, 1.0, 0.5, 1.0, mu * mu, 0.5 * sn, 0.5 * c, -slope);
            for (step = 0u; step < 30u; ++step) avbd_step(s);   /* settle onto the slope */
            start[0] = s->body[1].x[0]; start[1] = s->body[1].x[1];
            s->body[1].v[0] = s->body[1].v[1] = s->body[1].v[2] = 0.0;
            memset(s->body[1].v_before, 0, sizeof s->body[1].v_before);
            for (step = 0u; step < 60u; ++step) avbd_step(s);
            moved[trial] = hypot(s->body[1].x[0] - start[0], s->body[1].x[1] - start[1]);
        }
        demo_check(d, "box on a slope, friction 0.5 > tan: moved in 1 s, m", moved[0], "< 1e-3", moved[0] < 1e-3);
        demo_check(d, "  friction 0.2 < tan: moved / (g (sin - mu cos) dt^2 n (n + 1) / 2) - 1", moved[1] / (10.0 * (sn - 0.2 * c) / 3600.0 * 60.0 * 61.0 / 2.0) - 1.0, "|.| < 0.03",
                   fabs(moved[1] / (10.0 * (sn - 0.2 * c) / 3600.0 * 60.0 * 61.0 / 2.0) - 1.0) < 0.03);
    }

    /* 8. a box that starts 0.3 deep in the ground: how fast it is thrown out (section 3.6) */
    {
        double fastest[3] = {0}, depth[3];
        int    trial;
        for (trial = 0; trial < 3; ++trial) {
            empty(s);
            avbd_box(s, 100.0, 1.0, 0.0, 0.5, 0.0, -0.5, 0.0);
            avbd_box(s, 1.0, 1.0, 1.0, 0.5, 0.0, 0.2, 0.0);
            s->post_stabilize = trial == 2;
            s->alpha          = trial == 0 ? 0.0 : 0.95;
            for (step = 0u; step < 120u; ++step) {
                avbd_step(s);
                fastest[trial] = fmax(fastest[trial], s->body[1].v[1]);
            }
            depth[trial] = 0.5 - s->body[1].x[1];
        }
        demo_check(d, "box starting 0.3 m inside the ground: fastest upward speed, m/s, all error corrected at once (alpha 0; reported)", fastest[0], "", true);
        demo_check(d, "  alpha 0.95, the paper's value", fastest[1], "< 1/5 of that", fastest[1] < 0.2 * fastest[0]);
        demo_check(d, "  post-stabilisation, the reference demo's default", fastest[2], "< 1e-3", fastest[2] < 1e-3);
        demo_check(d, "  depth left after 2 s, m: alpha 0.95 (reported)", depth[1], "", true);
        demo_check(d, "  post-stabilisation", depth[2], "< 2e-3", depth[2] < 2e-3);
    }

    /* 9. the pyramid of scene 0 */
    {
        double fastest = 0.0;
        a->scene = 0u; seed(a);
        for (step = 0u; step < 300u; ++step) avbd_step(&a->world[0]);
        for (i = 1u; i < a->world[0].bodies; ++i) fastest = fmax(fastest, hypot(a->world[0].body[i].v[0], a->world[0].body[i].v[1]));
        demo_check(d, "pyramid of 105 boxes after 5 s: fastest box, m/s", fastest, "< 0.01", fastest < 0.01);
        demo_check(d, "  top box: height, m (0.25 + 13 x 0.5 = 6.75 when packed)", a->world[0].body[105].x[1], "6.7 .. 6.76", a->world[0].body[105].x[1] > 6.7 && a->world[0].body[105].x[1] < 6.76);
    }
    free(w.body); free(w.force);
}

int main(int argc, char **argv) {
    static App app;
    static const DemoOption options[] = {
        {"--scene", .u32 = &app.scene, .lo = 0, .hi = 3, .help = "0 pyramid, 1 rope and a heavy box, 2 two springs (AVBD left, VBD right), 3 friction on a slope"},
        {"--iterations", .u32 = &app.iterations, .lo = 1, .hi = 100, .help = "iterations per time step (1/60 s)"},
        {"--vbd", .flag = &app.vbd, .help = "plain vertex block descent: no multipliers, no stiffness ramp"},
        {"--no-post", .flag = &app.no_post, .help = "alpha 0.99 instead of the reference demo's post-stabilisation"},
        {0}};
    Demo *d = &app.demo;
    app.iterations = 10u; app.built = 99u;
    demo_defaults(d, "avbd", "Augmented Vertex Block Descent");
    demo_open(d, options, argc, argv);
    canvas_open(d, &app.canvas, 60000u, CANVAS_RGB(26, 28, 36));
    while (demo_running(d)) {
        if (d->ui) app_ui(&app, d->ui);
        app_frame(&app);
    }
    if (d->check) app_check(&app);
    return demo_close(d);
}
