/* method.c — Augmented Vertex Block Descent (AVBD).
 *
 * Giles, Diaz, Yuksel: "Augmented Vertex Block Descent", SIGGRAPH 2025.
 * Written after the paper and the authors' 2-D reference demo
 * (github.com/savant117/avbd-demo2d, (c) 2025 Chris Giles), whose structure,
 * constants and box-box contact routine (Box2D-lite's) this follows.
 *
 * Implicit Euler is the minimum of  |x - y|^2_M / (2 dt^2) + E(x). Vertex
 * block descent moves one body at a time to the minimum of that with all the
 * others held: one Newton step on a 3 x 3 system (x, y, angle). AVBD adds
 *
 *   hard constraints   E_j = k C^2 / 2 + lambda C, with lambda <- k C + lambda
 *                      after each iteration: the force moves into lambda and
 *                      k can stay small (eq. 8, 11)
 *   bounds             the force k C + lambda is clamped: contacts only
 *                      push, friction stays inside mu x normal force (3.2, 3.3)
 *   stiffness ramp     k starts low and grows by beta |C| an iteration, up to
 *                      the real stiffness for a soft force (eq. 12, 16)
 *   safe Hessian       the second-derivative part is replaced by the lengths
 *                      of its columns on the diagonal (3.5)
 *   error left alone   C <- C - alpha C(start of step) for hard constraints,
 *                      so old error is not turned into velocity (eq. 18)
 *   warm start         k and lambda of the last step, scaled by gamma (eq. 19)
 */
#include "method.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static double clamp(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

/* A point of a body's frame in the world, and its offset from the body's centre. */
static void arm_of(const AvbdBody *b, const double local[2], double r[2]) {
    const double c = cos(b->x[2]), s = sin(b->x[2]);
    r[0] = c * local[0] - s * local[1];
    r[1] = s * local[0] + c * local[1];
}

void avbd_defaults(Avbd *s) {
    s->dt = 1.0 / 60.0; s->gravity = 10.0; s->iterations = 10u;
    s->alpha = 0.99; s->beta = 1e5; s->gamma = 0.99;
    s->post_stabilize = true; s->vbd = false; s->vbd_hard = 1e6;
}

int avbd_box(Avbd *s, double width, double height, double density, double friction, double x, double y, double angle) {
    AvbdBody *b;
    s->body = (AvbdBody *)realloc(s->body, (size_t)(s->bodies + 1u) * sizeof(AvbdBody));
    b       = &s->body[s->bodies];
    memset(b, 0, sizeof *b);
    b->size[0] = width; b->size[1] = height;
    b->mass     = width * height * density;
    b->moment   = b->mass * (width * width + height * height) / 12.0;
    b->friction = friction;
    b->radius   = 0.5 * sqrt(width * width + height * height);
    b->x[0] = x; b->x[1] = y; b->x[2] = angle;
    return (int)s->bodies++;
}

static AvbdForce *add_force(Avbd *s, uint32_t kind, int a, int b, int rows) {
    AvbdForce *f;
    int        i;
    if (s->forces == s->capacity) {
        s->capacity = s->capacity ? 2u * s->capacity : 64u;
        s->force    = (AvbdForce *)realloc(s->force, (size_t)s->capacity * sizeof(AvbdForce));
    }
    f = &s->force[s->forces++];
    memset(f, 0, sizeof *f);
    f->kind = kind; f->a = a; f->b = b; f->rows = rows;
    for (i = 0; i < AVBD_ROWS; ++i) { f->stiffness[i] = INFINITY; f->fmin[i] = -INFINITY; f->fmax[i] = INFINITY; }
    return f;
}

void avbd_spring(Avbd *s, int a, int b, const double ra[2], const double rb[2], double stiffness, double rest) {
    AvbdForce *f = add_force(s, AVBD_SPRING, a, b, 1);
    memcpy(f->ra, ra, sizeof f->ra); memcpy(f->rb, rb, sizeof f->rb);
    f->stiffness[0] = stiffness;
    f->rest         = rest;
}

