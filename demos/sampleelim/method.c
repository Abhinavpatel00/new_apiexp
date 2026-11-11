/* method.c — Cem Yuksel: "Sample Elimination for Generating Poisson Disk Sample Sets"
 * (Eurographics 2015).
 *
 * From M input samples keep N so that no two are close: give every sample the weight
 * w_i = sum_j (1 - d^ij / (2 r_max))^alpha over the samples within 2 r_max (eq. 1), then
 * repeatedly remove the sample with the largest weight and take its share off the weights of
 * its neighbours (figure 2). r_max is the spacing of a hexagonal packing of N samples (eq. 2);
 * distances below 2 r_min count as 2 r_min (eq. 4, 5: "weight limiting").
 *
 * The paper's implementation finds neighbours with a kd-tree; this one uses a grid of cells
 * 2 r_max wide. The heap is the paper's.
 */
#include "method.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.14159265358979323846

double se_random(uint64_t *state) {
    *state ^= *state << 13; *state ^= *state >> 7; *state ^= *state << 17;   /* xorshift64 */
    return (double)(*state >> 11) / 9007199254740992.0;
}

double se_rmax(const Se *s, uint32_t n) { return sqrt(s->size[0] * s->size[1] / (2.0 * sqrt(3.0) * n)); }

static double distance(const Se *s, const double *a, const double *b) {
    double d[2] = {a[0] - b[0], a[1] - b[1]};
    if (s->tiled) { d[0] -= s->size[0] * floor(d[0] / s->size[0] + 0.5); d[1] -= s->size[1] * floor(d[1] / s->size[1] + 0.5); }
    return sqrt(d[0] * d[0] + d[1] * d[1]);
}

/* What the elimination works on: local sample i is p[2 index[i]]. */
typedef struct Work {
    const Se       *s;
    const double   *p, *intensity;
    const uint32_t *index;
    double          rmax, rmin, *w;
    uint32_t       *heap, *where;        /* heap of samples by weight, largest first; where[i]: i's place in it */
    uint32_t       *start, *member;      /* grid: the samples of cell c are member[start[c] .. start[c + 1]) */
    uint32_t        cells[2], have, left;
} Work;

/* eq. 1 with eq. 4: what sample j adds to the weight of sample i */
static double share(const Work *k, uint32_t i, uint32_t j) {
    double d = distance(k->s, k->p + 2u * k->index[i], k->p + 2u * k->index[j]);
    if (k->intensity) d *= 3.0 - 2.0 * k->intensity[k->index[i]];
    d = fmin(fmax(d, 2.0 * k->rmin), 2.0 * k->rmax);
    return pow(1.0 - d / (2.0 * k->rmax), k->s->alpha);
}

static void sift_down(Work *k, uint32_t at) {
    const uint32_t moving = k->heap[at];
    for (;;) {
        uint32_t child = 2u * at + 1u;
        if (child >= k->left) break;
        if (child + 1u < k->left && k->w[k->heap[child + 1u]] > k->w[k->heap[child]]) child++;
        if (k->w[k->heap[child]] <= k->w[moving]) break;
        k->heap[at] = k->heap[child]; k->where[k->heap[at]] = at;
        at = child;
    }
    k->heap[at] = moving; k->where[moving] = at;
}

/* Calls share-taking for every sample within reach of sample j: adds (sign +1) the shares of the
 * neighbours to w[j], or takes (sign -1) j's share off each neighbour and restores the heap. */
