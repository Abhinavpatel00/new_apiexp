/* method.c — "Contact and Friction Simulation for Computer Graphics" (Sheldon Andrews, Kenny
 * Erleben, Zachary Ferguson; SIGGRAPH 2022 course notes).
 *
 * Rigid spheres and boxes that touch. A step finds where they touch (section 2.1), writes
 * for every contact how its relative velocity depends on the impulses there (sections 1.5,
 * 1.7, 1.10.1) and finds impulses that keep the bodies apart and obey Coulomb friction
 * (chapter 3).
 *
 * A contact has a normal n from body a to body b and two tangents; with u the velocities of
 * all bodies its velocity is v = J u (eq. 22, 43), one row of J for each direction. An
 * impulse lambda there changes u by M^-1 J' lambda, so
 *
 *     v = A lambda + b,   A = J M^-1 J',   b = J (u + h M^-1 f)        (eq. 45a, 227)
 *
 * The normal part must satisfy v_n >= 0, lambda_n >= 0, v_n lambda_n = 0. Friction must lie
 * in a set that grows with lambda_n and, when the contact slides, take out as much energy
 * as it can. Two sets are here:
 *
 *     box      |lambda_t| <= mu lambda_n for each tangent by itself (eq. 40): a pyramid that
 *              holds the true cone; the boxed LCP of section 1.7
 *     ellipse  (lambda_t / mu_t lambda_n)^2 + (lambda_b / mu_b lambda_n)^2 + (lambda_tau /
 *              mu_tau lambda_n)^2 <= 1 (eq. 220b): Coulomb's cone when mu_t = mu_b, with
 *              friction that depends on direction and friction against spinning
 *
 * Both are solved through the same fixed point (eq. 215):
 *
 *     lambda_n = max(0, lambda_n - r v_n),   lambda_f = closest point of the set to (lambda_f - r v_f)
 *
 * which holds at a solution for every r > 0; r only decides how fast iterating it converges.
 * Algorithm 11 (Jacobi) does every contact from the last iterate, algorithm 12
 * (Gauss-Seidel) uses each new value at once; with the box and r = 1 / A_ii the latter is
 * projected Gauss-Seidel (eq. 147, 158), with r = omega / A_ii projected SOR. With `adapt`
 * an iteration that changes the impulses more than the one before is undone and r is made
 * smaller (section 3.4.4). Algorithm 10 (NNCG) takes what a sweep changes as a gradient and
 * runs Fletcher-Reeves conjugate gradients on it.
 *
 * The step is Moreau's (section 3.4.5): half a step of positions, contacts and impulses
 * there, new velocities, the other half. A share `erp` of every overlap beyond `slop` is
 * then taken out by moving the bodies (section 3.4.6: displacements d >= 0 along the
 * normals with A d + gap >= 0, by `project` Gauss-Seidel sweeps), which gives them no
 * speed; with project = 0 the share is added to b instead (section 1.9, eq. 59).
 *
 * This file's own choices: the frame's first tangent is the x axis of body a laid into the
 * contact plane; a contact's coefficients are the smaller of the two bodies'; a contact is
 * made already when the gap is below `margin` and then may close the gap in the step; the
 * friction block has one r = omega / (sum of its A_ii), because different r inside the
 * block would change the fixed point; the normal's new value is used for the friction of
 * the same contact; A is never formed, M^-1 J' lambda is kept for each body (section 3.4.2,
 * eq. 230); the solver starts from the impulses of the last step's contacts (`warm`), which
 * the notes mention only in passing; the overlap left alone (`slop`); a sphere whose centre
 * is inside a box has the gap -(depth to the nearest face) - radius, where the notes leave
 * the radius out; edge against edge is chosen only if its axis is clearly the shallowest.
 */
#include "method.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static double dot(const double *a, const double *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

static void cross(const double *a, const double *b, double *out) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

static double clamp(double x, double low, double high) { return fmax(low, fmin(high, x)); }

Cf *cf_open(void) {
    Cf *c = calloc(1, sizeof *c);
    c->gravity    = 9.81;
    c->dt         = 1.0 / 60.0;
    c->margin     = 0.01;
    c->erp        = 0.2;
    c->slop       = 0.002;
    c->project    = 10;
    c->set        = CF_SET_ELLIPSE;
    c->method     = CF_GAUSS_SEIDEL;
    c->iterations = 40;
    c->warm       = true;
    c->room       = c->old_room = 64;
    c->contact    = malloc(64 * sizeof *c->contact);
    c->old        = malloc(64 * sizeof *c->old);
    c->omega      = 1.0;
    return c;
}

