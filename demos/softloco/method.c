/* method.c — "Soft Body Locomotion" (Jie Tan, Greg Turk, C. Karen Liu; ACM Trans. Graph.
 * 31(4), SIGGRAPH 2012).
 *
 * A body of tetrahedra with no skeleton. It moves by muscles: segments that want to be
 * shorter than they are. Each step the lengths the muscles want are chosen so that the body
 * comes as near as it can to goals such as "the centre of mass is here" (section 5), and the
 * choice is made together with what every point on the ground does: stay, slide or let go
 * (section 6).
 *
 * The body (section 4.1): corotational linear finite elements with the stiffness written in
 * the turned element (eq. 2), damping C = damping * K, one implicit step (eq. 8, 9):
 *
 *     (M + dt C + dt^2 K) v' = M v + dt (f - K (p - R x) + A (wanted - length)) + contact
 *     p' = p + dt v'
 *
 * The muscles (section 4.2): segment j pulls with stiffness * (wanted - length). That number
 * is put into the elements near it as a stress along the segment's direction (eq. 3, 4),
 * weighted by a Gaussian of the distance and normalised within the muscle's group (eq. 5);
 * the stress times each face's area and normal, shared among the face's corners, is the
 * force. A (eq. 6) holds that force for every muscle.
 *
 * The choice (section 5.1, eq. 11): the velocity after the step is linear in the wanted
 * lengths and the contact impulses, so a goal that is quadratic in it gives a quadratic
 * programme. The contact conditions are complementarities: an impulse or a separating
 * velocity, sticking or the edge of the friction cone. With the cone taken as eight
 * directions (section 7) a contact is in one of ten cases: free, static, or sliding with
 * friction on one of the eight edges. For fixed cases the programme is convex.
 *
 * The search (section 6): solve with a first guess of the cases. Every inequality that the
 * answer touches names a contact that might do better in another case (an impulse of 0:
 * let go; friction on the cone: slide; a sliding speed of 0: stay). Each such change gives
 * a new programme. They are tried best first until none is new or `visits` are solved.
 *
 * This file's own choices: the impulses are held as (normal, x, z) for each contact and the
 * eight-direction cone as its eight edges, which is the same set. In every case a contact
 * has three equalities, so the impulses follow from the wanted lengths and each programme
 * is left with the lengths as unknowns and inequalities only; it is solved by the dual
 * active-set method of Goldfarb and Idnani. Muscle ends are nodes; a muscle's stiffness is
 * `stiffness` / its rest length; a small cost keeps muscles near their rest length; each
 * edge of the cone has its own coefficient; contacts are nodes near the ground, not
 * patches. With no goals, or no programme that can be met, the contact impulses come from
 * projected Gauss-Seidel sweeps.
 */
#include "method.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define NODES_MOST 64   /* programmes a search may hold */

typedef struct Node {
    int    state[SL_CONTACTS];
    double objective;
    bool   feasible, expanded;
    int    candidates, candidate_contact[4 * SL_CONTACTS], candidate_state[4 * SL_CONTACTS];
    double *u;   /* wanted lengths, then (normal, x, z) impulses */
} Node;

typedef struct SlPlan {
    int    nu, rows, goals;
    /* this step, in scaled unknowns: lengths / rest length, impulses / (weight * dt), velocities / (gravity * dt) */
    double *s, vc0[3 * SL_CONTACTS], bias[SL_CONTACTS];   /* unscaled: contact velocity = vc0 + S u; the normal one must be >= bias */
    double *ss, *t, *miss, ease, impulse_unit, speed_unit; /* ss: S scaled; goal k misses by t[k] . u - miss[k] */
    /* one programme */
    double *e, *quantity, *offset;   /* e: the equalities; quantity (6 c rows): impulses, then contact velocities, = quantity . lengths + offset */
    double *p, *q, *c, *b, *x, *work;
    int    *row_contact, *row_state, *set;
    Node   node[NODES_MOST];
} SlPlan;

/* a = L L' in place (lower triangle, row after row) */
static void cholesky(double *a, int n) {
    int i, j, k;
    for (j = 0; j < n; ++j) {
        double d = a[j * n + j];
        for (k = 0; k < j; ++k) d -= a[j * n + k] * a[j * n + k];
        d = sqrt(d); a[j * n + j] = d;
        for (i = j + 1; i < n; ++i) {
            double t = a[i * n + j];
            for (k = 0; k < j; ++k) t -= a[i * n + k] * a[j * n + k];
            a[i * n + j] = t / d;
        }
    }
}

static void cholesky_solve(const double *a, int n, double *b) {
    int i, k;
    for (i = 0; i < n; ++i) {
        double t = b[i];
        for (k = 0; k < i; ++k) t -= a[i * n + k] * b[k];
        b[i] = t / a[i * n + i];
    }
    for (i = n - 1; i >= 0; --i) {
        double t = b[i];
        for (k = i + 1; k < n; ++k) t -= a[k * n + i] * b[k];
        b[i] = t / a[i * n + i];
    }
}

/* min x G x / 2 + a x with C x >= b, G positive definite (lower triangle given; it is
 * factored in place): the dual active-set method of Goldfarb and Idnani (1983), with the
 * active set's small system solved afresh every time. set[0 .. *active) are the rows that
 * hold with equality at the answer. False: the rows cannot all be met. */