void avbd_joint(Avbd *s, int a, int b, const double ra[2], const double rb[2], const double stiffness[3]) {
    AvbdForce      *f = add_force(s, AVBD_JOINT, a, b, 3);
    const AvbdBody *bb = &s->body[b];
    double          size[2] = {bb->size[0], bb->size[1]};
    memcpy(f->ra, ra, sizeof f->ra); memcpy(f->rb, rb, sizeof f->rb);
    memcpy(f->stiffness, stiffness, 3u * sizeof(double));
    if (a >= 0) { size[0] += s->body[a].size[0]; size[1] += s->body[a].size[1]; }
    f->rest = (a >= 0 ? s->body[a].x[2] : 0.0) - bb->x[2];
    f->arm  = size[0] * size[0] + size[1] * size[1];   /* makes the angle row a length, as the reference demo does */
}

/* ---- contact points of two boxes, after Box2D-lite ---------------------- */

typedef struct Clip { double v[2]; uint32_t feature; } Clip;

/* Keep the part of the segment with n . v <= offset. */
static int clip_segment(Clip out[2], const Clip in[2], const double n[2], double offset, uint32_t side) {
    const double d0 = n[0] * in[0].v[0] + n[1] * in[0].v[1] - offset, d1 = n[0] * in[1].v[0] + n[1] * in[1].v[1] - offset;
    int          count = 0;
    if (d0 <= 0.0) out[count++] = in[0];
    if (d1 <= 0.0) out[count++] = in[1];
    if (d0 * d1 < 0.0) {
        const double t = d0 / (d0 - d1);
        out[count].v[0]    = in[0].v[0] + t * (in[1].v[0] - in[0].v[0]);
        out[count].v[1]    = in[0].v[1] + t * (in[1].v[1] - in[0].v[1]);
        out[count].feature = (d0 > 0.0 ? in[0].feature : in[1].feature) | side;
        count++;
    }
    return count;
}