void cf_close(Cf *c) {
    free(c->body); free(c->contact); free(c->old); free(c->old_lambda); free(c->order); free(c->lambda); free(c->row); free(c->back); free(c->diag);
    free(c->bias); free(c->r); free(c->free); free(c->push); free(c->scratch);
    free(c);
}

void cf_place(CfBody *b) {
    const double n = sqrt(b->q[0] * b->q[0] + b->q[1] * b->q[1] + b->q[2] * b->q[2] + b->q[3] * b->q[3]);
    const double w = b->q[0] / n, x = b->q[1] / n, y = b->q[2] / n, z = b->q[3] / n;
    const double r[9] = {1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y),
                         2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x),
                         2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)};
    int i, j, k;
    b->q[0] = w; b->q[1] = x; b->q[2] = y; b->q[3] = z;
    memcpy(b->r, r, sizeof r);
    for (i = 0; i < 3; ++i) {
        for (j = 0; j < 3; ++j) {
            b->inertia_inv_world[3 * i + j] = 0.0;
            for (k = 0; k < 3; ++k) b->inertia_inv_world[3 * i + j] += r[3 * i + k] * b->inertia_inv[k] * r[3 * j + k];
        }
    }
}

int cf_add(Cf *c, int shape, const double half[3], double density, const double p[3]) {
    CfBody *b;
    double  mass, inertia[3];
    int     k;
    c->body = realloc(c->body, (size_t)(c->bodies + 1) * sizeof *c->body);
    b       = &c->body[c->bodies];
    memset(b, 0, sizeof *b);
    b->shape = shape;
    memcpy(b->half, half, sizeof b->half);
    memcpy(b->p, p, sizeof b->p);
    b->q[0]  = 1.0;
    b->mu[0] = b->mu[1] = 0.5;
    mass     = density * (shape == CF_BOX ? 8.0 * half[0] * half[1] * half[2] : 4.18879020478639 * half[0] * half[0] * half[0]);
    for (k = 0; k < 3; ++k) {
        const double a = half[(k + 1) % 3], e = half[(k + 2) % 3];
        inertia[k]        = shape == CF_BOX ? mass * (a * a + e * e) / 3.0 : 0.4 * mass * half[0] * half[0];
        b->inertia_inv[k] = mass > 0.0 ? 1.0 / inertia[k] : 0.0;
    }
    b->mass_inv = mass > 0.0 ? 1.0 / mass : 0.0;
    cf_place(b);
    return c->bodies++;
}