static bool qp_solve(int n, double *g, const double *a, int rows, const double *c, const double *b, double *x, int *set, int *active, double *work, int *sweeps) {
    double *y = work, *small = y + rows * n, *r = small + n * n, *u = r + n, *z = u + n;
    int     count = 0, i, j, k, p, it;

    cholesky(g, n);
    for (i = 0; i < n; ++i) x[i] = -a[i];
    cholesky_solve(g, n, x);
    for (k = 0; k < rows; ++k) { memcpy(y + k * n, c + k * n, (size_t)n * sizeof(double)); cholesky_solve(g, n, y + k * n); }
    for (it = 0; it < 20 * rows; ++it) {
        /* the row that is missed by most */
        double worst = -1e-10, slack, lift = 0.0;
        for (p = -1, k = 0; k < rows; ++k) {
            double t = -b[k];
            for (i = 0; i < n; ++i) t += c[k * n + i] * x[i];
            if (t < worst) { worst = t; p = k; }
        }
        if (p < 0) break;
        slack = worst;
        for (;; ++*sweeps) {
            /* directions: z for x, r for the multipliers of the active rows */
            double zc = 0.0, size = 0.0, partial = 1e300, full, step;
            int    drop = -1;
            for (i = 0; i < count; ++i) {
                r[i] = 0.0;
                for (k = 0; k < n; ++k) r[i] += c[set[i] * n + k] * y[p * n + k];
                for (j = 0; j <= i; ++j) {
                    double t = 0.0;
                    for (k = 0; k < n; ++k) t += c[set[i] * n + k] * y[set[j] * n + k];
                    small[i * count + j] = t;
                }
            }
            cholesky(small, count);
            cholesky_solve(small, count, r);
            for (k = 0; k < n; ++k) {
                z[k] = y[p * n + k];
                for (i = 0; i < count; ++i) z[k] -= r[i] * y[set[i] * n + k];
                zc += z[k] * c[p * n + k]; size += y[p * n + k] * c[p * n + k];
            }
            for (i = 0; i < count; ++i)
                if (r[i] > 1e-12 && u[i] / r[i] < partial) { partial = u[i] / r[i]; drop = i; }
            full = zc > 1e-11 * size ? -slack / zc : 1e300;
            step = fmin(partial, full);
            if (step > 1e299 || !(zc == zc)) { *active = 0; return false; }
            for (i = 0; i < count; ++i) u[i] -= step * r[i];
            lift += step;
            if (full < 1e299) {
                for (k = 0; k < n; ++k) x[k] += step * z[k];
                slack += step * zc;
            }
            if (step == full) { set[count] = p; u[count] = lift; count += 1; break; }
            for (i = drop; i + 1 < count; ++i) { set[i] = set[i + 1]; u[i] = u[i + 1]; }
            count -= 1;
        }
    }
    *active = count;
    return it < 20 * rows;
}

/* One more inequality of the programme: sum of weight[k] * quantity[index[k]] >= bound, over
 * the lengths, its largest entry 1. `contact` and `state`: what to try if the answer touches it. */
static void row_add(SlPlan *q, int m, int terms, const int *index, const double *weight, double bound, int contact, int state) {
    double *row = q->c + q->rows * m, most = 0.0;
    int     k, j;
    for (j = 0; j < m; ++j) {
        row[j] = 0.0;
        for (k = 0; k < terms; ++k) row[j] += weight[k] * q->quantity[index[k] * m + j];
        most = fmax(most, fabs(row[j]));
    }
    for (k = 0; k < terms; ++k) bound -= weight[k] * q->offset[index[k]];
    /* a row with no lengths in it either holds or cannot: keep it only if it cannot, as 0 >= 1 */
    if (most < 1e-12) { if (bound < 1e-9) return; most = bound; }
    for (j = 0; j < m; ++j) row[j] /= most;
    q->b[q->rows] = bound / most; q->row_contact[q->rows] = contact; q->row_state[q->rows] = state;
    q->rows += 1;
}

/* The convex programme for the cases in nd->state: its answer, its value and which contacts
 * might do better in another case. */