int avbd_collide(const AvbdBody *a, const AvbdBody *b, AvbdContact out[2]) {
    const AvbdBody *box[2] = {a, b};
    const double    ca = cos(a->x[2]), sa = sin(a->x[2]), cb = cos(b->x[2]), sb = sin(b->x[2]);
    const double    axis[2][2][2] = {{{ca, sa}, {-sa, ca}}, {{cb, sb}, {-sb, cb}}};   /* [box][axis] */
    const double    dp[2] = {b->x[0] - a->x[0], b->x[1] - a->x[1]};
    double          best = -INFINITY, normal[2] = {0, 0}, front, side, incident_n[2];
    int             best_box = 0, best_axis = 0, w, k, i, count = 0, n;
    Clip            edge[2], clipped[2], inside[2];

    /* the face with the least overlap along its normal; a later face must be clearly better */
    for (w = 0; w < 2; ++w)
        for (k = 0; k < 2; ++k) {
            const double *u = axis[w][k];
            const AvbdBody *other = box[1 - w];
            const double  d = dp[0] * u[0] + dp[1] * u[1];
            const double  reach = 0.5 * other->size[0] * fabs(u[0] * axis[1 - w][0][0] + u[1] * axis[1 - w][0][1])
                                + 0.5 * other->size[1] * fabs(u[0] * axis[1 - w][1][0] + u[1] * axis[1 - w][1][1]);
            const double  separation = fabs(d) - 0.5 * box[w]->size[k] - reach;
            if (separation > 0.0) return 0;
            if (w + k > 0 && separation <= 0.95 * best + 0.01 * 0.5 * box[w]->size[k]) continue;
            best = separation; best_box = w; best_axis = k;
            normal[0] = d > 0.0 ? u[0] : -u[0];   /* from a to b */
            normal[1] = d > 0.0 ? u[1] : -u[1];
        }
    {
        /* reference face: on `best_box`, facing the other box */
        const AvbdBody *ref = box[best_box], *inc = box[1 - best_box];
        const double    sign = best_box == 0 ? 1.0 : -1.0, fn[2] = {sign * normal[0], sign * normal[1]};
        const double   *sn = axis[best_box][1 - best_axis], (*ia)[2] = axis[1 - best_box];
        const double    half_side = 0.5 * ref->size[1 - best_axis];
        double          corner[2][2];
        uint32_t        id[2];
        front = ref->x[0] * fn[0] + ref->x[1] * fn[1] + 0.5 * ref->size[best_axis];
        side  = ref->x[0] * sn[0] + ref->x[1] * sn[1];
        /* incident edge: the side of the other box that faces the reference face most squarely */
        incident_n[0] = -(fn[0] * ia[0][0] + fn[1] * ia[0][1]);
        incident_n[1] = -(fn[0] * ia[1][0] + fn[1] * ia[1][1]);
        if (fabs(incident_n[0]) > fabs(incident_n[1])) {
            const double sx = incident_n[0] > 0.0 ? 1.0 : -1.0;
            corner[0][0] = sx * 0.5 * inc->size[0]; corner[0][1] = -sx * 0.5 * inc->size[1];
            corner[1][0] = sx * 0.5 * inc->size[0]; corner[1][1] = sx * 0.5 * inc->size[1];
        } else {
            const double sy = incident_n[1] > 0.0 ? 1.0 : -1.0;
            corner[0][0] = sy * 0.5 * inc->size[0];  corner[0][1] = sy * 0.5 * inc->size[1];
            corner[1][0] = -sy * 0.5 * inc->size[0]; corner[1][1] = sy * 0.5 * inc->size[1];
        }
        for (i = 0; i < 2; ++i) {
            /* a corner is named by its two signs; the reference face by box and axis */
            id[i] = (corner[i][0] > 0.0 ? 1u : 0u) | (corner[i][1] > 0.0 ? 2u : 0u) | (uint32_t)best_box << 2 | (uint32_t)best_axis << 3;
            edge[i].v[0]    = inc->x[0] + ia[0][0] * corner[i][0] + ia[1][0] * corner[i][1];
            edge[i].v[1]    = inc->x[1] + ia[0][1] * corner[i][0] + ia[1][1] * corner[i][1];
            edge[i].feature = id[i];
        }
        {
            const double minus[2] = {-sn[0], -sn[1]};
            n = clip_segment(clipped, edge, minus, -side + half_side, 16u);
            if (n < 2) return 0;
            n = clip_segment(inside, clipped, sn, side + half_side, 32u);
            if (n < 2) return 0;
        }
        for (i = 0; i < 2; ++i) {
            const double separation = fn[0] * inside[i].v[0] + fn[1] * inside[i].v[1] - front;
            const double on_face[2] = {inside[i].v[0] - fn[0] * separation, inside[i].v[1] - fn[1] * separation};
            const double *pa = best_box == 0 ? on_face : inside[i].v, *pb = best_box == 0 ? inside[i].v : on_face;
            AvbdContact  *c = &out[count];
            if (separation > 0.0) continue;
            memset(c, 0, sizeof *c);
            c->normal[0] = -normal[0]; c->normal[1] = -normal[1];
            c->ra[0] = ca * (pa[0] - a->x[0]) + sa * (pa[1] - a->x[1]);
            c->ra[1] = -sa * (pa[0] - a->x[0]) + ca * (pa[1] - a->x[1]);
            c->rb[0] = cb * (pb[0] - b->x[0]) + sb * (pb[1] - b->x[1]);
            c->rb[1] = -sb * (pb[0] - b->x[0]) + cb * (pb[1] - b->x[1]);
            c->feature = inside[i].feature;
            count++;
        }
    }
    return count;
}

/* ---- forces ------------------------------------------------------------- */

/* Where a force holds a body, in the world. Body -1 is the world itself. */
static void hold(const Avbd *s, int body, const double local[2], double at[2]) {
    at[0] = local[0]; at[1] = local[1];
    if (body >= 0) {
        arm_of(&s->body[body], local, at);
        at[0] += s->body[body].x[0];
        at[1] += s->body[body].x[1];
    }
}