int cf_collide(const CfBody *a, const CfBody *b, double margin, CfContact out[8]) {
    int count = 0, i, k;

    if (a->shape == CF_SPHERE && b->shape == CF_SPHERE) {
        /* section 2.1.1; the point is in the middle of the overlap */
        double d[3], distance;
        for (k = 0; k < 3; ++k) d[k] = b->p[k] - a->p[k];
        distance   = sqrt(dot(d, d));
        out[0].gap = distance - a->half[0] - b->half[0];
        for (k = 0; k < 3; ++k) {
            out[0].frame[k] = distance > 1e-12 ? d[k] / distance : (double)(k == 1);
            out[0].point[k] = a->p[k] + out[0].frame[k] * (a->half[0] + 0.5 * out[0].gap);
        }
        count = out[0].gap <= margin;
    }
    if (a->shape != b->shape) {
        /* section 2.1.2: the sphere's centre in the box's frame, the box's closest point g to it */
        const CfBody *ball = a->shape == CF_SPHERE ? a : b, *box = a->shape == CF_SPHERE ? b : a;
        const double  turn = ball == a ? -1.0 : 1.0;
        double        local[3], g[3], n[3] = {0, 0, 0}, d[3], distance, least = 1e300;
        int           face = 0;
        for (k = 0; k < 3; ++k) d[k] = ball->p[k] - box->p[k];
        for (k = 0; k < 3; ++k) {
            local[k] = box->r[k] * d[0] + box->r[3 + k] * d[1] + box->r[6 + k] * d[2];
            g[k]     = clamp(local[k], -box->half[k], box->half[k]);
        }
        for (k = 0; k < 3; ++k) {
            n[k] = local[k] - g[k];
            if (box->half[k] - fabs(local[k]) < least) { least = box->half[k] - fabs(local[k]); face = k; }
        }
        distance   = sqrt(dot(n, n));
        out[0].gap = distance - ball->half[0];
        for (k = 0; k < 3; ++k) n[k] = distance > 1e-12 ? n[k] / distance : 0.0;
        if (distance <= 1e-12) {
            /* the centre is inside the box: out through the nearest face. The notes give the gap as -least;
             * here the radius is taken off as well, so that the gap is continuous with the shallow case. */
            n[face]    = local[face] < 0.0 ? -1.0 : 1.0;
            g[face]    = n[face] * box->half[face];
            out[0].gap = -least - ball->half[0];
        }
        for (k = 0; k < 3; ++k) {
            out[0].point[k] = box->p[k] + box->r[3 * k] * g[0] + box->r[3 * k + 1] * g[1] + box->r[3 * k + 2] * g[2];
            out[0].frame[k] = turn * (box->r[3 * k] * n[0] + box->r[3 * k + 1] * n[1] + box->r[3 * k + 2] * n[2]);
        }
        count = out[0].gap <= margin;
    }
    if (a->shape == CF_BOX && b->shape == CF_BOX) {
        /* section 2.1.3: the 15 axes, then the face that is least deep with the other box's face cut to it, or the
         * two edges that cross */
        const CfBody *box[2] = {a, b};
        double        d[3], ax[6][3], l[3], best[2] = {-1e300, -1e300};
        int           which[2] = {0, 6}, j;
        bool          apart = false;

        for (k = 0; k < 3; ++k) d[k] = b->p[k] - a->p[k];
        for (i = 0; i < 6; ++i) {
            for (k = 0; k < 3; ++k) ax[i][k] = box[i / 3]->r[3 * k + i % 3];
        }
        for (i = 0; i < 15; ++i) {
            double length, s;
            if (i < 6) memcpy(l, ax[i], sizeof l);
            if (i >= 6) cross(ax[(i - 6) / 3], ax[3 + (i - 6) % 3], l);
            length = sqrt(dot(l, l));
            if (length < 1e-6) continue;   /* two parallel edges: their axis is a face's */
            s = fabs(dot(d, l)) / length;
            for (j = 0; j < 6; ++j) s -= box[j / 3]->half[j % 3] * fabs(dot(ax[j], l)) / length;
            apart = apart || s > margin;
            if (s > best[i >= 6]) { best[i >= 6] = s; which[i >= 6] = i; }
        }

        if (!apart && best[1] > best[0] + 0.05 * fabs(best[0]) + 1e-4) {
            /* edge against edge: the edge of each box that lies furthest towards the other, their closest points */
            const int    ea = (which[1] - 6) / 3, eb = (which[1] - 6) % 3;
            const double *u = ax[ea], *v = ax[3 + eb];
            double       pa[3], pb[3], w0[3], length, turn, uv, uw, vw, sa, sb;
            cross(u, v, l);
            length = sqrt(dot(l, l));
            turn   = dot(d, l) < 0.0 ? -1.0 : 1.0;
            for (k = 0; k < 3; ++k) { l[k] *= turn / length; pa[k] = a->p[k]; pb[k] = b->p[k]; }
            for (j = 0; j < 3; ++j) {
                const double sign_a = dot(ax[j], l) < 0.0 ? -1.0 : 1.0, sign_b = dot(ax[3 + j], l) < 0.0 ? -1.0 : 1.0;
                for (k = 0; k < 3; ++k) {
                    pa[k] += j == ea ? 0.0 : sign_a * a->half[j] * ax[j][k];
                    pb[k] -= j == eb ? 0.0 : sign_b * b->half[j] * ax[3 + j][k];
                }
            }
            for (k = 0; k < 3; ++k) w0[k] = pa[k] - pb[k];
            uv = dot(u, v); uw = dot(u, w0); vw = dot(v, w0);
            sa = clamp((uv * vw - uw) / (1.0 - uv * uv), -a->half[ea], a->half[ea]);
            sb = clamp((vw - uv * uw) / (1.0 - uv * uv), -b->half[eb], b->half[eb]);
            out[0].gap = 0.0;
            for (k = 0; k < 3; ++k) {
                const double qa = pa[k] + sa * u[k], qb = pb[k] + sb * v[k];
                out[0].point[k] = 0.5 * (qa + qb);
                out[0].frame[k] = l[k];
                out[0].gap     += (qb - qa) * l[k];
            }
            count = 1;
        }
        if (!apart && count == 0) {
            /* face: the reference face is which[0]; the other box's face that looks at it most is cut to its four sides */
            const int     ref = which[0] / 3, axis = which[0] % 3;
            const CfBody *R = box[ref], *I = box[1 - ref];
            const double  side = (dot(d, ax[which[0]]) < 0.0 ? -1.0 : 1.0) * (ref ? -1.0 : 1.0);
            double        n[3], poly[2][12][3], most = -1.0, turn = 1.0;
            int           face = 0, vertices = 4, from = 0, plane;

            for (k = 0; k < 3; ++k) n[k] = side * ax[which[0]][k];
            for (j = 0; j < 3; ++j) {
                const double along = dot(n, ax[3 * (1 - ref) + j]);
                if (fabs(along) > most) { most = fabs(along); face = j; turn = along > 0.0 ? -1.0 : 1.0; }
            }
            for (i = 0; i < 4; ++i) {
                const double *e0 = ax[3 * (1 - ref) + face], *e1 = ax[3 * (1 - ref) + (face + 1) % 3], *e2 = ax[3 * (1 - ref) + (face + 2) % 3];
                const double  s1 = (i == 0 || i == 3 ? 1.0 : -1.0) * I->half[(face + 1) % 3], s2 = (i < 2 ? 1.0 : -1.0) * I->half[(face + 2) % 3];
                double        world[3];
                for (k = 0; k < 3; ++k) world[k] = I->p[k] + turn * I->half[face] * e0[k] + s1 * e1[k] + s2 * e2[k] - R->p[k];
                for (k = 0; k < 3; ++k) poly[0][i][k] = R->r[k] * world[0] + R->r[3 + k] * world[1] + R->r[6 + k] * world[2];
            }
            for (plane = 0; plane < 4; ++plane) {
                /* Sutherland-Hodgman against sign * x[m] <= half[m] */
                const int    m = (axis + 1 + plane / 2) % 3;
                const double sign = plane % 2 ? -1.0 : 1.0, limit = R->half[m];
                int          made = 0;
                for (i = 0; i < vertices; ++i) {
                    const double *p = poly[from][i], *q = poly[from][(i + 1) % vertices];
                    const double  dp = sign * p[m] - limit, dq = sign * q[m] - limit;
                    if (dp <= 0.0) memcpy(poly[1 - from][made++], p, 3 * sizeof(double));
                    if ((dp <= 0.0) != (dq <= 0.0)) {
                        for (k = 0; k < 3; ++k) poly[1 - from][made][k] = p[k] + (q[k] - p[k]) * dp / (dp - dq);
                        made++;
                    }
                }
                vertices = made;
                from     = 1 - from;
            }
            for (i = 0; i < vertices && count < 8; ++i) {
                double      *x = poly[from][i];
                const double depth = side * x[axis] - R->half[axis];
                if (depth > margin) continue;
                x[axis] = side * R->half[axis];   /* onto the reference face */
                for (k = 0; k < 3; ++k) {
                    out[count].point[k] = R->p[k] + R->r[3 * k] * x[0] + R->r[3 * k + 1] * x[1] + R->r[3 * k + 2] * x[2];
                    out[count].frame[k] = ref ? -n[k] : n[k];
                }
                out[count++].gap = depth;
            }
        }
    }

    for (i = 0; i < count; ++i) {
        /* the frame: a's x axis laid into the contact plane (its z axis if x is along the normal); the smaller coefficients */
        double *n = out[i].frame, *t = out[i].frame + 3, x[3], along, length;
        for (k = 0; k < 3; ++k) x[k] = a->r[3 * k];
        along = dot(x, n);
        for (k = 0; k < 3; ++k) x[k] = fabs(along) > 0.95 ? a->r[3 * k + 2] : x[k];
        along = dot(x, n);
        for (k = 0; k < 3; ++k) t[k] = x[k] - along * n[k];
        length = sqrt(dot(t, t));
        for (k = 0; k < 3; ++k) t[k] /= length;
        cross(n, t, out[i].frame + 6);
        for (k = 0; k < 3; ++k) out[i].mu[k] = fmin(a->mu[k], b->mu[k]);
    }
    return count;
}

