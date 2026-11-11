/* method.c — Truong, Yuksel, Watcharopas, Levine, Kirby: "Particle Merging-and-Splitting"
 * (IEEE TVCG 2021), sections 3 and 5.1, for discs in the plane.
 *
 * Two particles that collide become one meta-particle for the length of a time step (eq. 1
 * to 3). The integrator moves it like any other particle, so whatever else acts in that
 * step (springs, gravity, other particles) has its say in how the collision ends. Then the
 * meta-particle is split again (eq. 8 to 16), giving back the kinetic energy the merge took
 * (eq. 4), which was kept in a "virtual bond" meanwhile.
 *
 * Not here: coupling two different integrators (section 4, eq. 17 to 20), fluids, fracture.
 * Walls are this demo's own: a particle is turned back at a
 * wall, and a meta-particle when any of its particles would cross one.
 */
#include "method.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define NONE UINT32_MAX

double ms_merge(double m1, const double v1[2], double m2, const double v2[2], double v12[2]) {
    const double dx = v1[0] - v2[0], dy = v1[1] - v2[1];
    v12[0] = (m1 * v1[0] + m2 * v2[0]) / (m1 + m2);
    v12[1] = (m1 * v1[1] + m2 * v2[1]) / (m1 + m2);
    return m1 * m2 / (2.0 * (m1 + m2)) * (dx * dx + dy * dy);
}

bool ms_split(double m1, const double v1[2], double m2, const double n[2], double energy, const double v12[2], double out1[2], double out2[2]) {
    const double m12 = m1 + m2, s2 = 2.0 * energy / (m12 * (m1 / m2));               /* eq. 11 */
    const double d[2] = {v12[0] - v1[0], v12[1] - v1[1]};
    /* eq. 13 with epsilon = 0: mu^2 - 2 b mu + c = 0. The smaller root is the one that parts them (eq. 14). */
    const double b = n[0] * d[0] + n[1] * d[1], c = d[0] * d[0] + d[1] * d[1] - s2, inside = b * b - c;
    const double mu = b - sqrt(fmax(inside, 0.0));
    double       epsilon[2] = {0.0, 0.0};
    if (inside < 0.0) {
        /* no root: the nearest point of the line v1 + mu n to the circle of eq. 10 (eq. 15, 16) */
        const double w[2] = {d[0] - mu * n[0], d[1] - mu * n[1]}, length = sqrt(w[0] * w[0] + w[1] * w[1]);
        epsilon[0] = (length - sqrt(s2)) * w[0] / length;
        epsilon[1] = (length - sqrt(s2)) * w[1] / length;
    }
    out1[0] = v1[0] + mu * n[0] + epsilon[0];
    out1[1] = v1[1] + mu * n[1] + epsilon[1];
    out2[0] = (m12 * v12[0] - m1 * out1[0]) / m2;                                    /* eq. 8 */
    out2[1] = (m12 * v12[1] - m1 * out1[1]) / m2;
    return inside >= 0.0;
}

uint32_t ms_add(Ms *s, double x, double y, double mass, double radius) {
    s->particle = (MsParticle *)realloc(s->particle, (s->particles + 1u) * sizeof(MsParticle));
    s->particle[s->particles] = (MsParticle){{x, y}, {0.0, 0.0}, mass, radius, false};
    return s->particles++;
}

void ms_link(Ms *s, uint32_t a, uint32_t b, double stiffness) {
    const double dx = s->particle[b].x[0] - s->particle[a].x[0], dy = s->particle[b].x[1] - s->particle[a].x[1];
    s->spring = (MsSpring *)realloc(s->spring, (s->springs + 1u) * sizeof(MsSpring));
    s->spring[s->springs++] = (MsSpring){a, b, sqrt(dx * dx + dy * dy), stiffness};
}

double ms_energy(const Ms *s) {
    double   energy = 0.0;
    uint32_t i;
    for (i = 0u; i < s->particles; ++i) {
        const MsParticle *p = &s->particle[i];
        energy += 0.5 * p->mass * (p->v[0] * p->v[0] + p->v[1] * p->v[1]) + p->mass * s->gravity * p->x[1];
    }
    for (i = 0u; i < s->springs; ++i) {
        const MsParticle *a = &s->particle[s->spring[i].a], *b = &s->particle[s->spring[i].b];
        const double      stretch = sqrt((b->x[0] - a->x[0]) * (b->x[0] - a->x[0]) + (b->x[1] - a->x[1]) * (b->x[1] - a->x[1])) - s->spring[i].rest;
        energy += 0.5 * s->spring[i].stiffness * stretch * stretch;
    }
    return energy;
}