static void node_solve(Sl *s, Node *nd) {
    SlPlan      *q = s->plan;
    const int    m = s->muscles, nu = q->nu, c3 = 3 * s->contacts, wide = c3 + 1 + m;
    const double narrow = cos(0.39269908169872414);   /* an edge's middle is this much nearer than its ends */
    int          i, j, k, r, kept = 0, active = 0;
    bool         lets_go[SL_CONTACTS] = {false}, solvable = true;

    /* 1. three equalities a contact: [E | constant | lengths], impulses = E^-1 (constant + lengths part) */
    memset(q->e, 0, (size_t)c3 * wide * sizeof(double));
    for (i = 0; i < s->contacts; ++i) {
        const double angle = (nd->state[i] - SL_SLIDE + 0.5) * 0.78539816339744831, co = cos(angle), si = sin(angle);
        for (k = 0; k < 3; ++k) {
            double       *row = q->e + (3 * i + k) * wide;
            const double *sn = q->ss + (3 * i) * nu, *sx = sn + nu, *sz = sx + nu;
            if (nd->state[i] == SL_FREE) { row[3 * i + k] = 1.0; continue; }
            if (k == 0 || nd->state[i] == SL_STATIC) {
                /* this velocity is given: the normal one the bias, a static contact's others 0 */
                const double *sv = q->ss + (3 * i + k) * nu;
                memcpy(row, sv + m, (size_t)c3 * sizeof(double));
                row[c3] = (k ? 0.0 : q->bias[i] / q->speed_unit) - q->vc0[3 * i + k] / q->speed_unit;
                for (j = 0; j < m; ++j) row[c3 + 1 + j] = -sv[j];
                continue;
            }
            if (k == 1) {
                /* sliding: no velocity across the edge's middle direction */
                for (j = 0; j < c3; ++j) row[j] = -si * sx[m + j] + co * sz[m + j];
                row[c3] = (si * q->vc0[3 * i + 1] - co * q->vc0[3 * i + 2]) / q->speed_unit;
                for (j = 0; j < m; ++j) row[c3 + 1 + j] = si * sx[j] - co * sz[j];
                continue;
            }
            /* sliding: friction on the edge */
            row[3 * i] = -s->friction[nd->state[i] - SL_SLIDE] * narrow; row[3 * i + 1] = co; row[3 * i + 2] = si;
        }
    }
    for (i = 0; i < c3; ++i) {
        /* elimination with the largest entry of the column as pivot */
        int pivot = i;
        for (r = i + 1; r < c3; ++r)
            if (fabs(q->e[r * wide + i]) > fabs(q->e[pivot * wide + i])) pivot = r;
        solvable &= fabs(q->e[pivot * wide + i]) > 1e-12;
        for (j = 0; j < wide; ++j) { const double held = q->e[i * wide + j]; q->e[i * wide + j] = q->e[pivot * wide + j]; q->e[pivot * wide + j] = held; }
        for (r = 0; r < c3; ++r) {
            const double t = r == i ? 0.0 : q->e[r * wide + i] / q->e[i * wide + i];
            if (t == 0.0) continue;
            for (j = i; j < wide; ++j) q->e[r * wide + j] -= t * q->e[i * wide + j];
        }
    }
    /* 2. impulses and contact velocities as functions of the lengths */
    for (i = 0; i < c3; ++i) {
        q->offset[i] = q->e[i * wide + c3] / q->e[i * wide + i];
        for (j = 0; j < m; ++j) q->quantity[i * m + j] = q->e[i * wide + c3 + 1 + j] / q->e[i * wide + i];
    }
    for (i = 0; i < c3; ++i) {
        const double *sv = q->ss + i * nu;
        q->offset[c3 + i] = q->vc0[i] / q->speed_unit;
        for (j = 0; j < m; ++j) q->quantity[(c3 + i) * m + j] = sv[j];
        for (k = 0; k < c3; ++k) {
            q->offset[c3 + i] += sv[m + k] * q->offset[k];
            for (j = 0; j < m; ++j) q->quantity[(c3 + i) * m + j] += sv[m + k] * q->quantity[k * m + j];
        }
    }
    /* 3. the objective over the lengths */
    nd->objective = q->ease * m;
    for (i = 0; i < m; ++i) {
        q->q[i] = -2.0 * q->ease;
        for (j = 0; j <= i; ++j) q->p[i * m + j] = i == j ? 2.0 * q->ease : 0.0;
    }
    for (k = 0; k < q->goals; ++k) {
        const double *t = q->t + k * nu, weight = s->goal_weight[k];
        double        miss = q->miss[k], *row = q->x;
        for (j = 0; j < m; ++j) row[j] = t[j];
        for (i = 0; i < c3; ++i) {
            miss -= t[m + i] * q->offset[i];
            for (j = 0; j < m; ++j) row[j] += t[m + i] * q->quantity[i * m + j];
        }
        nd->objective += weight * miss * miss;
        for (i = 0; i < m; ++i) {
            q->q[i] -= 2.0 * weight * miss * row[i];
            for (j = 0; j <= i; ++j) q->p[i * m + j] += 2.0 * weight * row[i] * row[j];
        }
    }
    memcpy(q->work, q->p, (size_t)m * m * sizeof(double));
    /* 4. the inequalities: the muscles' range, and for every contact those of its case */
    q->rows = 0;
    for (j = 0; j < 2 * m; ++j) {
        double *row = q->c + q->rows * m;
        memset(row, 0, (size_t)m * sizeof(double));
        row[j / 2] = j % 2 ? -1.0 : 1.0; q->b[q->rows] = j % 2 ? -1.0 : s->least[j / 2] / s->length0[j / 2];
        q->row_contact[q->rows] = -1; q->row_state[q->rows] = 0; q->rows += 1;
    }
    for (i = 0; i < s->contacts; ++i) {
        const int    e = nd->state[i] - SL_SLIDE, fn[3] = {3 * i, 3 * i + 1, 3 * i + 2}, vn[3] = {c3 + 3 * i, c3 + 3 * i + 1, c3 + 3 * i + 2};
        const double angle = (e + 0.5) * 0.78539816339744831, co = cos(angle), si = sin(angle), one = 1.0;
        if (nd->state[i] == SL_FREE) { row_add(q, m, 1, vn, &one, q->bias[i] / q->speed_unit, i, SL_STATIC); continue; }
        row_add(q, m, 1, fn, &one, 0.0, i, SL_FREE);
        for (j = 0; j < 8; ++j) {
            /* friction inside edge j of the cone (a sliding contact is on its own edge already) */
            const double turn = (j + 0.5) * 0.78539816339744831, weight[3] = {s->friction[j] * narrow, -cos(turn), -sin(turn)};
            if (j != e) row_add(q, m, 3, fn, weight, 0.0, i, SL_SLIDE + j);
        }
        if (nd->state[i] >= SL_SLIDE) {
            /* it moves against the friction */
            const double against[3] = {0.0, -co, -si};
            row_add(q, m, 3, vn, against, 0.0, i, SL_STATIC);
        }
    }
    nd->feasible = solvable && qp_solve(m, q->p, q->q, q->rows, q->c, q->b, q->x, q->set, &active, q->work + m * m, &s->sweeps);
    nd->expanded = false;
    s->solved += 1;

    for (i = 0; i < m; ++i) {
        double t = 0.0;
        for (j = 0; j < m; ++j) t += q->work[(i > j ? i * m + j : j * m + i)] * q->x[j];
        nd->objective += q->x[i] * (0.5 * t + q->q[i]);
        nd->u[i] = q->x[i] * s->length0[i];
    }
    for (i = 0; i < c3; ++i) {
        double t = q->offset[i];
        for (j = 0; j < m; ++j) t += q->quantity[i * m + j] * q->x[j];
        nd->u[m + i] = t * q->impulse_unit;
    }
    if (!nd->feasible) nd->objective = 1e300;

    /* the inequalities the answer touches; a contact whose impulse is 0 is only asked to let go */
    nd->candidates = 0;
    for (r = 0; r < (nd->feasible ? q->rows : 0); ++r) {
        double slack = -q->b[r];
        for (j = 0; j < m; ++j) slack += q->c[r * m + j] * q->x[j];
        if (q->row_contact[r] < 0 || slack > 1e-8) continue;
        nd->candidate_contact[nd->candidates] = q->row_contact[r]; nd->candidate_state[nd->candidates] = q->row_state[r];
        nd->candidates += 1;
        lets_go[q->row_contact[r]] |= q->row_state[r] == SL_FREE;
    }
    for (r = 0; r < nd->candidates; ++r) {
        if (lets_go[nd->candidate_contact[r]] && nd->candidate_state[r] != SL_FREE) continue;
        nd->candidate_contact[kept] = nd->candidate_contact[r]; nd->candidate_state[kept] = nd->candidate_state[r];
        kept += 1;
    }
    nd->candidates = kept;
}

/* Contact impulses for given wanted lengths by projected Gauss-Seidel sweeps (friction
 * outside the cone is shortened onto it), and what each contact did. Used when no programme
 * chose them. */