/* g of algorithm 13: sum of a^2 y^2 / (a^2 + t)^2 - 1. */
static double ellipsoid_g(const double a[3], const double y[3], double t) {
    double g = -1.0;
    int    k;
    for (k = 0; k < 3; ++k) g += a[k] > 0.0 ? a[k] * a[k] * y[k] * y[k] / ((a[k] * a[k] + t) * (a[k] * a[k] + t)) : 0.0;
    return g;
}

void cf_ellipsoid(const double s[3], double z[3]) {
    /* algorithm 13: scaled into the unit cube; x = a^2 y / (a^2 + t) with t >= 0 the root of g, by bisection */
    const double scale = 1.0 / fmax(fmax(1.0, fmax(s[0], fmax(s[1], s[2]))), fmax(fabs(z[0]), fmax(fabs(z[1]), fabs(z[2]))));
    const double a[3] = {scale * s[0], scale * s[1], scale * s[2]}, y[3] = {scale * z[0], scale * z[1], scale * z[2]};
    const bool   outside = ellipsoid_g(a, y, 0.0) > 0.0;
    double       low = 0.0, high = fmax(a[0], fmax(a[1], a[2])) * sqrt(dot(y, y));
    int          k, step;
    while (outside && ellipsoid_g(a, y, high) > 0.0) { low = high; high *= 1.5; }
    for (step = 0; step < 80 && outside; ++step) {
        const double t = 0.5 * (low + high), g = ellipsoid_g(a, y, t);
        low  = g > 0.0 ? t : low;
        high = g > 0.0 ? high : t;
    }
    for (k = 0; k < 3; ++k) z[k] = a[k] > 0.0 ? a[k] * a[k] * y[k] / (a[k] * a[k] + (outside ? 0.5 * (low + high) : 0.0)) / scale : 0.0;
}

