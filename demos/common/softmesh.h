/* softmesh.h — a box, a shape of cubes or a ring of tetrahedra and its surface, for the demos that deform one. */
#ifndef MU_SOFTMESH_H
#define MU_SOFTMESH_H
#include "canvas.h"

typedef struct SoftMesh {
    uint32_t  nodes, tets, faces;
    double   *rest;   /* 3 a node                                               */
    uint32_t *tet;    /* 4 a tetrahedron; (b - a) x (c - a) . (d - a) > 0       */
    uint32_t *face;   /* 3 a surface triangle, seen anticlockwise from outside  */
} SoftMesh;

/* nx x ny x nz cubes of side `cell` with a corner at the origin, six tetrahedra a cube (all sharing
 * the cube's diagonal, so neighbours fit). Only the cubes for which solid[x + nx * (y + ny * z)] is
 * not 0 are made (NULL: all). Lattice points that no cube uses are left out; the others keep their
 * order (x fastest, z slowest). */
void softmesh_cells(SoftMesh *m, uint32_t nx, uint32_t ny, uint32_t nz, double cell, const uint8_t *solid);

/* A ring standing on the ground in the x-y plane: `around` cells round it (their side is
 * `cell` only if the caller picks `inner` to suit), `thick` outwards from the radius
 * `inner`, `wide` along z from 0. Node (i, j, k) is i + j * around + k * around * (thick + 1);
 * i runs clockwise seen from +z, starting at the lowest point. */
void softmesh_ring(SoftMesh *m, uint32_t around, uint32_t thick, uint32_t wide, double inner, double cell);

/* The surface at node positions `x` (3 a node), lit; `edge` > 0 also draws
 * the triangle edges that wide. */
void softmesh_draw(const SoftMesh *m, const double *x, Canvas *c, uint32_t rgba, float edge);
#endif /* MU_SOFTMESH_H */