/* Contacts of this step, with what the same contacts had last step. Returns false when the boxes no longer touch. */
static bool contact_begin(const Avbd *s, AvbdForce *f) {
    const AvbdBody *a = &s->body[f->a], *b = &s->body[f->b];
    const AvbdForce old = *f;
    int             i, j;
    f->contacts = avbd_collide(a, b, f->contact);
    f->rows     = 2 * f->contacts;
    for (i = 0; i < f->contacts; ++i) {
        AvbdContact *c = &f->contact[i];
        double       raw[2], rbw[2], tangent[2], gap[2];
        f->penalty[2 * i] = f->penalty[2 * i + 1] = f->lambda[2 * i] = f->lambda[2 * i + 1] = 0.0;
        for (j = 0; j < old.contacts; ++j) {
            if (c->feature != old.contact[j].feature) continue;
            f->penalty[2 * i] = old.penalty[2 * j]; f->penalty[2 * i + 1] = old.penalty[2 * j + 1];
            f->lambda[2 * i]  = old.lambda[2 * j];  f->lambda[2 * i + 1]  = old.lambda[2 * j + 1];
            c->stick          = old.contact[j].stick;
            /* a contact that held last step keeps its points: static friction does not creep */
            if (c->stick) { memcpy(c->ra, old.contact[j].ra, sizeof c->ra); memcpy(c->rb, old.contact[j].rb, sizeof c->rb); }
        }
        tangent[0] = c->normal[1]; tangent[1] = -c->normal[0];
        arm_of(a, c->ra, raw); arm_of(b, c->rb, rbw);
        /* C and its gradients at the start of the step; contacts use this straight-line form all step (section 4) */
        c->jan[0] = c->normal[0];  c->jan[1] = c->normal[1];  c->jan[2] = raw[0] * c->normal[1] - raw[1] * c->normal[0];
        c->jbn[0] = -c->normal[0]; c->jbn[1] = -c->normal[1]; c->jbn[2] = -(rbw[0] * c->normal[1] - rbw[1] * c->normal[0]);
        c->jat[0] = tangent[0];    c->jat[1] = tangent[1];    c->jat[2] = raw[0] * tangent[1] - raw[1] * tangent[0];
        c->jbt[0] = -tangent[0];   c->jbt[1] = -tangent[1];   c->jbt[2] = -(rbw[0] * tangent[1] - rbw[1] * tangent[0]);
        gap[0]   = a->x[0] + raw[0] - b->x[0] - rbw[0];
        gap[1]   = a->x[1] + raw[1] - b->x[1] - rbw[1];
        c->c0[0] = c->normal[0] * gap[0] + c->normal[1] * gap[1] + AVBD_MARGIN;
        c->c0[1] = tangent[0] * gap[0] + tangent[1] * gap[1];
    }
    for (i = 0; i < f->contacts; ++i) { f->fmin[2 * i] = -INFINITY; f->fmax[2 * i] = 0.0; f->stiffness[2 * i] = f->stiffness[2 * i + 1] = INFINITY; }
    return f->contacts > 0;
}