static void visit(Work *k, uint32_t j, int sign) {
    const double *pj = k->p + 2u * k->index[j];
    const int     cx = (int)(pj[0] / k->s->size[0] * k->cells[0]), cy = (int)(pj[1] / k->s->size[1] * k->cells[1]);
    /* with fewer than three cells across, every cell is a neighbour once */
    const int     x0 = k->cells[0] < 3u ? -cx : -1, x1 = k->cells[0] < 3u ? (int)k->cells[0] - 1 - cx : 1;
    const int     y0 = k->cells[1] < 3u ? -cy : -1, y1 = k->cells[1] < 3u ? (int)k->cells[1] - 1 - cy : 1;
    int           dx, dy;
    uint32_t      at;
    for (dy = y0; dy <= y1; ++dy)
        for (dx = x0; dx <= x1; ++dx) {
            int nx = cx + dx, ny = cy + dy;
            if (k->s->tiled) { nx = (nx + (int)k->cells[0]) % (int)k->cells[0]; ny = (ny + (int)k->cells[1]) % (int)k->cells[1]; }
            if (nx < 0 || ny < 0 || nx >= (int)k->cells[0] || ny >= (int)k->cells[1]) continue;
            for (at = k->start[(uint32_t)ny * k->cells[0] + (uint32_t)nx]; at < k->start[(uint32_t)ny * k->cells[0] + (uint32_t)nx + 1u]; ++at) {
                const uint32_t i = k->member[at];
                if (i == j || k->where[i] >= k->left) continue;   /* itself, or eliminated */
                if (sign > 0) { k->w[j] += share(k, j, i); continue; }
                k->w[i] -= share(k, i, j);
                sift_down(k, k->where[i]);
            }
        }
}

void se_eliminate(const Se *s, const double *p, uint32_t *index, uint32_t have, uint32_t want, const double *intensity, double *weight) {
    Work      k = {.s = s, .p = p, .intensity = intensity, .index = index, .have = have, .left = have};
    uint32_t *result = (uint32_t *)malloc(have * sizeof(uint32_t));
    uint32_t  i, c, kept;
    k.rmax = se_rmax(s, want) * (intensity ? 3.0 : 1.0);
    k.rmin = k.rmax * (1.0 - pow((double)want / have, s->gamma)) * s->beta;                       /* eq. 5 */
    k.cells[0] = (uint32_t)fmax(1.0, floor(s->size[0] / (2.0 * k.rmax)));
    k.cells[1] = (uint32_t)fmax(1.0, floor(s->size[1] / (2.0 * k.rmax)));
    k.w      = (double *)calloc(have, sizeof(double));
    k.heap   = (uint32_t *)malloc(have * sizeof(uint32_t));
    k.where  = (uint32_t *)malloc(have * sizeof(uint32_t));
    k.start  = (uint32_t *)calloc((size_t)k.cells[0] * k.cells[1] + 2u, sizeof(uint32_t));
    k.member = (uint32_t *)malloc(have * sizeof(uint32_t));
    /* counting sort into cells: start[c + 2] counts, then start[c + 1] is filled up to the end of c */
    for (i = 0u; i < have; ++i) {
        c = (uint32_t)(p[2u * index[i] + 1u] / s->size[1] * k.cells[1]) * k.cells[0] + (uint32_t)(p[2u * index[i]] / s->size[0] * k.cells[0]);
        k.start[c + 2u]++;
    }
    for (c = 0u; c < k.cells[0] * k.cells[1]; ++c) k.start[c + 2u] += k.start[c + 1u];
    for (i = 0u; i < have; ++i) {
        c = (uint32_t)(p[2u * index[i] + 1u] / s->size[1] * k.cells[1]) * k.cells[0] + (uint32_t)(p[2u * index[i]] / s->size[0] * k.cells[0]);
        k.member[k.start[c + 1u]++] = i;
    }
    for (i = 0u; i < have; ++i) { k.heap[i] = i; k.where[i] = i; }
    for (i = 0u; i < have; ++i) visit(&k, i, +1);
    for (i = have / 2u; i-- > 0u;) sift_down(&k, i);

    while (k.left > want) {
        const uint32_t top = k.heap[0];
        /* the top leaves the heap; its place past the end records the order of elimination */
        k.left--;
        k.heap[0] = k.heap[k.left]; k.where[k.heap[0]] = 0u;
        k.heap[k.left] = top; k.where[top] = k.left;
        if (k.left) sift_down(&k, 0u);
        visit(&k, top, -1);
    }
    /* kept samples in their old order, then heap[want ..]: the last one eliminated is at `want` */
    for (kept = 0u, i = 0u; i < have; ++i)
        if (k.where[i] < want) {
            if (weight) weight[kept] = k.w[i];
            result[kept++] = index[i];
        }
    for (i = want; i < have; ++i) result[i] = index[k.heap[i]];
    memcpy(index, result, have * sizeof(uint32_t));
    free(result); free(k.w); free(k.heap); free(k.where); free(k.start); free(k.member);
}

