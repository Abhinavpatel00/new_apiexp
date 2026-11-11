/* method.c — "Simplifying Textured Triangle Meshes in the Wild" (Hsueh-Ti Derek Liu,
 * Xiaoting Zhang, Cem Yuksel; ACM Trans. Graph. 44(6), 2025).
 *
 * Meshes made by hand are seldom one closed surface: parts touch without sharing vertices,
 * sheets have borders, three faces meet in one edge. Edge collapses by the quadric error of
 * Garland and Heckbert (1997) go wrong on them in two ways: a flat part costs nothing to
 * remove, and parts that only touch are never merged. The paper changes three things.
 *
 * 1. Virtual edges (section 4.1.1). Two triangles of different connected parts that are
 *    nearer than 2 r get an edge between their nearest vertices. Triangle distance, not
 *    vertex distance: a ball that rests in the middle of a large triangle is far from that
 *    triangle's corners.
 * 2. The cost of collapsing an edge ij to a point x (section 4.2, appendix A): the plane
 *    term of Garland and Heckbert, sum over the faces round i and j of area / 3 times the
 *    squared distance of x from the face's plane (eq. 9 - 13), kept and added up through
 *    the collapses ("memory"); plus the area term of Lindstrom and Turk (1998) for the
 *    border edges ab round i and j, the squared area of the triangle a b x (eq. 15), worked
 *    out anew from the mesh as it is ("memoryless"). On a closed surface there are no border
 *    edges and the cost is Garland and Heckbert's.
 * 3. Colour (section 4.3): no UV coordinates are carried along. A point of the simplified
 *    mesh is taken back through the collapses one by one; at each it is projected onto the
 *    faces that collapse touched, as they were before. wm_transfer does that.
 *
 * The mesh is lists (appendix C.1): for every vertex its faces and its edges. A collapse
 * moves one vertex's lists to the other and drops faces and edges that became degenerate or
 * double.
 *
 * This file's own choices: the point x solves (A + l I) x = -b + l m with m the edge's
 * middle and l = 1e-7 of A's trace, so that it is defined where A is singular (the paper
 * names the SVD or probabilistic quadrics for this); eq. 15 is used with the factor 1 / 2 as
 * printed (the squared area itself would be 1 / 4); nothing stops a collapse that turns a
 * face over.
 */
#include "method.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static void list_add(WmList *l, int value) {
    if (l->count == l->room) { l->room = l->room ? 2 * l->room : 8; l->item = realloc(l->item, (size_t)l->room * sizeof(int)); }
    l->item[l->count++] = value;
}

double wm_closest(const double p[3], const double a[3], const double b[3], const double c[3], double q[3]) {
    /* Ericson, Real-Time Collision Detection, 5.1.5: by the regions of the triangle */
    double ab[3], ac[3], ap[3], bp[3], cp[3], d1, d2, d3, d4, d5, d6, va, vb, vc, v = 0.0, w = 0.0;
    int    k;
    for (k = 0; k < 3; ++k) { ab[k] = b[k] - a[k]; ac[k] = c[k] - a[k]; ap[k] = p[k] - a[k]; bp[k] = p[k] - b[k]; cp[k] = p[k] - c[k]; }
    d1 = ab[0] * ap[0] + ab[1] * ap[1] + ab[2] * ap[2]; d2 = ac[0] * ap[0] + ac[1] * ap[1] + ac[2] * ap[2];
    d3 = ab[0] * bp[0] + ab[1] * bp[1] + ab[2] * bp[2]; d4 = ac[0] * bp[0] + ac[1] * bp[1] + ac[2] * bp[2];
    d5 = ab[0] * cp[0] + ab[1] * cp[1] + ab[2] * cp[2]; d6 = ac[0] * cp[0] + ac[1] * cp[1] + ac[2] * cp[2];
    vc = d1 * d4 - d3 * d2; vb = d5 * d2 - d1 * d6; va = d3 * d6 - d5 * d4;
    if (d1 <= 0.0 && d2 <= 0.0) { v = 0.0; w = 0.0; }
    else if (d3 >= 0.0 && d4 <= d3) { v = 1.0; w = 0.0; }
    else if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) { v = d1 / (d1 - d3); w = 0.0; }
    else if (d6 >= 0.0 && d5 <= d6) { v = 0.0; w = 1.0; }
    else if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) { v = 0.0; w = d2 / (d2 - d6); }
    else if (va <= 0.0 && d4 - d3 >= 0.0 && d5 - d6 >= 0.0) { w = (d4 - d3) / ((d4 - d3) + (d5 - d6)); v = 1.0 - w; }
    else { v = vb / (va + vb + vc); w = vc / (va + vb + vc); }
    for (k = 0; k < 3; ++k) q[k] = a[k] + ab[k] * v + ac[k] * w;
    return (p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]) + (p[2] - q[2]) * (p[2] - q[2]);
}