/* A particle or a meta-particle. The first `particles` nodes are the particles themselves. */
typedef struct Node {
    double   mass, v[2];       /* v: what it had when it was merged into its parent, or at the start */
    double   after[2];         /* its velocity at the end of the step                                 */
    double   force[2];
    uint32_t parent, a, b, size;
    double   normal[2], bond;  /* of the merge that made it: unit vector a -> b, energy of eq. 4      */
} Node;

typedef struct Pair {
    uint32_t i, j;
    double   n[2];             /* unit vector i -> j */
    double   depth;
    uint8_t  state;            /* 0 seen, 1 marked: apart at the start; 2 must merge in the second stage */
} Pair;

void ms_step(Ms *s) {
    const uint32_t n = s->particles;
    Node          *node = (Node *)malloc(2u * (size_t)n * sizeof(Node));
    Pair          *pair = NULL;
    double        *force = (double *)calloc(2u * (size_t)n, sizeof(double));
    uint32_t       pairs = 0u, room = 0u, nodes, i, k, stage, at;
    bool           again = false;

    /* Collision rule 1 (eq. 21): the particles intersect. Neighbours joined by a spring are left alone (5.1). */
    for (i = 0u; i < n; ++i)
        for (k = i + 1u; k < n; ++k) {
            const MsParticle *a = &s->particle[i], *b = &s->particle[k];
            const double      dx = b->x[0] - a->x[0], dy = b->x[1] - a->x[1], reach = a->radius + b->radius, far2 = dx * dx + dy * dy;
            bool              joined = false;
            if (far2 >= reach * reach || far2 == 0.0 || a->pinned || b->pinned) continue;
            for (at = 0u; at < s->springs && !joined; ++at)
                joined = (s->spring[at].a == i && s->spring[at].b == k) || (s->spring[at].a == k && s->spring[at].b == i);
            if (joined) continue;
            if (pairs == room) { room = room ? 2u * room : 256u; pair = (Pair *)realloc(pair, room * sizeof(Pair)); }
            pair[pairs++] = (Pair){i, k, {dx / sqrt(far2), dy / sqrt(far2)}, reach - sqrt(far2), 0u};
        }

    /* Impulses change velocities before anything else: each approaching pair on its own, in turn. */
    for (at = 0u; at < pairs && s->method == MS_IMPULSE; ++at) {
        MsParticle  *a = &s->particle[pair[at].i], *b = &s->particle[pair[at].j];
        const double closing = pair[at].n[0] * (b->v[0] - a->v[0]) + pair[at].n[1] * (b->v[1] - a->v[1]);
        const double impulse = -(1.0 + sqrt(s->alpha)) * closing / (1.0 / a->mass + 1.0 / b->mass);
        if (closing >= 0.0) continue;
        for (k = 0u; k < 2u; ++k) { a->v[k] -= impulse * pair[at].n[k] / a->mass; b->v[k] += impulse * pair[at].n[k] / b->mass; }
    }

    /* Forces at the start of the step: weight, springs, and the penalty if that is the method. */
    for (i = 0u; i < n; ++i) force[2u * i + 1u] = -s->particle[i].mass * s->gravity;
    for (at = 0u; at < s->springs; ++at) {
        const MsSpring   *e = &s->spring[at];
        const MsParticle *a = &s->particle[e->a], *b = &s->particle[e->b];
        const double      d[2] = {b->x[0] - a->x[0], b->x[1] - a->x[1]}, length = sqrt(d[0] * d[0] + d[1] * d[1]);
        const double      closing = ((b->v[0] - a->v[0]) * d[0] + (b->v[1] - a->v[1]) * d[1]) / length;
        const double      pull = e->stiffness * (length - e->rest) + s->spring_damping * closing;
        for (k = 0u; k < 2u; ++k) { force[2u * e->a + k] += pull * d[k] / length; force[2u * e->b + k] -= pull * d[k] / length; }
    }
    for (at = 0u; at < pairs && s->method == MS_PENALTY; ++at)
        for (k = 0u; k < 2u; ++k) {
            force[2u * pair[at].i + k] -= s->penalty * pair[at].depth * pair[at].n[k];
            force[2u * pair[at].j + k] += s->penalty * pair[at].depth * pair[at].n[k];
        }

    /* Two stages (5.1). The first merges intersecting pairs whose particles approach (rule 2, eq. 22) and
     * marks the others. If the particles of a marked pair approach after the step, the step is done again
     * with those merged too. */
    for (stage = 0u; stage < 2u; ++stage) {
        uint32_t without = 0u, merged = 0u;
        for (i = 0u; i < n; ++i) {
            const MsParticle *p = &s->particle[i];
            node[i] = (Node){.mass = p->mass, .v = {p->v[0], p->v[1]}, .force = {force[2u * i], force[2u * i + 1u]}, .parent = NONE, .size = 1u};
        }
        nodes = n;
        for (at = 0u; at < pairs && s->method == MS_MERGE; ++at) {
            const MsParticle *p = &s->particle[pair[at].i], *q = &s->particle[pair[at].j];
            uint32_t          a = pair[at].i, b = pair[at].j;
            if (pair[at].state != 2u && pair[at].n[0] * (q->v[0] - p->v[0]) + pair[at].n[1] * (q->v[1] - p->v[1]) >= 0.0) {
                pair[at].state = 1u;
                continue;
            }
            while (node[a].parent != NONE) a = node[a].parent;
            while (node[b].parent != NONE) b = node[b].parent;
            if (a == b || node[a].size + node[b].size > s->limit) continue;
            /* A meta-particle is a particle like any other, so this is eq. 1 to 4 whatever a and b are made of.
             * The direction kept for the split is that of the two particles that touch. */
            node[nodes] = (Node){.mass = node[a].mass + node[b].mass, .force = {node[a].force[0] + node[b].force[0], node[a].force[1] + node[b].force[1]},
                                 .parent = NONE, .a = a, .b = b, .size = node[a].size + node[b].size, .normal = {pair[at].n[0], pair[at].n[1]}};
            node[nodes].bond = ms_merge(node[a].mass, node[a].v, node[b].mass, node[b].v, node[nodes].v);
            s->largest = node[nodes].size > s->largest ? node[nodes].size : s->largest;
            node[a].parent = node[b].parent = nodes++;
            merged++;
        }
        /* The integrator: every node without a parent is a particle to it. */
        for (k = 0u; k < nodes; ++k) {
            if (node[k].parent != NONE) continue;
            node[k].after[0] = node[k].v[0] + s->dt * node[k].force[0] / node[k].mass;
            node[k].after[1] = node[k].v[1] + s->dt * node[k].force[1] / node[k].mass;
        }
        for (i = 0u; i < n; ++i) {
            const MsParticle *p = &s->particle[i];
            uint32_t          root = i;
            while (node[root].parent != NONE) root = node[root].parent;
            for (k = 0u; k < 2u; ++k) {
                const double next = p->x[k] + s->dt * node[root].after[k];
                if ((next < p->radius && node[root].after[k] < 0.0) || (next > s->box[k] - p->radius && node[root].after[k] > 0.0)) node[root].after[k] = -node[root].after[k];
            }
        }
        /* Split in the reverse order of merging; a later merge has the higher index. */
        for (k = nodes; k-- > n;)
            without += !ms_split(node[node[k].a].mass, node[node[k].a].v, node[node[k].b].mass, node[k].normal, s->alpha * node[k].bond, node[k].after, node[node[k].a].after, node[node[k].b].after);
        for (at = 0u; at < pairs && stage == 0u; ++at) {
            const double *va = node[pair[at].i].after, *vb = node[pair[at].j].after;
            if (pair[at].state == 1u && pair[at].n[0] * (vb[0] - va[0]) + pair[at].n[1] * (vb[1] - va[1]) < 0.0) { pair[at].state = 2u; again = true; }
        }
        if (!again || stage == 1u) { s->without_root += without; s->merges += merged; break; }
        s->second_stages++;
    }

    /* The split comes after the positions (5.1): a meta-particle keeps its shape during the step, so each
     * particle moves as its outermost meta-particle did (eq. 6, 7), and leaves with its own velocity. */
    for (i = 0u; i < n; ++i) {
        MsParticle *p = &s->particle[i];
        uint32_t    root = i;
        while (node[root].parent != NONE) root = node[root].parent;
        for (k = 0u; k < 2u; ++k) {
            p->x[k] += p->pinned ? 0.0 : s->dt * node[root].after[k];
            p->v[k]  = p->pinned ? 0.0 : node[i].after[k];
        }
    }
    free(node); free(pair); free(force);
}