void se_order(const Se *s, const double *p, uint32_t *index, uint32_t have, uint32_t want) {
    uint32_t n;
    se_eliminate(s, p, index, have, want, 0, 0);
    for (n = want; n > 1u; n /= 2u) se_eliminate(s, p, index, n, n / 2u, 0, 0);
}

double se_radius(const Se *s, const double *p, const uint32_t *index, uint32_t n, double *mean) {
    double   least = 1e300;
    uint32_t i, j;
    *mean = 0.0;
    for (i = 0u; i < n; ++i) {
        double nearest = 1e300;
        for (j = 0u; j < n; ++j)
            if (j != i) nearest = fmin(nearest, distance(s, p + 2u * index[i], p + 2u * index[j]));
        least  = fmin(least, 0.5 * nearest);
        *mean += 0.5 * nearest / n;
    }
    return least;
}

uint32_t se_darts(const Se *s, double radius, uint32_t want, uint64_t tries, uint64_t *state, double *p) {
    /* cells 2 radius wide or wider: a dart can only be stopped from the 3 x 3 cells round it */
    const uint32_t nx = (uint32_t)fmax(3.0, floor(s->size[0] / (2.0 * radius))), ny = (uint32_t)fmax(3.0, floor(s->size[1] / (2.0 * radius)));
    uint32_t      *cell = (uint32_t *)calloc((size_t)nx * ny * 8u, sizeof(uint32_t)), *fill = (uint32_t *)calloc((size_t)nx * ny, sizeof(uint32_t));
    uint32_t       count = 0u, at;
    uint64_t       t;
    int            dx, dy;
    for (t = 0u; t < tries && count < want; ++t) {
        const double x[2] = {se_random(state) * s->size[0], se_random(state) * s->size[1]};
        const int    cx = (int)(x[0] / s->size[0] * nx), cy = (int)(x[1] / s->size[1] * ny);
        bool         free_spot = true;
        for (dy = -1; dy <= 1; ++dy)
            for (dx = -1; dx <= 1; ++dx) {
                int qx = cx + dx, qy = cy + dy;
                if (s->tiled) { qx = (qx + (int)nx) % (int)nx; qy = (qy + (int)ny) % (int)ny; }
                if (qx < 0 || qy < 0 || qx >= (int)nx || qy >= (int)ny) continue;
                for (at = 0u; at < fill[(uint32_t)qy * nx + (uint32_t)qx]; ++at)
                    free_spot = free_spot && distance(s, x, p + 2u * cell[((uint32_t)qy * nx + (uint32_t)qx) * 8u + at]) >= 2.0 * radius;
            }
        if (!free_spot || fill[(uint32_t)cy * nx + (uint32_t)cx] == 8u) continue;
        p[2u * count] = x[0]; p[2u * count + 1u] = x[1];
        cell[((uint32_t)cy * nx + (uint32_t)cx) * 8u + fill[(uint32_t)cy * nx + (uint32_t)cx]++] = count++;
    }
    free(cell); free(fill);
    return count;
}

void se_spectrum(const double *p, const uint32_t *index, uint32_t n, int reach, double *power) {
    const int side = 2 * reach + 1;
    int       kx, ky;
    uint32_t  j;
    for (ky = -reach; ky <= reach; ++ky)
        for (kx = -reach; kx <= reach; ++kx) {
            double re = 0.0, im = 0.0;
            for (j = 0u; j < n; ++j) {
                const double phase = -2.0 * PI * (kx * p[2u * index[j]] + ky * p[2u * index[j] + 1u]);
                re += cos(phase); im += sin(phase);
            }
            power[(ky + reach) * side + (kx + reach)] = (re * re + im * im) / n;
        }
}