/* How many faces hold both ends of an edge: 0 virtual, 1 border, 2 inside a surface, more where sheets meet. */
static int edge_faces(const Wm *w, int a, int b) {
    const WmList *l = &w->vertex_faces[a];
    int           i, n = 0;
    for (i = 0; i < l->count; ++i) {
        const int *f = w->face + 3 * l->item[i];
        n += w->alive[l->item[i]] && (f[0] == b || f[1] == b || f[2] == b);
    }
    return n;
}

/* The quadric of collapsing edge e, the point that makes it least, and its value there. */
static double edge_cost(const Wm *w, int e, double x[3]) {
    const int    a = w->end[2 * e], b = w->end[2 * e + 1];
    const double *pa = w->x + 3 * a, *pb = w->x + 3 * b;
    double       q[10], m[6], r[3], l, det;
    int          k, side, i;

    for (k = 0; k < 10; ++k) q[k] = w->quadric[10 * a + k] + w->quadric[10 * b + k];
    if (w->area == 1)
        for (k = 0; k < 10; ++k) q[k] += w->area_quadric[10 * a + k] + w->area_quadric[10 * b + k];
    if (w->area == 2)
        for (side = 0; side < 2; ++side) {
            const int     v = side ? b : a;
            const WmList *edges = &w->vertex_edges[v];
            for (i = 0; i < edges->count; ++i) {
                const int g = edges->item[i], other = w->end[2 * g] == v ? w->end[2 * g + 1] : w->end[2 * g];
                if (!w->edge_alive[g] || (side == 1 && other == a) || edge_faces(w, v, other) != 1) continue;
                {
                    /* eq. 15, 16: |s cross x + t|^2 / 2 with s = v_b - v_a, t = v_a cross v_b */
                    const double *va = w->x + 3 * v, *vb = w->x + 3 * other;
                    const double s[3] = {vb[0] - va[0], vb[1] - va[1], vb[2] - va[2]};
                    const double t[3] = {va[1] * vb[2] - va[2] * vb[1], va[2] * vb[0] - va[0] * vb[2], va[0] * vb[1] - va[1] * vb[0]};
                    const double ss = s[0] * s[0] + s[1] * s[1] + s[2] * s[2];
                    q[0] += 0.5 * (ss - s[0] * s[0]); q[1] -= 0.5 * s[0] * s[1]; q[2] -= 0.5 * s[0] * s[2];
                    q[3] += 0.5 * (ss - s[1] * s[1]); q[4] -= 0.5 * s[1] * s[2]; q[5] += 0.5 * (ss - s[2] * s[2]);
                    q[6] += 0.5 * (t[1] * s[2] - t[2] * s[1]); q[7] += 0.5 * (t[2] * s[0] - t[0] * s[2]); q[8] += 0.5 * (t[0] * s[1] - t[1] * s[0]);
                    q[9] += 0.5 * (t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
                }
            }
        }

    /* A x = -b (eq. 14), pulled a little towards the middle of the edge so that A may be singular */
    l = 1e-7 * (q[0] + q[3] + q[5]) + 1e-30;
    m[0] = q[0] + l; m[1] = q[1]; m[2] = q[2]; m[3] = q[3] + l; m[4] = q[4]; m[5] = q[5] + l;
    for (k = 0; k < 3; ++k) r[k] = -q[6 + k] + l * 0.5 * (pa[k] + pb[k]);
    det = m[0] * (m[3] * m[5] - m[4] * m[4]) - m[1] * (m[1] * m[5] - m[4] * m[2]) + m[2] * (m[1] * m[4] - m[3] * m[2]);
    x[0] = ((m[3] * m[5] - m[4] * m[4]) * r[0] + (m[2] * m[4] - m[1] * m[5]) * r[1] + (m[1] * m[4] - m[2] * m[3]) * r[2]) / det;
    x[1] = ((m[2] * m[4] - m[1] * m[5]) * r[0] + (m[0] * m[5] - m[2] * m[2]) * r[1] + (m[1] * m[2] - m[0] * m[4]) * r[2]) / det;
    x[2] = ((m[1] * m[4] - m[2] * m[3]) * r[0] + (m[1] * m[2] - m[0] * m[4]) * r[1] + (m[0] * m[3] - m[1] * m[1]) * r[2]) / det;
    return x[0] * (q[0] * x[0] + 2.0 * q[1] * x[1] + 2.0 * q[2] * x[2]) + x[1] * (q[3] * x[1] + 2.0 * q[4] * x[2]) + q[5] * x[2] * x[2] +
           2.0 * (q[6] * x[0] + q[7] * x[1] + q[8] * x[2]) + q[9];
}

/* Works out an edge's cost and puts it in the queue (a heap, cheapest on top; equal costs by edge number). */
static void queue_edge(Wm *w, int e) {
    double x[3];
    const double cost = edge_cost(w, e, x);
    int    at;
    if (w->queued == w->queue_room) {
        w->queue_room *= 2;
        w->queue_cost = realloc(w->queue_cost, (size_t)w->queue_room * sizeof(double));
        w->queue_edge = realloc(w->queue_edge, (size_t)w->queue_room * sizeof(int));
        w->queue_stamp = realloc(w->queue_stamp, (size_t)w->queue_room * sizeof(int));
    }
    w->stamp[e] += 1;
    for (at = w->queued++; at > 0; at = (at - 1) / 2) {
        const int up = (at - 1) / 2;
        if (w->queue_cost[up] < cost || (w->queue_cost[up] == cost && w->queue_edge[up] < e)) break;
        w->queue_cost[at] = w->queue_cost[up]; w->queue_edge[at] = w->queue_edge[up]; w->queue_stamp[at] = w->queue_stamp[up];
    }
    w->queue_cost[at] = cost; w->queue_edge[at] = e; w->queue_stamp[at] = w->stamp[e];
}

Wm *wm_open(int vertices, const double *x, int faces, const int *face, int virtual_edges, int area, double reach) {
    Wm    *w = calloc(1, sizeof *w);
    int   *part = malloc((size_t)vertices * sizeof(int)), *order, f, v, i, j, k, e;
    double *low, *high;

    w->vertices = vertices; w->faces = w->faces_left = faces; w->area = area;
    w->x = malloc((size_t)vertices * 3 * sizeof(double)); memcpy(w->x, x, (size_t)vertices * 3 * sizeof(double));
    w->face = malloc((size_t)faces * 3 * sizeof(int)); memcpy(w->face, face, (size_t)faces * 3 * sizeof(int));
    w->alive = malloc((size_t)faces * sizeof(bool));
    w->quadric = calloc((size_t)vertices * 10, sizeof(double)); w->area_quadric = calloc((size_t)vertices * 10, sizeof(double));
    w->vertex_faces = calloc((size_t)vertices, sizeof(WmList)); w->vertex_edges = calloc((size_t)vertices, sizeof(WmList));
    w->moved_room = 1024; w->moved = malloc((size_t)w->moved_room * 3 * sizeof(double));

    /* faces: lists, plane quadrics (eq. 5, 8, 11: area / 3 of n n^T to each corner), parts, edges */
    for (v = 0; v < vertices; ++v) part[v] = v;
    {
        WmList ends = {0};
        for (f = 0; f < faces; ++f) {
            const int    *t = face + 3 * f;
            const double *a = x + 3 * t[0], *b = x + 3 * t[1], *c = x + 3 * t[2];
            double        n[3] = {(b[1] - a[1]) * (c[2] - a[2]) - (b[2] - a[2]) * (c[1] - a[1]), (b[2] - a[2]) * (c[0] - a[0]) - (b[0] - a[0]) * (c[2] - a[2]),
                                  (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])};
            const double  twice = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]), weight = twice / 6.0;
            double        d;
            w->alive[f] = true;
            for (k = 0; k < 3; ++k) n[k] /= twice > 0.0 ? twice : 1.0;
            d = -(n[0] * a[0] + n[1] * a[1] + n[2] * a[2]);
            for (k = 0; k < 3; ++k) {
                double *q = w->quadric + 10 * t[k];
                int     root = t[k], other = t[(k + 1) % 3];
                q[0] += weight * n[0] * n[0]; q[1] += weight * n[0] * n[1]; q[2] += weight * n[0] * n[2]; q[3] += weight * n[1] * n[1]; q[4] += weight * n[1] * n[2];
                q[5] += weight * n[2] * n[2]; q[6] += weight * n[0] * d; q[7] += weight * n[1] * d; q[8] += weight * n[2] * d; q[9] += weight * d * d;
                list_add(&w->vertex_faces[t[k]], f);
                while (part[root] != root) root = part[root];
                while (part[other] != other) other = part[other];
                part[root] = other;
                for (i = 0; i < w->vertex_edges[t[k]].count; ++i) {
                    const int g = w->vertex_edges[t[k]].item[i];
                    if (ends.item[2 * g] == t[(k + 1) % 3] || ends.item[2 * g + 1] == t[(k + 1) % 3]) break;
                }
                if (i == w->vertex_edges[t[k]].count) {
                    list_add(&w->vertex_edges[t[k]], ends.count / 2); list_add(&w->vertex_edges[t[(k + 1) % 3]], ends.count / 2);
                    list_add(&ends, t[k]); list_add(&ends, t[(k + 1) % 3]);
                }
            }
        }
        for (v = 0; v < vertices; ++v) {
            int root = v;
            while (part[root] != root) root = part[root];
            part[v] = root;
            w->parts += root == v && w->vertex_faces[v].count > 0;
        }

        /* virtual edges: sweep along x over the boxes of the triangles (or the vertices) */
        low = malloc((size_t)faces * 3 * sizeof(double)); high = malloc((size_t)faces * 3 * sizeof(double));
        order = malloc((size_t)(faces > vertices ? faces : vertices) * sizeof(int));
        if (virtual_edges == 1) {
            int gap;
            for (v = 0; v < vertices; ++v) order[v] = v;
            for (gap = vertices / 2; gap > 0; gap /= 2)   /* a shell sort by x */
                for (i = gap; i < vertices; ++i) {
                    const int held = order[i];
                    for (j = i; j >= gap && x[3 * order[j - gap]] > x[3 * held]; j -= gap) order[j] = order[j - gap];
                    order[j] = held;
                }
            for (i = 0; i < vertices; ++i)
                for (j = i + 1; j < vertices && x[3 * order[j]] - x[3 * order[i]] < 2.0 * reach; ++j) {
                    const int     a = order[i], b = order[j];
                    const double *p = x + 3 * a, *q = x + 3 * b;
                    if (part[a] == part[b] || !w->vertex_faces[a].count || !w->vertex_faces[b].count) continue;
                    if ((p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]) + (p[2] - q[2]) * (p[2] - q[2]) >= 4.0 * reach * reach) continue;
                    list_add(&w->vertex_edges[a], ends.count / 2); list_add(&w->vertex_edges[b], ends.count / 2);
                    list_add(&ends, a); list_add(&ends, b);
                    w->virtual_edges += 1;
                }
        }
        if (virtual_edges == 2) {
            int gap;
            for (f = 0; f < faces; ++f) {
                order[f] = f;
                for (k = 0; k < 3; ++k) {
                    low[3 * f + k] = fmin(x[3 * face[3 * f] + k], fmin(x[3 * face[3 * f + 1] + k], x[3 * face[3 * f + 2] + k]));
                    high[3 * f + k] = fmax(x[3 * face[3 * f] + k], fmax(x[3 * face[3 * f + 1] + k], x[3 * face[3 * f + 2] + k]));
                }
            }
            for (gap = faces / 2; gap > 0; gap /= 2)
                for (i = gap; i < faces; ++i) {
                    const int held = order[i];
                    for (j = i; j >= gap && low[3 * order[j - gap]] > low[3 * held]; j -= gap) order[j] = order[j - gap];
                    order[j] = held;
                }
            for (i = 0; i < faces; ++i)
                for (j = i + 1; j < faces && low[3 * order[j]] - high[3 * order[i]] < 2.0 * reach; ++j) {
                    const int     fa = order[i], fb = order[j];
                    const int    *ta = face + 3 * fa, *tb = face + 3 * fb;
                    const double *pa[3] = {x + 3 * ta[0], x + 3 * ta[1], x + 3 * ta[2]}, *pb[3] = {x + 3 * tb[0], x + 3 * tb[1], x + 3 * tb[2]};
                    double        nearest = 1e300, least = 1e300, q[3];
                    int           m, side;
                    if (part[ta[0]] == part[tb[0]]) continue;
                    if (low[3 * fb + 1] - high[3 * fa + 1] >= 2.0 * reach || low[3 * fa + 1] - high[3 * fb + 1] >= 2.0 * reach) continue;
                    if (low[3 * fb + 2] - high[3 * fa + 2] >= 2.0 * reach || low[3 * fa + 2] - high[3 * fb + 2] >= 2.0 * reach) continue;
                    /* distance between two triangles: a corner and a triangle, two sides, or 0 where a side goes through the other */
                    for (k = 0; k < 3; ++k) {
                        nearest = fmin(nearest, fmin(wm_closest(pa[k], pb[0], pb[1], pb[2], q), wm_closest(pb[k], pa[0], pa[1], pa[2], q)));
                        for (m = 0; m < 3; ++m) {
                            /* two segments (Ericson 5.1.9) */
                            const double *p1 = pa[k], *q1 = pa[(k + 1) % 3], *p2 = pb[m], *q2 = pb[(m + 1) % 3];
                            const double d1[3] = {q1[0] - p1[0], q1[1] - p1[1], q1[2] - p1[2]}, d2[3] = {q2[0] - p2[0], q2[1] - p2[1], q2[2] - p2[2]}, r[3] = {p1[0] - p2[0], p1[1] - p2[1], p1[2] - p2[2]};
                            const double aa = d1[0] * d1[0] + d1[1] * d1[1] + d1[2] * d1[2], ee = d2[0] * d2[0] + d2[1] * d2[1] + d2[2] * d2[2], ff = d2[0] * r[0] + d2[1] * r[1] + d2[2] * r[2];
                            const double cc = d1[0] * r[0] + d1[1] * r[1] + d1[2] * r[2], bb = d1[0] * d2[0] + d1[1] * d2[1] + d1[2] * d2[2], denom = aa * ee - bb * bb;
                            double       s = denom > 1e-300 ? fmin(fmax((bb * ff - cc * ee) / denom, 0.0), 1.0) : 0.0, t = ee > 1e-300 ? (bb * s + ff) / ee : 0.0, gapx, gapy, gapz;
                            if (t < 0.0) { t = 0.0; s = aa > 1e-300 ? fmin(fmax(-cc / aa, 0.0), 1.0) : 0.0; }
                            if (t > 1.0) { t = 1.0; s = aa > 1e-300 ? fmin(fmax((bb - cc) / aa, 0.0), 1.0) : 0.0; }
                            gapx = p1[0] + d1[0] * s - p2[0] - d2[0] * t; gapy = p1[1] + d1[1] * s - p2[1] - d2[1] * t; gapz = p1[2] + d1[2] * s - p2[2] - d2[2] * t;
                            nearest = fmin(nearest, gapx * gapx + gapy * gapy + gapz * gapz);
                        }
                        for (side = 0; side < 2; ++side) {
                            /* does side k of one triangle go through the other */
                            const double **s3 = side ? pb : pa, **t3 = side ? pa : pb;
                            const double *p = s3[k], *g = s3[(k + 1) % 3], *a = t3[0], *b = t3[1], *c = t3[2];
                            const double n[3] = {(b[1] - a[1]) * (c[2] - a[2]) - (b[2] - a[2]) * (c[1] - a[1]), (b[2] - a[2]) * (c[0] - a[0]) - (b[0] - a[0]) * (c[2] - a[2]),
                                                 (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])};
                            const double dp = n[0] * (p[0] - a[0]) + n[1] * (p[1] - a[1]) + n[2] * (p[2] - a[2]), dg = n[0] * (g[0] - a[0]) + n[1] * (g[1] - a[1]) + n[2] * (g[2] - a[2]);
                            if (dp * dg < 0.0) {
                                const double at = dp / (dp - dg), h[3] = {p[0] + (g[0] - p[0]) * at, p[1] + (g[1] - p[1]) * at, p[2] + (g[2] - p[2]) * at};
                                if (wm_closest(h, a, b, c, q) < 1e-14 * sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2])) nearest = 0.0;
                            }
                        }
                    }
                    if (nearest >= 4.0 * reach * reach) continue;
                    /* an edge between their nearest corners (all pairs that are equally near) */
                    for (k = 0; k < 3; ++k)
                        for (m = 0; m < 3; ++m)
                            least = fmin(least, (pa[k][0] - pb[m][0]) * (pa[k][0] - pb[m][0]) + (pa[k][1] - pb[m][1]) * (pa[k][1] - pb[m][1]) + (pa[k][2] - pb[m][2]) * (pa[k][2] - pb[m][2]));
                    for (k = 0; k < 3; ++k)
                        for (m = 0; m < 3; ++m) {
                            const int a = ta[k], b = tb[m];
                            if ((pa[k][0] - pb[m][0]) * (pa[k][0] - pb[m][0]) + (pa[k][1] - pb[m][1]) * (pa[k][1] - pb[m][1]) + (pa[k][2] - pb[m][2]) * (pa[k][2] - pb[m][2]) > least) continue;
                            for (e = 0; e < w->vertex_edges[a].count; ++e) {
                                const int g = w->vertex_edges[a].item[e];
                                if (ends.item[2 * g] == b || ends.item[2 * g + 1] == b) break;
                            }
                            if (e < w->vertex_edges[a].count) continue;
                            list_add(&w->vertex_edges[a], ends.count / 2); list_add(&w->vertex_edges[b], ends.count / 2);
                            list_add(&ends, a); list_add(&ends, b);
                            w->virtual_edges += 1;
                        }
                }
        }
        free(low); free(high); free(order);
        w->edges = ends.count / 2; w->end = ends.item;
    }

    /* parts once the virtual edges join them */
    for (e = 0; e < w->edges; ++e) {
        int a = w->end[2 * e], b = w->end[2 * e + 1];
        while (part[a] != a) a = part[a];
        while (part[b] != b) b = part[b];
        part[a] = b;
    }
    for (v = 0; v < vertices; ++v) w->joined += part[v] == v && w->vertex_faces[v].count > 0;
    free(part);

    /* the area term summed like the plane term (area 1): every border edge's quadric to both its ends */
    w->edge_alive = malloc((size_t)w->edges * sizeof(bool)); w->stamp = calloc((size_t)w->edges, sizeof(int));
    for (e = 0; e < w->edges; ++e) {
        const int     a = w->end[2 * e], b = w->end[2 * e + 1];
        const double *va = w->x + 3 * a, *vb = w->x + 3 * b;
        const double  s[3] = {vb[0] - va[0], vb[1] - va[1], vb[2] - va[2]}, t[3] = {va[1] * vb[2] - va[2] * vb[1], va[2] * vb[0] - va[0] * vb[2], va[0] * vb[1] - va[1] * vb[0]};
        const double  ss = s[0] * s[0] + s[1] * s[1] + s[2] * s[2];
        w->edge_alive[e] = true;
        if (edge_faces(w, a, b) != 1) continue;
        for (k = 0; k < 2; ++k) {
            double *q = w->area_quadric + 10 * (k ? b : a);
            q[0] += 0.5 * (ss - s[0] * s[0]); q[1] -= 0.5 * s[0] * s[1]; q[2] -= 0.5 * s[0] * s[2]; q[3] += 0.5 * (ss - s[1] * s[1]); q[4] -= 0.5 * s[1] * s[2]; q[5] += 0.5 * (ss - s[2] * s[2]);
            q[6] += 0.5 * (t[1] * s[2] - t[2] * s[1]); q[7] += 0.5 * (t[2] * s[0] - t[0] * s[2]); q[8] += 0.5 * (t[0] * s[1] - t[1] * s[0]);
            q[9] += 0.5 * (t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
        }
    }

    w->queue_room = 2 * w->edges + 16;
    w->queue_cost = malloc((size_t)w->queue_room * sizeof(double)); w->queue_edge = malloc((size_t)w->queue_room * sizeof(int)); w->queue_stamp = malloc((size_t)w->queue_room * sizeof(int));
    for (e = 0; e < w->edges; ++e) queue_edge(w, e);
    return w;
}