void cf_detect(Cf *c) {
    CfContact *swap = c->old;
    const int  room = c->old_room;
    int        i, j, k, count;
    /* the last contacts and their impulses are kept for cf_problem */
    c->old_lambda = realloc(c->old_lambda, (4u * (size_t)c->contacts + 4u) * sizeof(double));
    memcpy(c->old_lambda, c->lambda, 4u * (size_t)c->contacts * sizeof(double));
    memset(&c->old_lambda[4 * c->contacts], 0, 4 * sizeof(double));   /* what a contact with no match gets */
    c->old      = c->contact; c->old_room = c->room; c->olds = c->contacts;
    c->contact  = swap;       c->room     = room;
    c->contacts = 0;
    for (i = 0; i < c->bodies; ++i) {
        for (j = i + 1; j < c->bodies; ++j) {
            const CfBody *a = &c->body[i], *b = &c->body[j];
            const double  reach = sqrt(dot(a->half, a->half)) + sqrt(dot(b->half, b->half)) + c->margin;
            const double  d[3] = {b->p[0] - a->p[0], b->p[1] - a->p[1], b->p[2] - a->p[2]};
            if (a->mass_inv + b->mass_inv == 0.0 || dot(d, d) > reach * reach) continue;
            if (c->contacts + 8 > c->room) {
                c->room    = 2 * c->room + 64;
                c->contact = realloc(c->contact, (size_t)c->room * sizeof *c->contact);
            }
            count = cf_collide(a, b, c->margin, &c->contact[c->contacts]);
            for (k = 0; k < count; ++k) { c->contact[c->contacts + k].a = i; c->contact[c->contacts + k].b = j; }
            c->contacts += count;
        }
    }
}