void avbd_constraint(const Avbd *s, AvbdForce *f, double alpha) {
    double pa[2], pb[2];
    int    i;
    if (f->kind == AVBD_CONTACT) {
        const AvbdBody *a = &s->body[f->a], *b = &s->body[f->b];
        const double    mu = sqrt(a->friction * b->friction);
        double          da[3], db[3];
        for (i = 0; i < 3; ++i) { da[i] = a->x[i] - a->initial[i]; db[i] = b->x[i] - b->initial[i]; }
        for (i = 0; i < f->contacts; ++i) {
            AvbdContact *c = &f->contact[i];
            const double bound = fabs(f->lambda[2 * i]) * mu;
            f->c[2 * i]     = c->c0[0] * (1.0 - alpha) + c->jan[0] * da[0] + c->jan[1] * da[1] + c->jan[2] * da[2] + c->jbn[0] * db[0] + c->jbn[1] * db[1] + c->jbn[2] * db[2];
            f->c[2 * i + 1] = c->c0[1] * (1.0 - alpha) + c->jat[0] * da[0] + c->jat[1] * da[1] + c->jat[2] * da[2] + c->jbt[0] * db[0] + c->jbt[1] * db[1] + c->jbt[2] * db[2];
            /* friction may be at most mu x the normal force found so far */
            f->fmax[2 * i + 1] = bound;
            f->fmin[2 * i + 1] = -bound;
            c->stick           = fabs(f->lambda[2 * i + 1]) < bound && fabs(c->c0[1]) < AVBD_STICK;
        }
        return;
    }
    hold(s, f->a, f->ra, pa);
    hold(s, f->b, f->rb, pb);
    if (f->kind == AVBD_SPRING) {
        f->c[0] = hypot(pa[0] - pb[0], pa[1] - pb[1]) - f->rest;
        return;
    }
    {
        const double now[3] = {pa[0] - pb[0], pa[1] - pb[1], ((f->a >= 0 ? s->body[f->a].x[2] : 0.0) - s->body[f->b].x[2] - f->rest) * f->arm};
        /* a hard row leaves alone the share alpha of the error it started the step with (eq. 18) */
        for (i = 0; i < 3; ++i) f->c[i] = isinf(f->stiffness[i]) ? now[i] - f->c0[i] * alpha : now[i];
    }
}

void avbd_derivatives(const Avbd *s, const AvbdForce *f, int body, double j[AVBD_ROWS][3], double h[AVBD_ROWS][3][3]) {
    const double sign = body == f->a ? 1.0 : -1.0;
    double       r[2], pa[2], pb[2];
    int          i;
    memset(h, 0, sizeof(double[AVBD_ROWS][3][3]));
    if (f->kind == AVBD_CONTACT) {
        for (i = 0; i < f->contacts; ++i) {
            memcpy(j[2 * i], body == f->a ? f->contact[i].jan : f->contact[i].jbn, sizeof j[0]);
            memcpy(j[2 * i + 1], body == f->a ? f->contact[i].jat : f->contact[i].jbt, sizeof j[0]);
        }
        return;
    }
    arm_of(&s->body[body], body == f->a ? f->ra : f->rb, r);
    if (f->kind == AVBD_JOINT) {
        /* C = (point on a) - (point on b); turning a body moves its point by perp(r), and that by -r */
        j[0][0] = sign; j[0][1] = 0.0;  j[0][2] = -sign * r[1];
        j[1][0] = 0.0;  j[1][1] = sign; j[1][2] = sign * r[0];
        j[2][0] = 0.0;  j[2][1] = 0.0;  j[2][2] = sign * f->arm;
        h[0][2][2] = -sign * r[0];
        h[1][2][2] = -sign * r[1];
        return;
    }
    hold(s, f->a, f->ra, pa);
    hold(s, f->b, f->rb, pb);
    {
        /* C = |d| - rest, d = (point on a) - (point on b) */
        const double d[2] = {pa[0] - pb[0], pa[1] - pb[1]}, length = hypot(d[0], d[1]);
        const double n[2] = {d[0] / length, d[1] / length}, turn[2] = {-r[1], r[0]};
        const double dxx[2][2] = {{(1.0 - n[0] * n[0]) / length, -n[0] * n[1] / length}, {-n[0] * n[1] / length, (1.0 - n[1] * n[1]) / length}};
        const double dxr[2] = {dxx[0][0] * turn[0] + dxx[0][1] * turn[1], dxx[1][0] * turn[0] + dxx[1][1] * turn[1]};
        j[0][0] = sign * n[0]; j[0][1] = sign * n[1]; j[0][2] = sign * (n[0] * turn[0] + n[1] * turn[1]);
        h[0][0][0] = dxx[0][0]; h[0][0][1] = dxx[0][1]; h[0][1][0] = dxx[1][0]; h[0][1][1] = dxx[1][1];
        h[0][0][2] = h[0][2][0] = dxr[0];
        h[0][1][2] = h[0][2][1] = dxr[1];
        h[0][2][2] = turn[0] * dxr[0] + turn[1] * dxr[1] - sign * (n[0] * r[0] + n[1] * r[1]);
    }
}