static void contacts_resolve(const Sl *s, const double *wanted, double *impulse, int *did) {
    const SlPlan *q = s->plan;
    const int     m = s->muscles, nu = q->nu, c3 = 3 * s->contacts;
    const double  narrow = cos(0.39269908169872414);
    double        free_velocity[3 * SL_CONTACTS], velocity[3];
    int           sweep, i, j, k, e;

    for (i = 0; i < c3; ++i) {
        free_velocity[i] = q->vc0[i];
        for (j = 0; j < m; ++j) free_velocity[i] += q->s[i * nu + j] * wanted[j];
    }
    for (sweep = 0; sweep < 300; ++sweep)
        for (i = 0; i < s->contacts; ++i) {
            double *f = impulse + 3 * i, keep = 1.0;
            for (k = 0; k < 3; ++k) {
                const double *row = q->s + (3 * i + k) * nu + m;
                double        t = free_velocity[3 * i + k];
                for (j = 0; j < c3; ++j) t += row[j] * impulse[j];
                f[k] -= (t - (k ? 0.0 : q->bias[i])) / row[3 * i + k];
                if (k == 0) f[0] = fmax(f[0], 0.0);
            }
            for (e = 0; e < 8; ++e) {
                const double along = cos((e + 0.5) * 0.78539816339744831) * f[1] + sin((e + 0.5) * 0.78539816339744831) * f[2], limit = s->friction[e] * narrow * f[0];
                if (along > limit) keep = fmin(keep, limit / along);
            }
            f[1] *= keep; f[2] *= keep;
        }
    for (i = 0; i < s->contacts; ++i) {
        const double *f = impulse + 3 * i;
        double        most = -1.0;
        int           edge = 0;
        for (e = 0; e < 8; ++e) {
            /* the edge the friction is on or nearest to */
            const double along = (cos((e + 0.5) * 0.78539816339744831) * f[1] + sin((e + 0.5) * 0.78539816339744831) * f[2]) / (s->friction[e] * narrow);
            if (along > most) { most = along; edge = e; }
        }
        for (k = 1; k < 3; ++k) {
            velocity[k] = free_velocity[3 * i + k];
            for (j = 0; j < c3; ++j) velocity[k] += q->s[(3 * i + k) * nu + m + j] * impulse[j];
        }
        did[i] = f[0] <= 0.0 ? SL_FREE : most > 0.999 * f[0] && sqrt(velocity[1] * velocity[1] + velocity[2] * velocity[2]) > 1e-6 ? SL_SLIDE + edge : SL_STATIC;
    }
}