void cf_problem(Cf *c) {
    const size_t rows = 4u * (size_t)c->contacts + 4u, wide = 6u * (size_t)c->bodies;
    int          i, j, k;

    c->row     = realloc(c->row, 12u * rows * sizeof(double));
    c->back    = realloc(c->back, 12u * rows * sizeof(double));
    c->diag    = realloc(c->diag, rows * sizeof(double));
    c->bias    = realloc(c->bias, rows * sizeof(double));
    c->r       = realloc(c->r, rows * sizeof(double));
    c->lambda  = realloc(c->lambda, rows * sizeof(double));
    c->order   = realloc(c->order, rows * sizeof(int));
    c->free    = realloc(c->free, wide * sizeof(double));
    c->push    = realloc(c->push, wide * sizeof(double));
    c->scratch = realloc(c->scratch, (4u * rows + 3u * wide) * sizeof(double));
    memset(c->lambda, 0, rows * sizeof(double));

    /* u + h M^-1 f: gravity, and the gyroscopic term worked out in the body's frame */
    for (i = 0; i < c->bodies; ++i) {
        const CfBody *b = &c->body[i];
        double       *u = &c->free[6 * i], local[3], spin[3], turn[3];
        for (k = 0; k < 3; ++k) {
            local[k] = b->r[k] * b->w[0] + b->r[3 + k] * b->w[1] + b->r[6 + k] * b->w[2];
            spin[k]  = b->inertia_inv[k] > 0.0 ? local[k] / b->inertia_inv[k] : 0.0;
        }
        cross(spin, local, turn);
        for (k = 0; k < 3; ++k) turn[k] *= c->dt * b->inertia_inv[k];
        for (k = 0; k < 3; ++k) {
            u[k]     = b->v[k] - (k == 1 && b->mass_inv > 0.0 ? c->dt * c->gravity : 0.0);
            u[3 + k] = b->w[k] + b->r[3 * k] * turn[0] + b->r[3 * k + 1] * turn[1] + b->r[3 * k + 2] * turn[2];
        }
    }

    for (i = 0; i < c->contacts; ++i) {
        const CfContact *q = &c->contact[i];
        const CfBody    *body[2] = {&c->body[q->a], &c->body[q->b]};
        const int        index[2] = {q->a, q->b};
        double           near = 4e-4;
        int              match = c->olds;
        for (j = 0; j < 4; ++j) {
            /* rows 0..2: the velocity of b's point less a's along n, t, n x t (eq. 43); row 3: the spin about n
             * times mu_tau, a length, so that this row is a speed and its impulse an impulse like the others */
            const double *d = &q->frame[j < 3 ? 3 * j : 0];
            double       *row = &c->row[12 * (4 * i + j)], *back = &c->back[12 * (4 * i + j)];
            int           s;
            c->diag[4 * i + j] = 0.0;
            c->bias[4 * i + j] = 0.0;
            for (s = 0; s < 2; ++s) {
                const double sign = s ? 1.0 : -1.0, *w = body[s]->inertia_inv_world;
                const double arm[3] = {q->point[0] - body[s]->p[0], q->point[1] - body[s]->p[1], q->point[2] - body[s]->p[2]};
                double       turn[3];
                cross(arm, d, turn);
                for (k = 0; k < 3; ++k) {
                    row[6 * s + k]     = j < 3 ? sign * d[k] : 0.0;
                    row[6 * s + 3 + k] = j < 3 ? sign * turn[k] : sign * q->mu[2] * d[k];
                }
                for (k = 0; k < 3; ++k) {
                    back[6 * s + k]     = body[s]->mass_inv * row[6 * s + k];
                    back[6 * s + 3 + k] = w[3 * k] * row[6 * s + 3] + w[3 * k + 1] * row[6 * s + 4] + w[3 * k + 2] * row[6 * s + 5];
                }
                for (k = 0; k < 6; ++k) {
                    c->diag[4 * i + j] += row[6 * s + k] * back[6 * s + k];
                    c->bias[4 * i + j] += row[6 * s + k] * c->free[6 * index[s] + k];
                }
            }
        }
        /* an open gap may close in this step; a share of an overlap is taken out (eq. 59) */
        c->bias[4 * i] += (q->gap > 0.0 ? q->gap : c->project ? 0.0 : c->erp * fmin(0.0, q->gap + c->slop)) / c->dt;
        c->diag[4 * i + 3] += q->mu[2] > 0.0 ? 0.0 : 1.0;   /* a row of zeros: its impulse stays 0 */
        c->order[i]     = i;
        /* a first guess: the impulses of the last step's contact between the same two bodies nearest to this one,
         * within 2 cm; each old contact is used once */
        for (j = 0; j < (c->warm ? c->olds : 0); ++j) {
            const CfContact *o = &c->old[j];
            const double     d[3] = {o->point[0] - q->point[0], o->point[1] - q->point[1], o->point[2] - q->point[2]};
            if (o->a == q->a && o->b == q->b && dot(d, d) < near) { near = dot(d, d); match = j; }
        }
        memcpy(&c->lambda[4 * i], &c->old_lambda[4 * match], 4 * sizeof(double));
        c->old[match].a = -1;
    }
}

/* push = M^-1 J' lambda, 6 a body. */
static void impulses(const Cf *c, const double *lambda, double *push) {
    int i, k, s;
    memset(push, 0, 6u * (size_t)c->bodies * sizeof(double));
    for (i = 0; i < 4 * c->contacts; ++i) {
        const int index[2] = {c->contact[i / 4].a, c->contact[i / 4].b};
        for (s = 0; s < 2; ++s) {
            for (k = 0; k < 6; ++k) push[6 * index[s] + k] += c->back[12 * i + 6 * s + k] * lambda[i];
        }
    }
}

/* The r of every row: omega / A_ii, and for the rows of an ellipse omega / (the sum of their A_ii). */
static void factors(const Cf *c, double omega, double *r) {
    int i, j;
    for (i = 0; i < c->contacts; ++i) {
        const double *diag = &c->diag[4 * i];
        const double  block = diag[1] + diag[2] + diag[3];
        r[4 * i] = omega / diag[0];
        for (j = 1; j < 4; ++j) r[4 * i + j] = omega / (c->set == CF_SET_BOX ? diag[j] : block);
    }
}

/* One iteration over the contacts in c->order. `push` is M^-1 J' lambda and is kept so. If
 * `seen` is push itself every contact sees the ones before it (eq. 229, Gauss-Seidel); if
 * it is a copy made before the pass, none does and the friction set is that of the old
 * normal impulse (eq. 228, Jacobi). */