void wm_close(Wm *w) {
    int v;
    for (v = 0; v < w->vertices; ++v) { free(w->vertex_faces[v].item); free(w->vertex_edges[v].item); }
    free(w->x); free(w->face); free(w->alive); free(w->end); free(w->edge_alive); free(w->quadric); free(w->area_quadric); free(w->vertex_faces); free(w->vertex_edges);
    free(w->stamp); free(w->queue_cost); free(w->queue_edge); free(w->queue_stamp); free(w->ring.item); free(w->ring_first.item); free(w->kept.item); free(w->moved); free(w);
}

bool wm_collapse(Wm *w) {
    double  x[3];
    WmList *faces, *edges;
    int     e = -1, a, b, i, j, k;

    /* the cheapest edge whose queue entry still counts */
    while (w->queued > 0 && e < 0) {
        const int    top = w->queue_edge[0], top_stamp = w->queue_stamp[0], last = --w->queued;
        const double cost = w->queue_cost[last];
        int          at = 0;
        for (;;) {
            int down = 2 * at + 1;
            if (down >= last) break;
            if (down + 1 < last && (w->queue_cost[down + 1] < w->queue_cost[down] || (w->queue_cost[down + 1] == w->queue_cost[down] && w->queue_edge[down + 1] < w->queue_edge[down]))) down += 1;
            if (cost < w->queue_cost[down] || (cost == w->queue_cost[down] && w->queue_edge[last] < w->queue_edge[down])) break;
            w->queue_cost[at] = w->queue_cost[down]; w->queue_edge[at] = w->queue_edge[down]; w->queue_stamp[at] = w->queue_stamp[down];
            at = down;
        }
        w->queue_cost[at] = cost; w->queue_edge[at] = w->queue_edge[last]; w->queue_stamp[at] = w->queue_stamp[last];
        if (w->edge_alive[top] && w->stamp[top] == top_stamp) e = top;
    }
    if (e < 0) return false;
    a = w->end[2 * e]; b = w->end[2 * e + 1];
    edge_cost(w, e, x);

    /* remember the faces round the edge as they are, and where a was */
    if (w->collapses == w->moved_room) { w->moved_room *= 2; w->moved = realloc(w->moved, (size_t)w->moved_room * 3 * sizeof(double)); }
    list_add(&w->ring_first, w->ring.count); list_add(&w->kept, a);
    for (k = 0; k < 3; ++k) w->moved[3 * w->collapses + k] = w->x[3 * a + k];
    for (j = 0; j < 2; ++j) {
        const WmList *l = &w->vertex_faces[j ? b : a];
        for (i = 0; i < l->count; ++i) {
            const int f = l->item[i], *t = w->face + 3 * f;
            if (!w->alive[f] || (j == 1 && (t[0] == a || t[1] == a || t[2] == a))) continue;
            list_add(&w->ring, f); list_add(&w->ring, t[0]); list_add(&w->ring, t[1]); list_add(&w->ring, t[2]);
        }
    }
    w->collapses += 1;

    /* b goes into a */
    for (k = 0; k < 3; ++k) w->x[3 * a + k] = x[k];
    for (k = 0; k < 10; ++k) { w->quadric[10 * a + k] += w->quadric[10 * b + k]; w->area_quadric[10 * a + k] += w->area_quadric[10 * b + k]; }
    faces = &w->vertex_faces[a];
    for (i = 0; i < w->vertex_faces[b].count; ++i) {
        const int f = w->vertex_faces[b].item[i];
        int      *t = w->face + 3 * f;
        bool      gone;
        if (!w->alive[f]) continue;
        for (k = 0; k < 3; ++k) t[k] = t[k] == b ? a : t[k];
        gone = t[0] == t[1] || t[1] == t[2] || t[2] == t[0];
        for (j = 0; j < faces->count && !gone; ++j) {
            /* the same three vertices as a face a already has */
            const int *u = w->face + 3 * faces->item[j];
            gone = w->alive[faces->item[j]] && faces->item[j] != f && (t[0] == u[0] || t[0] == u[1] || t[0] == u[2]) && (t[1] == u[0] || t[1] == u[1] || t[1] == u[2]) &&
                   (t[2] == u[0] || t[2] == u[1] || t[2] == u[2]);
        }
        if (gone) { w->alive[f] = false; w->faces_left -= 1; }
        else list_add(faces, f);
    }
    edges = &w->vertex_edges[a];
    for (i = 0; i < w->vertex_edges[b].count; ++i) {
        const int g = w->vertex_edges[b].item[i], other = w->end[2 * g] == b ? w->end[2 * g + 1] : w->end[2 * g];
        bool      gone = other == a;
        if (!w->edge_alive[g]) continue;
        for (j = 0; j < edges->count && !gone; ++j) {
            const int h = edges->item[j];
            gone = w->edge_alive[h] && (w->end[2 * h] == other || w->end[2 * h + 1] == other);
        }
        if (gone) { w->edge_alive[g] = false; continue; }
        w->end[2 * g] = a; w->end[2 * g + 1] = other;
        list_add(edges, g);
    }

    /* new costs: the edges at a, and with the area term worked out anew also those at a's neighbours */
    for (i = 0; i < edges->count; ++i) {
        const int g = edges->item[i], other = w->end[2 * g] == a ? w->end[2 * g + 1] : w->end[2 * g];
        if (!w->edge_alive[g]) continue;
        queue_edge(w, g);
        if (w->area != 2) continue;
        for (j = 0; j < w->vertex_edges[other].count; ++j) {
            const int h = w->vertex_edges[other].item[j];
            if (w->edge_alive[h] && h != g) queue_edge(w, h);
        }
    }
    return true;
}