Sl *sl_open(int nodes, const double *rest, int tets, const uint32_t *tet, int muscles, const int *end, const int *group, double reach) {
    Sl          *s = calloc(1, sizeof *s);
    const int    n3 = 3 * nodes, nu = muscles + 3 * SL_CONTACTS, rows = 2 * muscles + 9 * SL_CONTACTS;
    SlPlan      *q = calloc(1, sizeof *q);
    int          t, i, j, k;

    s->nodes = nodes; s->tets = tets; s->muscles = muscles; s->reach = reach;
    s->young = 4e5; s->poisson = 0.45; s->damping = 0.2; s->density = 1000.0; s->gravity = 9.81; s->dt = 1.0 / 60.0;
    for (i = 0; i < 8; ++i) s->friction[i] = 0.8;
    s->stiffness = 4.8e5; s->relax = 1e-6; s->search = 1; s->visits = 16; s->warm = true;
    s->rest = malloc((size_t)n3 * sizeof(double)); memcpy(s->rest, rest, (size_t)n3 * sizeof(double));
    s->p = malloc((size_t)n3 * sizeof(double)); memcpy(s->p, rest, (size_t)n3 * sizeof(double));
    s->v = calloc((size_t)n3, sizeof(double)); s->push = calloc((size_t)n3, sizeof(double)); s->mass = calloc((size_t)nodes, sizeof(double));
    s->tet = malloc((size_t)tets * 4 * sizeof(int)); memcpy(s->tet, tet, (size_t)tets * 4 * sizeof(int));
    s->end = malloc((size_t)muscles * 2 * sizeof(int) + 8); memcpy(s->end, end, (size_t)muscles * 2 * sizeof(int));
    s->group = malloc((size_t)muscles * sizeof(int) + 8); memcpy(s->group, group, (size_t)muscles * sizeof(int));
    s->length0 = malloc((size_t)muscles * sizeof(double) + 8); s->length = malloc((size_t)muscles * sizeof(double) + 8);
    s->wanted = malloc((size_t)muscles * sizeof(double) + 8); s->least = malloc((size_t)muscles * sizeof(double) + 8);
    s->direction = malloc((size_t)muscles * 3 * sizeof(double) + 8);
    s->tet_inverse = malloc((size_t)tets * 9 * sizeof(double)); s->tet_volume = malloc((size_t)tets * sizeof(double));
    s->weight = calloc((size_t)tets * (size_t)muscles + 1, sizeof(double));
    s->k = malloc((size_t)n3 * n3 * sizeof(double)); s->factor = malloc((size_t)n3 * n3 * sizeof(double));
    s->a = malloc((size_t)n3 * (size_t)muscles * sizeof(double) + 8); s->x = malloc((size_t)n3 * (size_t)nu * sizeof(double)); s->v0 = malloc((size_t)n3 * sizeof(double));
    s->goal_room = 16; s->goal_row = malloc((size_t)s->goal_room * n3 * sizeof(double));
    s->goal_target = malloc((size_t)s->goal_room * sizeof(double)); s->goal_weight = malloc((size_t)s->goal_room * sizeof(double));
    s->plan = q;
    q->s = malloc((size_t)3 * SL_CONTACTS * nu * sizeof(double)); q->ss = malloc((size_t)3 * SL_CONTACTS * nu * sizeof(double));
    q->t = malloc((size_t)64 * nu * sizeof(double)); q->miss = malloc(64 * sizeof(double));
    q->e = malloc((size_t)3 * SL_CONTACTS * (3 * SL_CONTACTS + 1 + muscles) * sizeof(double));
    q->quantity = malloc((size_t)6 * SL_CONTACTS * muscles * sizeof(double) + 8); q->offset = malloc((size_t)6 * SL_CONTACTS * sizeof(double));
    q->p = malloc((size_t)muscles * muscles * sizeof(double) + 8); q->q = malloc((size_t)muscles * sizeof(double) + 8); q->x = malloc((size_t)muscles * sizeof(double) + 8);
    q->c = malloc((size_t)rows * muscles * sizeof(double) + 8); q->b = malloc((size_t)rows * sizeof(double));
    q->work = malloc(((size_t)muscles * muscles * 2 + (size_t)rows * muscles + 3 * (size_t)muscles) * sizeof(double) + 8);
    q->row_contact = malloc((size_t)rows * sizeof(int)); q->row_state = malloc((size_t)rows * sizeof(int)); q->set = malloc((size_t)rows * sizeof(int));
    for (i = 0; i < NODES_MOST; ++i) q->node[i].u = malloc((size_t)nu * sizeof(double));

    /* every element: the inverse of its three rest edges (its rows are the gradients of the shape functions of corners 1 .. 3) and its volume */
    for (t = 0; t < tets; ++t) {
        const double *x0 = rest + 3 * tet[4 * t], *x1 = rest + 3 * tet[4 * t + 1], *x2 = rest + 3 * tet[4 * t + 2], *x3 = rest + 3 * tet[4 * t + 3];
        const double  d[9] = {x1[0] - x0[0], x2[0] - x0[0], x3[0] - x0[0], x1[1] - x0[1], x2[1] - x0[1], x3[1] - x0[1], x1[2] - x0[2], x2[2] - x0[2], x3[2] - x0[2]};
        const double  det = d[0] * (d[4] * d[8] - d[5] * d[7]) - d[1] * (d[3] * d[8] - d[5] * d[6]) + d[2] * (d[3] * d[7] - d[4] * d[6]);
        double       *inv = s->tet_inverse + 9 * t;
        inv[0] = (d[4] * d[8] - d[5] * d[7]) / det; inv[1] = (d[2] * d[7] - d[1] * d[8]) / det; inv[2] = (d[1] * d[5] - d[2] * d[4]) / det;
        inv[3] = (d[5] * d[6] - d[3] * d[8]) / det; inv[4] = (d[0] * d[8] - d[2] * d[6]) / det; inv[5] = (d[2] * d[3] - d[0] * d[5]) / det;
        inv[6] = (d[3] * d[7] - d[4] * d[6]) / det; inv[7] = (d[1] * d[6] - d[0] * d[7]) / det; inv[8] = (d[0] * d[4] - d[1] * d[3]) / det;
        s->tet_volume[t] = det / 6.0;
        for (k = 0; k < 4; ++k) s->mass[tet[4 * t + k]] += s->density * det / 24.0;
    }
    /* every muscle: rest length and direction; its weight in every element (eq. 5) */
    for (j = 0; j < muscles; ++j) {
        const double *a = rest + 3 * end[2 * j], *b = rest + 3 * end[2 * j + 1], ab[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const double  length = sqrt(ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2]);
        s->length0[j] = s->wanted[j] = s->length[j] = length; s->least[j] = 0.5 * length;
        for (k = 0; k < 3; ++k) s->direction[3 * j + k] = ab[k] / length;
        for (t = 0; t < tets; ++t) {
            double centre[3] = {0, 0, 0}, along = 0.0, distance = 0.0;
            for (k = 0; k < 12; ++k) centre[k % 3] += 0.25 * rest[3 * tet[4 * t + k / 3] + k % 3];
            for (k = 0; k < 3; ++k) along += (centre[k] - a[k]) * ab[k] / (length * length);
            along = fmin(fmax(along, 0.0), 1.0);
            for (k = 0; k < 3; ++k) distance += (centre[k] - a[k] - along * ab[k]) * (centre[k] - a[k] - along * ab[k]);
            s->weight[(size_t)t * muscles + j] = exp(-distance / (reach * reach));
        }
    }
    {
        /* normalised within the group: a second array so that the sums use the plain weights */
        double *plain = malloc((size_t)tets * (size_t)muscles * sizeof(double) + 8);
        memcpy(plain, s->weight, (size_t)tets * (size_t)muscles * sizeof(double));
        for (t = 0; t < tets; ++t)
            for (j = 0; j < muscles; ++j) {
                double sum = 0.0;
                for (i = 0; i < muscles; ++i) sum += group[i] == group[j] ? plain[(size_t)t * muscles + i] : 0.0;
                s->weight[(size_t)t * muscles + j] = plain[(size_t)t * muscles + j] / sum;
            }
        free(plain);
    }
    return s;
}

void sl_close(Sl *s) {
    SlPlan *q = s->plan;
    int     i;
    for (i = 0; i < NODES_MOST; ++i) free(q->node[i].u);
    free(q->s); free(q->ss); free(q->t); free(q->miss); free(q->e); free(q->quantity); free(q->offset); free(q->p); free(q->q); free(q->x); free(q->c); free(q->b); free(q->work); free(q->row_contact); free(q->row_state); free(q->set); free(q);
    free(s->rest); free(s->p); free(s->v); free(s->push); free(s->mass); free(s->tet); free(s->end); free(s->group); free(s->length0); free(s->length); free(s->wanted); free(s->least);
    free(s->direction); free(s->tet_inverse); free(s->tet_volume); free(s->weight); free(s->k); free(s->factor); free(s->a); free(s->x); free(s->v0);
    free(s->goal_row); free(s->goal_target); free(s->goal_weight); free(s);
}

double sl_centre(const Sl *s, double c[3]) {
    double mass = 0.0;
    int    i, k;
    c[0] = c[1] = c[2] = 0.0;
    for (i = 0; i < s->nodes; ++i) {
        mass += s->mass[i];
        for (k = 0; k < 3; ++k) c[k] += s->mass[i] * s->p[3 * i + k];
    }
    for (k = 0; k < 3; ++k) c[k] /= mass;
    return mass;
}

/* A new goal row, all 0. */
static double *goal_add(Sl *s, double weight, double target) {
    const int n3 = 3 * s->nodes;
    if (s->goals == s->goal_room) {
        s->goal_room *= 2;
        s->goal_row = realloc(s->goal_row, (size_t)s->goal_room * n3 * sizeof(double));
        s->goal_target = realloc(s->goal_target, (size_t)s->goal_room * sizeof(double)); s->goal_weight = realloc(s->goal_weight, (size_t)s->goal_room * sizeof(double));
    }
    s->goal_target[s->goals] = target; s->goal_weight[s->goals] = weight;
    memset(s->goal_row + (size_t)s->goals * n3, 0, (size_t)n3 * sizeof(double));
    return s->goal_row + (size_t)(s->goals++) * n3;
}

