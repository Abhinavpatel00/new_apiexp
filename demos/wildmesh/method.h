/* method.h — simplifying a triangle mesh that need not be one clean surface
 * (Liu, Zhang, Yuksel 2025). See method.c. */
#ifndef WILDMESH_METHOD_H
#define WILDMESH_METHOD_H
#include <stdbool.h>

typedef struct WmList {
    int *item, count, room;
} WmList;

typedef struct Wm {
    /* the mesh as it is now: faces keep their number from the input to the end */
    int     vertices, faces, faces_left;
    double *x;       /* 3 a vertex */
    int    *face;    /* 3 a face */
    bool   *alive;   /* a face */
    /* edges: those of the faces, and virtual ones between parts that do not share a vertex */
    int     edges, virtual_edges, parts, joined;   /* parts: connected pieces of the input; joined: with the virtual edges */
    int    *end;          /* 2 an edge */
    bool   *edge_alive;
    /* the rest is the method's own */
    int     area;         /* 0 no area term, 1 summed like the plane term, 2 worked out anew for every cost */
    double *quadric, *area_quadric;   /* 10 a vertex: A (xx xy xz yy yz zz), b, c */
    WmList *vertex_faces, *vertex_edges;
    int    *stamp;        /* an edge: which of its entries in the queue counts */
    double *queue_cost;
    int    *queue_edge, *queue_stamp, queued, queue_room;
    /* what every collapse changed, for wm_transfer */
    int     collapses;
    WmList  ring, ring_first, kept;   /* ring: face, its three vertices before; ring_first: where a collapse's entries start */
    double *moved;        /* 3 a collapse: where the kept vertex was */
    int     moved_room;
} Wm;

/* Takes a copy of the mesh. virtual_edges: 0 none, 1 between vertices of different parts
 * nearer than 2 * reach, 2 between the nearest vertices of triangles of different parts
 * nearer than 2 * reach (the paper's). area: see above (2 is the paper's). */
Wm *wm_open(int vertices, const double *x, int faces, const int *face, int virtual_edges, int area, double reach);
void wm_close(Wm *w);

/* Collapses the cheapest edge. False when there is none left. */
bool wm_collapse(Wm *w);

/* Where `count` points of the mesh as it is now come from on the input. face[i] is the face
 * point i lies on; on return it is a face of the input and q (3 a point) the place on it.
 * Takes back every collapse on the way: afterwards the mesh is the input again and the
 * simplifier must not be used further. */
void wm_transfer(Wm *w, int count, const double *p, int *face, double *q);

/* The point of triangle abc nearest to p, into q; returns the squared distance. */
double wm_closest(const double p[3], const double a[3], const double b[3], const double c[3], double q[3]);

#endif /* WILDMESH_METHOD_H */