static void pass(const Cf *c, const double *r, double *lambda, const double *seen, double *push) {
    const int size = c->set == CF_SET_BOX ? 1 : 3;
    int       o, j, k, s, first, count;
    for (o = 0; o < c->contacts; ++o) {
        const int        i = c->order[o];
        const CfContact *q = &c->contact[i];
        const int        index[2] = {q->a, q->b};
        const double     old = lambda[4 * i];
        double           z[4];
        for (first = 0; first < 4; first += count) {
            const double limit = seen == push ? lambda[4 * i] : old;
            const double half[3] = {q->mu[0] * limit, q->mu[1] * limit, q->mu[2] > 0.0 ? limit : 0.0};
            count = first ? size : 1;
            for (j = first; j < first + count; ++j) {
                const double *row = &c->row[12 * (4 * i + j)];
                double        speed = c->bias[4 * i + j];
                for (s = 0; s < 2; ++s) {
                    for (k = 0; k < 6; ++k) speed += row[6 * s + k] * seen[6 * index[s] + k];
                }
                z[j] = lambda[4 * i + j] - r[4 * i + j] * speed;
            }
            if (first == 0) z[0] = fmax(0.0, z[0]);
            if (first && size == 1) z[first] = clamp(z[first], -half[first - 1], half[first - 1]);
            if (first && size == 3) cf_ellipsoid(half, &z[1]);
            for (j = first; j < first + count; ++j) {
                const double *back = &c->back[12 * (4 * i + j)], change = z[j] - lambda[4 * i + j];
                lambda[4 * i + j] = z[j];
                for (s = 0; s < 2; ++s) {
                    for (k = 0; k < 6; ++k) push[6 * index[s] + k] += back[6 * s + k] * change;
                }
            }
        }
    }
}

double cf_merit(const Cf *c) {
    const size_t rows = 4u * (size_t)c->contacts, wide = 6u * (size_t)c->bodies;
    double      *lambda = c->scratch + 2u * (rows + 4u), *r = lambda + rows + 4u, *seen = r + rows + 4u + wide, *push = seen + wide;
    double       worst = 0.0;
    size_t       i;
    memcpy(lambda, c->lambda, rows * sizeof(double));
    impulses(c, lambda, push);
    memcpy(seen, push, wide * sizeof(double));
    factors(c, 1.0, r);
    pass(c, r, lambda, seen, push);
    for (i = 0; i < rows; ++i) worst = fmax(worst, fabs(lambda[i] - c->lambda[i]));
    return worst;
}

void cf_solve(Cf *c, double *history) {
    const size_t rows = 4u * (size_t)c->contacts, wide = 6u * (size_t)c->bodies;
    const double shrink = c->method == CF_JACOBI ? 0.5 : 0.9;   /* the notes' values for one r and for one r a row */
    double      *lambda = c->lambda, *push = c->push;
    double      *last = c->scratch, *p = last + rows + 4u, *seen = p + 3u * (rows + 4u);
    double       before = 1e300, gg_last = 1.0, top = 1.0;
    size_t       i;
    int          k, s, step;

    factors(c, c->omega, c->r);
    for (step = 0; step < 30 && c->method == CF_JACOBI; ++step) {
        /* one r for all: omega / (the largest eigenvalue of A), by the power method from a vector of ones */
        for (i = 0; i < rows; ++i) last[i] = step ? p[i] / top : 1.0 / sqrt((double)rows);
        impulses(c, last, push);
        top = 0.0;
        for (i = 0; i < rows; ++i) {
            const int index[2] = {c->contact[i / 4].a, c->contact[i / 4].b};
            p[i] = 0.0;
            for (s = 0; s < 2; ++s) {
                for (k = 0; k < 6; ++k) p[i] += c->row[12 * i + 6 * (size_t)s + (size_t)k] * push[6 * index[s] + k];
            }
            top += p[i] * p[i];
        }
        top = sqrt(top);
        for (i = 0; i < rows; ++i) c->r[i] = c->omega / top;
    }
    impulses(c, lambda, push);
    memset(p, 0, rows * sizeof(double));
    c->used = c->rollbacks = 0;
    c->change = 0.0;

    for (k = 0; k < c->iterations; ++k) {
        double change = 0.0, largest = 0.0, gg = 0.0;
        bool   done;
        memcpy(last, lambda, rows * sizeof(double));
        memcpy(seen, push, wide * sizeof(double));
        pass(c, c->r, lambda, c->method == CF_JACOBI ? seen : push, push);
        for (i = 0; i < rows; ++i) {
            change  = fmax(change, fabs(lambda[i] - last[i]));
            largest = fmax(largest, fabs(lambda[i]));
            gg     += (lambda[i] - last[i]) * (lambda[i] - last[i]);
        }
        done = change <= c->tolerance * largest || k == c->iterations - 1;
        if (c->method == CF_NNCG && !done) {
            /* algorithm 10: the sweep's change is minus the gradient; beta > 1 starts the directions again */
            const double beta = k ? gg / gg_last : 0.0;
            for (i = 0; i < rows; ++i) {
                const double down = lambda[i] - last[i];
                lambda[i] += beta > 1.0 ? 0.0 : beta * p[i];
                p[i]       = beta > 1.0 ? 0.0 : beta * p[i] + down;
            }
            impulses(c, lambda, push);
            gg_last = gg;
        }
        if (c->adapt && c->method != CF_NNCG && change > before && !done) {
            memcpy(lambda, last, rows * sizeof(double));
            memcpy(push, seen, wide * sizeof(double));
            for (i = 0; i < rows; ++i) c->r[i] *= shrink;
            c->rollbacks++;
        } else {
            before = change;
        }
        c->used   = k + 1;
        c->change = change;
        for (step = k; history && step < (done ? c->iterations : k + 1); ++step) history[step] = cf_merit(c);
        if (done) break;
    }
}