void wm_transfer(Wm *w, int count, const double *p, int *face, double *q) {
    int *mark = calloc((size_t)w->faces, sizeof(int)), c, i, s;
    for (c = w->collapses - 1; c >= 0; --c) {
        const int first = w->ring_first.item[c], end = c + 1 < w->collapses ? w->ring_first.item[c + 1] : w->ring.count;
        /* take the collapse back: the faces round the edge as they were (section 4.3.1: a vertex split) */
        for (i = first; i < end; i += 4) {
            const int f = w->ring.item[i];
            memcpy(w->face + 3 * f, w->ring.item + i + 1, 3 * sizeof(int));
            w->faces_left += !w->alive[f];
            w->alive[f] = true; mark[f] = c + 1;
        }
        memcpy(w->x + 3 * w->kept.item[c], w->moved + 3 * c, 3 * sizeof(double));
        /* a point on one of those faces goes to the nearest of them; the point itself stays where it is on the simplified mesh */
        for (s = 0; s < count; ++s) {
            double least = 1e300, unused[3];
            if (mark[face[s]] != c + 1) continue;
            for (i = first; i < end; i += 4) {
                const int   *t = w->face + 3 * w->ring.item[i];
                const double d = wm_closest(p + 3 * s, w->x + 3 * t[0], w->x + 3 * t[1], w->x + 3 * t[2], unused);
                if (d < least) { least = d; face[s] = w->ring.item[i]; }
            }
        }
    }
    for (s = 0; s < count; ++s) {
        const int *t = w->face + 3 * face[s];
        wm_closest(p + 3 * s, w->x + 3 * t[0], w->x + 3 * t[1], w->x + 3 * t[2], q + 3 * s);
    }
    w->collapses = 0; w->ring.count = 0; w->ring_first.count = 0; w->kept.count = 0;
    free(mark);
}