/* a x = b for a symmetric positive definite 3 x 3 matrix, by L D L^T */
static void solve3(const double a[3][3], const double b[3], double x[3]) {
    const double d0 = a[0][0], l10 = a[1][0] / d0, l20 = a[2][0] / d0;
    const double d1 = a[1][1] - l10 * l10 * d0, l21 = (a[2][1] - l20 * l10 * d0) / d1;
    const double d2 = a[2][2] - l20 * l20 * d0 - l21 * l21 * d1;
    const double y1 = b[1] - l10 * b[0], y2 = b[2] - l20 * b[0] - l21 * y1;
    x[2] = y2 / d2;
    x[1] = y1 / d1 - l21 * x[2];
    x[0] = b[0] / d0 - l10 * x[1] - l20 * x[2];
}

void avbd_step(Avbd *s) {
    const uint32_t total = s->iterations + (s->post_stabilize && !s->vbd ? 1u : 0u);
    uint32_t       i, k, n, iteration, kept = 0u;
    int            row, c;

    /* 1. pairs of boxes near each other with no force between them yet get a contact force */
    for (i = 0u; i < s->bodies; ++i)
        for (k = i + 1u; k < s->bodies; ++k) {
            const AvbdBody *a = &s->body[i], *b = &s->body[k];
            const double    reach = a->radius + b->radius, dx = a->x[0] - b->x[0], dy = a->x[1] - b->x[1];
            bool            joined = a->mass == 0.0 && b->mass == 0.0;
            if (dx * dx + dy * dy > reach * reach) continue;
            for (n = 0u; n < s->forces && !joined; ++n) joined = (s->force[n].a == (int)i && s->force[n].b == (int)k) || (s->force[n].a == (int)k && s->force[n].b == (int)i);
            if (!joined) add_force(s, AVBD_CONTACT, (int)i, (int)k, 0);
        }

    /* 2. start the forces: this step's contacts, C at the start, stiffness and multipliers from the last step (eq. 19) */
    for (n = 0u; n < s->forces; ++n) {
        AvbdForce *f = &s->force[n];
        if (f->kind == AVBD_CONTACT && !contact_begin(s, f)) continue;   /* no longer touching: dropped */
        if (f->kind == AVBD_JOINT) {
            double pa[2], pb[2];
            hold(s, f->a, f->ra, pa); hold(s, f->b, f->rb, pb);
            f->c0[0] = pa[0] - pb[0]; f->c0[1] = pa[1] - pb[1];
            f->c0[2] = ((f->a >= 0 ? s->body[f->a].x[2] : 0.0) - s->body[f->b].x[2] - f->rest) * f->arm;
        }
        for (row = 0; row < f->rows; ++row) {
            if (s->vbd) {
                f->penalty[row] = isinf(f->stiffness[row]) ? s->vbd_hard : f->stiffness[row];
                f->lambda[row]  = 0.0;
                continue;
            }
            if (!s->post_stabilize) f->lambda[row] *= s->alpha * s->gamma;
            f->penalty[row] = fmin(clamp(f->penalty[row] * s->gamma, AVBD_PENALTY_MIN, AVBD_PENALTY_MAX), f->stiffness[row]);
        }
        s->force[kept++] = *f;
    }
    s->forces = kept;

    /* 3. start the bodies: y of eq. 2, and a first guess that includes gravity only as far as the body fell freely last step */
    for (i = 0u; i < s->bodies; ++i) {
        AvbdBody    *b = &s->body[i];
        const double fell = b->mass > 0.0 ? clamp(-(b->v[1] - b->v_before[1]) / s->dt / s->gravity, 0.0, 1.0) : 0.0;
        b->v[2] = clamp(b->v[2], -50.0, 50.0);
        for (c = 0; c < 3; ++c) {
            b->initial[c]  = b->x[c];
            b->inertial[c] = b->x[c] + s->dt * b->v[c];
            b->x[c]        = b->inertial[c];
        }
        if (b->mass > 0.0) {
            b->inertial[1] -= s->gravity * s->dt * s->dt;
            b->x[1]        -= fell * s->gravity * s->dt * s->dt;
        }
    }

    for (iteration = 0u; iteration < total; ++iteration) {
        /* with post-stabilisation: every iteration ignores all the old error, and one more
         * at the end, after the velocities are taken, removes it */
        const double alpha = s->vbd ? 0.0 : s->post_stabilize ? (iteration < s->iterations ? 1.0 : 0.0) : s->alpha;

        /* primal: each body in turn to the minimum of its own energy, one Newton step (eq. 4 to 6) */
        for (i = 0u; i < s->bodies; ++i) {
            AvbdBody    *b = &s->body[i];
            const double m[3] = {b->mass / (s->dt * s->dt), b->mass / (s->dt * s->dt), b->moment / (s->dt * s->dt)};
            double       lhs[3][3] = {{m[0], 0, 0}, {0, m[1], 0}, {0, 0, m[2]}}, rhs[3], move[3];
            if (b->mass == 0.0) continue;
            for (c = 0; c < 3; ++c) rhs[c] = m[c] * (b->x[c] - b->inertial[c]);
            for (n = 0u; n < s->forces; ++n) {
                AvbdForce *f = &s->force[n];
                double     j[AVBD_ROWS][3], h[AVBD_ROWS][3][3];
                if (f->a != (int)i && f->b != (int)i) continue;
                avbd_constraint(s, f, alpha);
                avbd_derivatives(s, f, (int)i, j, h);
                for (row = 0; row < f->rows; ++row) {
                    /* only a hard constraint carries a multiplier (eq. 9); the force is kept inside its bounds (3.2) */
                    const double lambda = !s->vbd && isinf(f->stiffness[row]) ? f->lambda[row] : 0.0;
                    const double force = clamp(f->penalty[row] * f->c[row] + lambda, f->fmin[row], f->fmax[row]);
                    int          p, q;
                    for (p = 0; p < 3; ++p) {
                        rhs[p] += j[row][p] * force;
                        for (q = 0; q < 3; ++q) lhs[p][q] += f->penalty[row] * j[row][p] * j[row][q];
                        /* in place of force x second derivative: the lengths of its columns, on the diagonal (3.5) */
                        lhs[p][p] += fabs(force) * sqrt(h[row][0][p] * h[row][0][p] + h[row][1][p] * h[row][1][p] + h[row][2][p] * h[row][2][p]);
                    }
                }
            }
            solve3(lhs, rhs, move);
            for (c = 0; c < 3; ++c) b->x[c] -= move[c];
        }

        /* dual: the multipliers take over the force, and the stiffness grows with the error (eq. 11, 12, 16) */
        for (n = 0u; n < s->forces && iteration < s->iterations; ++n) {
            AvbdForce *f = &s->force[n];
            avbd_constraint(s, f, alpha);
            for (row = 0; row < f->rows; ++row) {
                const double lambda = !s->vbd && isinf(f->stiffness[row]) ? f->lambda[row] : 0.0;
                f->lambda[row] = clamp(f->penalty[row] * f->c[row] + lambda, f->fmin[row], f->fmax[row]);
                if (!s->vbd && f->lambda[row] > f->fmin[row] && f->lambda[row] < f->fmax[row])
                    f->penalty[row] = fmin(f->penalty[row] + s->beta * fabs(f->c[row]), fmin(AVBD_PENALTY_MAX, f->stiffness[row]));
            }
        }

        if (iteration + 1u == s->iterations)
            for (i = 0u; i < s->bodies; ++i) {
                AvbdBody *b = &s->body[i];
                for (c = 0; c < 3; ++c) {
                    b->v_before[c] = b->v[c];
                    if (b->mass > 0.0) b->v[c] = (b->x[c] - b->initial[c]) / s->dt;
                }
            }
    }
}
