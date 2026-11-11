/* softmesh.c — see softmesh.h. */
#include "softmesh.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct Side {
    uint32_t key[3];    /* the three nodes in rising order: the same for both tetrahedra that share it */
    uint32_t node[3];   /* as the tetrahedron has them, facing out                                      */
} Side;

static int side_order(const void *a, const void *b) { return memcmp(((const Side *)a)->key, ((const Side *)b)->key, sizeof(uint32_t[3])); }

/* The six orders in which the three axes can be walked from corner 000 to corner 111. */
static const uint8_t k_walk[6][3] = {{0, 1, 2}, {1, 2, 0}, {2, 0, 1}, {0, 2, 1}, {2, 1, 0}, {1, 0, 2}};

/* Fills m->face from m->tet. */
static void surface(SoftMesh *m) {
    /* The faces of a tetrahedron a b c d of positive volume, each seen anticlockwise from outside. */
    static const uint8_t k_face[4][3] = {{0, 2, 1}, {0, 1, 3}, {1, 2, 3}, {0, 3, 2}};
    Side    *sides;
    uint32_t t, f, k;

    /* A side that only one tetrahedron has is on the surface. */
    sides = (Side *)malloc((size_t)m->tets * 4u * sizeof(Side));
    for (t = 0u; t < m->tets; ++t)
        for (f = 0u; f < 4u; ++f) {
            Side *s = &sides[4u * t + f];
            for (k = 0u; k < 3u; ++k) s->key[k] = s->node[k] = m->tet[4u * t + k_face[f][k]];
            if (s->key[0] > s->key[1]) { const uint32_t swap = s->key[0]; s->key[0] = s->key[1]; s->key[1] = swap; }
            if (s->key[1] > s->key[2]) { const uint32_t swap = s->key[1]; s->key[1] = s->key[2]; s->key[2] = swap; }
            if (s->key[0] > s->key[1]) { const uint32_t swap = s->key[0]; s->key[0] = s->key[1]; s->key[1] = swap; }
        }
    qsort(sides, (size_t)m->tets * 4u, sizeof(Side), side_order);
    m->face  = (uint32_t *)malloc((size_t)m->tets * 12u * sizeof(uint32_t));
    m->faces = 0u;
    for (t = 0u; t < m->tets * 4u; ++t) {
        const bool twin_before = t > 0u && !side_order(&sides[t], &sides[t - 1u]);
        const bool twin_after  = t + 1u < m->tets * 4u && !side_order(&sides[t], &sides[t + 1u]);
        if (twin_before || twin_after) continue;
        memcpy(&m->face[3u * m->faces++], sides[t].node, sizeof(uint32_t[3]));
    }
    free(sides);
}

void softmesh_cells(SoftMesh *m, uint32_t nx, uint32_t ny, uint32_t nz, double cell, const uint8_t *solid) {
    const uint32_t stride[3] = {1u, nx + 1u, (nx + 1u) * (ny + 1u)}, lattice = (nx + 1u) * (ny + 1u) * (nz + 1u);
    uint32_t      *number = (uint32_t *)calloc(lattice, sizeof(uint32_t));   /* lattice point -> node, or 0 while unused */
    uint32_t       x, y, z, t, i;

    m->tets = 0u;
    m->tet  = (uint32_t *)malloc((size_t)nx * ny * nz * 24u * sizeof(uint32_t));
    for (z = 0u; z < nz; ++z)
        for (y = 0u; y < ny; ++y)
            for (x = 0u; x < nx; ++x)
                for (t = 0u; t < 6u && (!solid || solid[x + nx * (y + ny * z)]); ++t) {
                    uint32_t *q = &m->tet[4u * m->tets++];
                    q[0] = x + y * stride[1] + z * stride[2];
                    q[1] = q[0] + stride[k_walk[t][0]];
                    q[2] = q[1] + stride[k_walk[t][1]];
                    q[3] = q[2] + stride[k_walk[t][2]];
                    /* the last three walks are mirror images of the first three */
                    if (t >= 3u) { const uint32_t swap = q[1]; q[1] = q[2]; q[2] = swap; }
                }

    /* The lattice points that a tetrahedron uses become the nodes, in lattice order. */
    for (i = 0u; i < 4u * m->tets; ++i) number[m->tet[i]] = 1u;
    m->nodes = 0u;
    for (i = 0u; i < lattice; ++i) number[i] = number[i] ? ++m->nodes : 0u;
    m->rest = (double *)malloc((size_t)m->nodes * 3u * sizeof(double));
    for (i = 0u; i < lattice; ++i) {
        double *p = &m->rest[3u * (number[i] ? number[i] - 1u : 0u)];
        if (!number[i]) continue;
        p[0] = cell * (i % stride[1]); p[1] = cell * (i / stride[1] % (ny + 1u)); p[2] = cell * (i / stride[2]);
    }
    for (i = 0u; i < 4u * m->tets; ++i) m->tet[i] = number[m->tet[i]] - 1u;
    free(number);
    surface(m);
}