void cf_apply(Cf *c) {
    int i, k;
    for (i = 0; i < c->bodies; ++i) {
        for (k = 0; k < 3; ++k) {
            c->body[i].v[k] = c->free[6 * i + k] + c->push[6 * i + k];
            c->body[i].w[k] = c->free[6 * i + 3 + k] + c->push[6 * i + 3 + k];
        }
    }
}

void cf_step(Cf *c) {
    int i, k, half;
    for (half = 0; half < 2; ++half) {
        /* Moreau: half a step of positions, the contacts and impulses there, the other half with the new velocities */
        for (i = 0; i < c->bodies; ++i) {
            CfBody      *b = &c->body[i];
            const double q[4] = {b->q[0], b->q[1], b->q[2], b->q[3]}, *w = b->w, h = 0.25 * c->dt;
            for (k = 0; k < 3; ++k) b->p[k] += 0.5 * c->dt * b->v[k];
            b->q[0] += h * (-w[0] * q[1] - w[1] * q[2] - w[2] * q[3]);
            b->q[1] += h * (w[0] * q[0] + w[1] * q[3] - w[2] * q[2]);
            b->q[2] += h * (w[1] * q[0] + w[2] * q[1] - w[0] * q[3]);
            b->q[3] += h * (w[2] * q[0] + w[0] * q[2] - w[1] * q[1]);
            cf_place(b);
        }
        if (half == 0) {
            double *d, *move;
            int     sweep, s;
            cf_detect(c);
            cf_problem(c);
            cf_solve(c, NULL);
            cf_apply(c);
            d    = c->scratch;
            move = c->scratch + 4 * (4 * c->contacts + 4);
            /* section 3.4.6: displacements d >= 0 along the normals with A d + gap >= 0 (eq. 254), by sweeps of
             * eq. 255 with r = 1 / A_ii, for a share of each overlap; then the bodies are moved (eq. 256) */
            memset(d, 0, 4u * (size_t)c->contacts * sizeof(double));
            memset(move, 0, 6u * (size_t)c->bodies * sizeof(double));
            for (sweep = 0; sweep < c->project; ++sweep) {
                for (i = 0; i < c->contacts; ++i) {
                    const CfContact *q = &c->contact[i];
                    const double    *row = &c->row[48 * i], *back = &c->back[48 * i];
                    const int        index[2] = {q->a, q->b};
                    double           gap = q->gap > 0.0 ? q->gap : c->erp * fmin(0.0, q->gap + c->slop), change;
                    for (s = 0; s < 2; ++s) {
                        for (k = 0; k < 6; ++k) gap += row[6 * s + k] * move[6 * index[s] + k];
                    }
                    change = fmax(0.0, d[i] - gap / c->diag[4 * i]) - d[i];
                    d[i]  += change;
                    for (s = 0; s < 2; ++s) {
                        for (k = 0; k < 6; ++k) move[6 * index[s] + k] += back[6 * s + k] * change;
                    }
                }
            }
            for (i = 0; i < c->bodies; ++i) {
                CfBody      *b = &c->body[i];
                const double q[4] = {b->q[0], b->q[1], b->q[2], b->q[3]}, *w = &move[6 * i + 3];
                for (k = 0; k < 3; ++k) b->p[k] += move[6 * i + k];
                b->q[0] += 0.5 * (-w[0] * q[1] - w[1] * q[2] - w[2] * q[3]);
                b->q[1] += 0.5 * (w[0] * q[0] + w[1] * q[3] - w[2] * q[2]);
                b->q[2] += 0.5 * (w[1] * q[0] + w[2] * q[1] - w[0] * q[3]);
                b->q[3] += 0.5 * (w[2] * q[0] + w[0] * q[2] - w[1] * q[1]);
            }
        }
    }
}