void sl_goal_point(Sl *s, const double weight[3], const double *share, const double target[3]) {
    int i, k;
    for (k = 0; k < 3; ++k) {
        double at = 0.0, *row;
        for (i = 0; i < s->nodes; ++i) at += share[i] * s->p[3 * i + k];
        row = goal_add(s, weight[k], target[k] - at);
        for (i = 0; i < s->nodes; ++i) row[3 * i + k] = s->dt * share[i];
    }
}

void sl_goal_momentum(Sl *s, double weight, const double rate[3]) {
    int i, k;
    for (k = 0; k < 3; ++k) {
        double now = 0.0, *row;
        for (i = 0; i < s->nodes; ++i) now += s->mass[i] * s->v[3 * i + k];
        row = goal_add(s, weight, now + s->dt * rate[k]);
        for (i = 0; i < s->nodes; ++i) row[3 * i + k] = s->mass[i];
    }
}

void sl_goal_spin(Sl *s, double weight, const double rate[3]) {
    double c[3];
    int    i, k;
    sl_centre(s, c);
    for (k = 0; k < 3; ++k) {
        /* component k of sum of mass * (p - c) x v, with the positions as they are now */
        const int k1 = (k + 1) % 3, k2 = (k + 2) % 3;
        double    now = 0.0, *row;
        for (i = 0; i < s->nodes; ++i) now += s->mass[i] * ((s->p[3 * i + k1] - c[k1]) * s->v[3 * i + k2] - (s->p[3 * i + k2] - c[k2]) * s->v[3 * i + k1]);
        row = goal_add(s, weight, now + s->dt * rate[k]);
        for (i = 0; i < s->nodes; ++i) { row[3 * i + k2] = s->mass[i] * (s->p[3 * i + k1] - c[k1]); row[3 * i + k1] = -s->mass[i] * (s->p[3 * i + k2] - c[k2]); }
    }
}

void sl_goal_base(Sl *s, double weight, int triangles, const int *triangle, double rate) {
    double *row = goal_add(s, weight, rate), area = 0.0;
    int     t, i;
    for (t = 0; t < triangles; ++t) {
        /* half of ((b - a) x (c - a)) . up, and its rate with the velocities after the step */
        const int     a = triangle[3 * t], b = triangle[3 * t + 1], c = triangle[3 * t + 2];
        const double *pa = s->p + 3 * a, *pb = s->p + 3 * b, *pc = s->p + 3 * c;
        const double  ux = pb[0] - pa[0], uz = pb[2] - pa[2], wx = pc[0] - pa[0], wz = pc[2] - pa[2];
        area += 0.5 * (uz * wx - ux * wz);
        row[3 * b + 2] += 0.5 * wx; row[3 * a + 2] -= 0.5 * wx; row[3 * c] += 0.5 * uz; row[3 * a] -= 0.5 * uz;
        row[3 * b] -= 0.5 * wz; row[3 * a] += 0.5 * wz; row[3 * c + 2] -= 0.5 * ux; row[3 * a + 2] += 0.5 * ux;
    }
    for (i = 0; i < 3 * s->nodes && area < 0.0; ++i) row[i] = -row[i];
}