void softmesh_ring(SoftMesh *m, uint32_t around, uint32_t thick, uint32_t wide, double inner, double cell) {
    const uint32_t count[3] = {around, thick, wide}, stride[3] = {1u, around, around * (thick + 1u)};
    uint32_t i, j, k, t, w, tets = 0u;

    m->nodes = around * (thick + 1u) * (wide + 1u);
    m->tets  = 6u * around * thick * wide;
    m->rest  = (double *)malloc((size_t)m->nodes * 3u * sizeof(double));
    m->tet   = (uint32_t *)malloc((size_t)m->tets * 4u * sizeof(uint32_t));
    for (k = 0u; k <= wide; ++k)
        for (j = 0u; j <= thick; ++j)
            for (i = 0u; i < around; ++i) {
                /* clockwise seen from +z, so that (around, outwards, z) is right-handed like the box's axes */
                const double angle = -6.283185307179586 * i / around, radius = inner + cell * j;
                double      *p = &m->rest[3u * (i + j * stride[1] + k * stride[2])];
                p[0] = radius * sin(angle); p[1] = inner + cell * thick - radius * cos(angle); p[2] = cell * k;
            }
    for (k = 0u; k < wide; ++k)
        for (j = 0u; j < thick; ++j)
            for (i = 0u; i < around; ++i)
                for (t = 0u; t < 6u; ++t) {
                    uint32_t  at[3] = {i, j, k};
                    uint32_t *q = &m->tet[4u * tets++];
                    q[0] = i + j * stride[1] + k * stride[2];
                    for (w = 0u; w < 3u; ++w) {
                        at[k_walk[t][w]] += 1u;
                        q[w + 1u] = at[0] % count[0] + at[1] * stride[1] + at[2] * stride[2];
                    }
                    if (t >= 3u) { const uint32_t swap = q[1]; q[1] = q[2]; q[2] = swap; }
                }
    surface(m);
}

void softmesh_draw(const SoftMesh *m, const double *x, Canvas *c, uint32_t rgba, float edge) {
    uint32_t f, k, i;
    for (f = 0u; f < m->faces; ++f) {
        float p[3][3], lifted[3][3];
        for (k = 0u; k < 3u; ++k)
            for (i = 0u; i < 3u; ++i) {
                p[k][i] = (float)x[3u * m->face[3u * f + k] + i];
                /* edges sit a little towards the camera so that the face does not cover them */
                lifted[k][i] = c->flat ? p[k][i] + (i == 2u ? 0.01f : 0.0f) : p[k][i] + 0.004f * (c->eye[i] - p[k][i]);
            }
        canvas_lit(c, p[0], p[1], p[2], rgba);
        for (k = 0u; k < 3u && edge > 0.0f; ++k) canvas_line(c, lifted[k], lifted[(k + 1u) % 3u], edge, CANVAS_RGB(25, 28, 34));
    }
}