void sl_step(Sl *s, bool advance) {
    SlPlan      *q = s->plan;
    const int    n3 = 3 * s->nodes, m = s->muscles;
    const double lame = s->young * s->poisson / ((1.0 + s->poisson) * (1.0 - 2.0 * s->poisson)), shear = s->young / (2.0 * (1.0 + s->poisson));
    double      *rhs = s->v0, total_mass = 0.0;
    int          t, i, j, k, a, b, nu, c, best = -1, count = 0;

    /* 1. every element: its rotation, its stiffness in the turned frame into K, its force (eq. 2), and the muscles' pull on its corners (eq. 3, 4) */
    memset(s->k, 0, (size_t)n3 * n3 * sizeof(double)); memset(rhs, 0, (size_t)n3 * sizeof(double)); memset(s->a, 0, (size_t)n3 * (size_t)m * sizeof(double));
    memset(s->mass, 0, (size_t)s->nodes * sizeof(double));
    for (j = 0; j < m; ++j) {
        const double *pa = s->p + 3 * s->end[2 * j], *pb = s->p + 3 * s->end[2 * j + 1];
        s->length[j] = sqrt((pb[0] - pa[0]) * (pb[0] - pa[0]) + (pb[1] - pa[1]) * (pb[1] - pa[1]) + (pb[2] - pa[2]) * (pb[2] - pa[2]));
    }
    for (t = 0; t < s->tets; ++t) {
        const int    *node = s->tet + 4 * t;
        const double *inv = s->tet_inverse + 9 * t, volume = s->tet_volume[t];
        const double *p[4] = {s->p + 3 * node[0], s->p + 3 * node[1], s->p + 3 * node[2], s->p + 3 * node[3]};
        double        f[9], r[9], g[4][3], rg[4][3], u[4][3], normal[4][3];
        int           turn;
        for (i = 0; i < 3; ++i)
            for (j = 0; j < 3; ++j) f[3 * i + j] = (p[1][i] - p[0][i]) * inv[j] + (p[2][i] - p[0][i]) * inv[3 + j] + (p[3][i] - p[0][i]) * inv[6 + j];
        /* the rotation nearest to f: average with the inverse transpose until it stays (Higham) */
        memcpy(r, f, sizeof r);
        for (turn = 0; turn < 12; ++turn) {
            const double co[9] = {r[4] * r[8] - r[5] * r[7], r[5] * r[6] - r[3] * r[8], r[3] * r[7] - r[4] * r[6], r[2] * r[7] - r[1] * r[8], r[0] * r[8] - r[2] * r[6],
                                  r[1] * r[6] - r[0] * r[7], r[1] * r[5] - r[2] * r[4], r[2] * r[3] - r[0] * r[5], r[0] * r[4] - r[1] * r[3]};
            const double det = r[0] * co[0] + r[1] * co[1] + r[2] * co[2];
            for (i = 0; i < 9; ++i) r[i] = 0.5 * (r[i] + co[i] / det);
        }
        for (k = 0; k < 3; ++k) { g[1][k] = inv[k]; g[2][k] = inv[3 + k]; g[3][k] = inv[6 + k]; g[0][k] = -inv[k] - inv[3 + k] - inv[6 + k]; }
        for (a = 0; a < 4; ++a) {
            const double *x = s->rest + 3 * node[a];
            const double *pb = p[(a + 1) % 4], *pc = p[(a + 2) % 4], *pd = p[(a + 3) % 4];
            const double  e1[3] = {pc[0] - pb[0], pc[1] - pb[1], pc[2] - pb[2]}, e2[3] = {pd[0] - pb[0], pd[1] - pb[1], pd[2] - pb[2]};
            double        away;
            for (k = 0; k < 3; ++k) {
                rg[a][k] = r[3 * k] * g[a][0] + r[3 * k + 1] * g[a][1] + r[3 * k + 2] * g[a][2];
                u[a][k] = p[a][k] - (r[3 * k] * x[0] + r[3 * k + 1] * x[1] + r[3 * k + 2] * x[2]);
            }
            /* the face opposite corner a: half the cross product, pointing away from a */
            normal[a][0] = 0.5 * (e1[1] * e2[2] - e1[2] * e2[1]); normal[a][1] = 0.5 * (e1[2] * e2[0] - e1[0] * e2[2]); normal[a][2] = 0.5 * (e1[0] * e2[1] - e1[1] * e2[0]);
            away = normal[a][0] * (p[a][0] - pb[0]) + normal[a][1] * (p[a][1] - pb[1]) + normal[a][2] * (p[a][2] - pb[2]);
            for (k = 0; k < 3; ++k) normal[a][k] = away > 0.0 ? -normal[a][k] : normal[a][k];
            s->mass[node[a]] += 0.25 * s->density * volume;
        }
        for (a = 0; a < 4; ++a)
            for (b = 0; b < 4; ++b) {
                const double dot = g[a][0] * g[b][0] + g[a][1] * g[b][1] + g[a][2] * g[b][2];
                const double bu = rg[b][0] * u[b][0] + rg[b][1] * u[b][1] + rg[b][2] * u[b][2], au = rg[a][0] * u[b][0] + rg[a][1] * u[b][1] + rg[a][2] * u[b][2];
                for (i = 0; i < 3; ++i) {
                    rhs[3 * node[a] + i] -= volume * (lame * rg[a][i] * bu + shear * rg[b][i] * au + shear * dot * u[b][i]);
                    for (j = 0; j < 3; ++j)
                        s->k[(size_t)(3 * node[a] + i) * n3 + 3 * node[b] + j] += volume * (lame * rg[a][i] * rg[b][j] + shear * rg[b][i] * rg[a][j] + (i == j ? shear * dot : 0.0));
                }
            }
        for (j = 0; j < m; ++j) {
            const double weight = s->weight[(size_t)t * m + j], *d = s->direction + 3 * j;
            const double rd[3] = {r[0] * d[0] + r[1] * d[1] + r[2] * d[2], r[3] * d[0] + r[4] * d[1] + r[5] * d[2], r[6] * d[0] + r[7] * d[1] + r[8] * d[2]};
            if (weight < 1e-4) continue;
            for (a = 0; a < 4; ++a) {
                /* the stress rd rd' on the three faces at corner a, a third of each: minus a third of it on the face opposite */
                const double through = rd[0] * normal[a][0] + rd[1] * normal[a][1] + rd[2] * normal[a][2];
                for (k = 0; k < 3; ++k) s->a[(size_t)(3 * node[a] + k) * m + j] -= s->stiffness / s->length0[j] * weight * rd[k] * through / 3.0;
            }
        }
    }

    /* 2. the step's matrix M + dt C + dt^2 K, factored; the velocity the step gives with no contact and wanted lengths of 0 (eq. 8) */
    for (i = 0; i < n3; ++i) {
        double pull = 0.0;
        for (j = 0; j < m; ++j) pull += s->a[(size_t)i * m + j] * s->length[j];
        rhs[i] = s->mass[i / 3] * s->v[i] + s->dt * (rhs[i] + s->push[i] - (i % 3 == 1 ? s->mass[i / 3] * s->gravity : 0.0) - pull);
        for (j = 0; j <= i; ++j) s->factor[(size_t)i * n3 + j] = (s->dt * s->damping + s->dt * s->dt) * s->k[(size_t)i * n3 + j] + (i == j ? s->mass[i / 3] : 0.0);
    }
    cholesky(s->factor, n3);
    cholesky_solve(s->factor, n3, s->v0);

    /* 3. contacts: the nodes within 2 cm of the ground, the lowest first if there are too many. A node above the ground may come down to it; one below is pushed out by a fifth of its depth a step. */
    s->contacts = 0;
    for (i = 0; i < s->nodes; ++i) {
        const double y = s->p[3 * i + 1];
        if (y > 0.02 || (s->contacts == SL_CONTACTS && y >= s->p[3 * s->contact_node[SL_CONTACTS - 1] + 1])) continue;
        s->contacts += s->contacts < SL_CONTACTS;
        for (j = s->contacts - 1; j > 0 && s->p[3 * s->contact_node[j - 1] + 1] > y; --j) s->contact_node[j] = s->contact_node[j - 1];
        s->contact_node[j] = i;
    }
    c = s->contacts; nu = q->nu = m + 3 * c;
    for (i = 0; i < s->nodes; ++i) total_mass += s->mass[i];
    for (j = 0; j < m; ++j) {
        double *column = s->x + (size_t)j * n3;
        for (i = 0; i < n3; ++i) column[i] = s->dt * s->a[(size_t)i * m + j];
        cholesky_solve(s->factor, n3, column);
    }
    for (i = 0; i < 3 * c; ++i) {
        /* contact i / 3: normal (y), then x, then z */
        static const int axis[3] = {1, 0, 2};
        double *column = s->x + (size_t)(m + i) * n3;
        memset(column, 0, (size_t)n3 * sizeof(double));
        column[3 * s->contact_node[i / 3] + axis[i % 3]] = 1.0;
        cholesky_solve(s->factor, n3, column);
    }
    for (i = 0; i < 3 * c; ++i) {
        static const int axis[3] = {1, 0, 2};
        const int row = 3 * s->contact_node[i / 3] + axis[i % 3];
        const double y = s->p[3 * s->contact_node[i / 3] + 1];
        q->vc0[i] = s->v0[row];
        for (j = 0; j < nu; ++j) q->s[(size_t)i * nu + j] = s->x[(size_t)j * n3 + row];
        if (i % 3 == 0) q->bias[i / 3] = y > 0.0 ? -y / s->dt : -0.2 * y / s->dt;
    }

    /* 4. the wanted lengths, if there are goals (section 5, 6) */
    s->solved = 0; s->sweeps = 0; s->planned = false; s->objective = s->first_objective = 0.0;
    if (s->goals > 0) {
        /* the goals in the scaled unknowns: goal k misses by t[k] . u - miss[k] */
        q->goals = s->goals; q->impulse_unit = total_mass * s->gravity * s->dt; q->speed_unit = s->gravity * s->dt;
        for (k = 0; k < s->goals; ++k) {
            const double *row = s->goal_row + (size_t)k * n3;
            q->miss[k] = s->goal_target[k];
            for (i = 0; i < n3; ++i) q->miss[k] -= row[i] * s->v0[i];
            for (j = 0; j < nu; ++j) {
                const double *column = s->x + (size_t)j * n3;
                double        sum = 0.0;
                for (i = 0; i < n3; ++i) sum += row[i] * column[i];
                q->t[k * nu + j] = sum * (j < m ? s->length0[j] : q->impulse_unit);
            }
        }
        q->ease = s->relax;
        for (i = 0; i < 3 * c; ++i)
            for (j = 0; j < nu; ++j) q->ss[(size_t)i * nu + j] = q->s[(size_t)i * nu + j] * (j < m ? s->length0[j] : q->impulse_unit) / q->speed_unit;

        if (s->search == 2) {
            /* every combination of the ten cases: the best is kept in node 0 */
            int digit[SL_CONTACTS] = {0}, combinations = 1;
            for (i = 0; i < c; ++i) combinations *= 10;
            q->node[0].objective = 1e300; q->node[0].feasible = false;
            for (t = 0; t < combinations; ++t) {
                Node *nd = &q->node[1];
                for (i = 0; i < c; ++i) nd->state[i] = digit[i];
                node_solve(s, nd);
                if (nd->objective < q->node[0].objective) { double *u = q->node[0].u; q->node[0] = *nd; q->node[0].u = u; memcpy(u, nd->u, (size_t)nu * sizeof(double)); }
                for (i = 0; i < c && ++digit[i] == 10; ++i) digit[i] = 0;
            }
            count = 1;
        } else {
            /* the first guess: what touches the ground stays where it is (eq. 20 to 22), or what the last step chose */
            Node *root = &q->node[0];
            for (i = 0; i < c; ++i) {
                root->state[i] = s->p[3 * s->contact_node[i] + 1] < 1e-3 ? SL_STATIC : SL_FREE;
                for (j = 0; j < (s->warm && s->search == 1 ? s->last_contacts : 0); ++j)
                    if (s->last_node[j] == s->contact_node[i]) root->state[i] = s->last_plan[j];
            }
            node_solve(s, root);
            s->first_objective = root->objective;
            for (i = 0; i < c && !root->feasible; ++i) root->state[i] = s->p[3 * s->contact_node[i] + 1] < 1e-3 ? SL_STATIC : SL_FREE;
            if (!root->feasible && s->warm && s->search == 1) node_solve(s, root);
            if (!root->feasible && s->search == 1) {
                /* neither can be met: the cases of a plain step with the muscles as they are (end of section 6) */
                double impulse[3 * SL_CONTACTS] = {0};
                contacts_resolve(s, s->wanted, impulse, root->state);
                node_solve(s, root);
            }
            count = 1;
            while (s->search == 1) {
                /* the best programme not yet looked at: try each change its answer points to */
                int pick = -1;
                for (i = 0; i < count; ++i)
                    if (q->node[i].feasible && !q->node[i].expanded && (pick < 0 || q->node[i].objective < q->node[pick].objective)) pick = i;
                if (pick < 0 || count >= s->visits || count >= NODES_MOST) break;
                q->node[pick].expanded = true;
                for (k = 0; k < q->node[pick].candidates && count < s->visits && count < NODES_MOST; ++k) {
                    Node *child = &q->node[count];
                    memcpy(child->state, q->node[pick].state, sizeof child->state);
                    child->state[q->node[pick].candidate_contact[k]] = q->node[pick].candidate_state[k];
                    for (i = 0; i < count && memcmp(q->node[i].state, child->state, (size_t)c * sizeof(int)); ++i) {}
                    if (i < count) continue;
                    node_solve(s, child);
                    count += 1;
                }
            }
        }
        for (i = 0; i < count; ++i)
            if (q->node[i].feasible && (best < 0 || q->node[i].objective < q->node[best].objective)) best = i;
        if (best >= 0) {
            s->planned = true; s->objective = q->node[best].objective;
            for (j = 0; j < m; ++j) s->wanted[j] = fmin(fmax(q->node[best].u[j], s->least[j]), s->length0[j]);
            memcpy(s->contact_plan, q->node[best].state, (size_t)c * sizeof(int));
            s->last_contacts = c; memcpy(s->last_node, s->contact_node, (size_t)c * sizeof(int)); memcpy(s->last_plan, s->contact_plan, (size_t)c * sizeof(int));
        }
    }
    s->goals = 0;
    if (!advance) return;

    /* 5. the body moves: contact impulses for the lengths chosen, then eq. 8 and 9 */
    if (best >= 0) {
        /* the programme's impulses meet eq. 11 for its cases: they are the step's */
        memcpy(s->impulse, q->node[best].u + m, (size_t)(3 * c) * sizeof(double)); memcpy(s->contact_did, s->contact_plan, (size_t)c * sizeof(int));
    } else {
        memset(s->impulse, 0, sizeof s->impulse);
        contacts_resolve(s, s->wanted, s->impulse, s->contact_did);
    }
    memcpy(s->v, s->v0, (size_t)n3 * sizeof(double));
    for (j = 0; j < nu; ++j) {
        const double *column = s->x + (size_t)j * n3, amount = j < m ? s->wanted[j] : s->impulse[j - m];
        for (i = 0; i < n3; ++i) s->v[i] += column[i] * amount;
    }
    for (i = 0; i < n3; ++i) s->p[i] += s->dt * s->v[i];
}
