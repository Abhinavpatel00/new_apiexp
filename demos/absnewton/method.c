/* method.c — Chen, Liu, Levin, Zheng, Jacobson: "Stabler Neo-Hookean Simulation: Absolute
 * Eigenvalue Filtering for Projected Newton" (SIGGRAPH 2024).
 *
 * Quasi-static: find the node positions of least elastic energy (eq. 1) by Newton's method
 * (eq. 2, 3). The Hessian is a sum over tetrahedra (eq. 4); each tetrahedron's 12 x 12 part is
 * made positive semi-definite on its own before it is added (eq. 6), either by clamping its
 * negative eigenvalues (eq. 5) or by taking their absolute values (eq. 10, the paper's change).
 *
 * The paper uses a sparse Cholesky solver and automatic differentiation. Here the matrix is
 * dense, which limits the mesh to a few hundred nodes, and the derivatives are written out.
 */
#include "method.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

void nh_eigen(double *a, int n, double *values, double *vectors) {
    int sweep, p, q, k;
    for (p = 0; p < n; ++p)
        for (q = 0; q < n; ++q) vectors[p * n + q] = p == q;
    for (sweep = 0; sweep < 60; ++sweep) {
        double off = 0.0, all = 0.0;
        for (p = 0; p < n; ++p)
            for (q = 0; q < n; ++q) { all += a[p * n + q] * a[p * n + q]; off += p == q ? 0.0 : a[p * n + q] * a[p * n + q]; }
        if (off <= 1e-30 * all) break;
        for (p = 0; p < n; ++p)
            for (q = p + 1; q < n; ++q) {
                const double apq = a[p * n + q];
                double       theta, t, c, s;
                if (fabs(apq) < 1e-300) continue;
                theta = (a[q * n + q] - a[p * n + p]) / (2.0 * apq);
                t     = (theta < 0.0 ? -1.0 : 1.0) / (fabs(theta) + sqrt(theta * theta + 1.0));
                c     = 1.0 / sqrt(t * t + 1.0);
                s     = t * c;
                for (k = 0; k < n; ++k) {
                    const double akp = a[k * n + p], akq = a[k * n + q];
                    a[k * n + p] = c * akp - s * akq; a[k * n + q] = s * akp + c * akq;
                }
                for (k = 0; k < n; ++k) {
                    const double apk = a[p * n + k], aqk = a[q * n + k], vkp = vectors[k * n + p], vkq = vectors[k * n + q];
                    a[p * n + k] = c * apk - s * aqk; a[q * n + k] = s * apk + c * aqk;
                    vectors[k * n + p] = c * vkp - s * vkq; vectors[k * n + q] = s * vkp + c * vkq;
                }
            }
    }
    for (p = 0; p < n; ++p) values[p] = a[p * n + p];
}

void nh_filter(double *h, int n, NhFilter filter, double epsilon) {
    double copy[144], values[12], vectors[144];
    int    r, c, k;
    if (filter == NH_NONE) return;
    memcpy(copy, h, (size_t)(n * n) * sizeof(double));
    nh_eigen(copy, n, values, vectors);
    for (k = 0; k < n; ++k) values[k] = filter == NH_ABS ? fabs(values[k]) : fmax(values[k], epsilon);
    for (r = 0; r < n; ++r)
        for (c = 0; c < n; ++c) {
            double sum = 0.0;
            for (k = 0; k < n; ++k) sum += vectors[r * n + k] * values[k] * vectors[c * n + k];
            h[r * n + c] = sum;
        }
}

/* dJ/dF: the cofactors. */
static double cofactors(const double f[3][3], double cof[3][3]) {
    int a, b;
    for (a = 0; a < 3; ++a)
        for (b = 0; b < 3; ++b) {
            const int a1 = (a + 1) % 3, a2 = (a + 2) % 3, b1 = (b + 1) % 3, b2 = (b + 2) % 3;
            cof[a][b] = f[a1][b1] * f[a2][b2] - f[a1][b2] * f[a2][b1];
        }
    return f[0][0] * cof[0][0] + f[0][1] * cof[0][1] + f[0][2] * cof[0][2];
}

void nh_hessian_f(double mu, double lambda, const double f[3][3], double h[9][9]) {
    double       cof[3][3];
    const double j = cofactors(f, cof), alpha = 1.0 + mu / lambda;
    int          a, b, c, d;
    /* Psi = mu / 2 (|F|^2 - 3) + lambda / 2 (J - alpha)^2, so
     * d2 Psi = mu I + lambda dJ dJ^T + lambda (J - alpha) d2J, and J is linear in each entry of F:
     * d2J / dF_ab dF_cd = +-F_ef with e, f the remaining row and column. */
    for (a = 0; a < 3; ++a)
        for (b = 0; b < 3; ++b)
            for (c = 0; c < 3; ++c)
                for (d = 0; d < 3; ++d) {
                    double v = lambda * cof[a][b] * cof[c][d] + (a == c && b == d ? mu : 0.0);
                    if (a != c && b != d) v += lambda * (j - alpha) * ((c - a + 3) % 3 == (d - b + 3) % 3 ? 1.0 : -1.0) * f[3 - a - c][3 - b - d];
                    h[3 * a + b][3 * c + d] = v;
                }
}

void nh_mesh(Nh *s, uint32_t nodes, const double *rest, uint32_t tets, const uint32_t *tet) {
    const size_t n = (size_t)nodes * 3u;
    uint32_t     t;
    int          i, r, c;
    free(s->x); free(s->held); free(s->tet); free(s->gradient); free(s->direction); free(s->trial); free(s->hessian);
    s->nodes     = nodes;
    s->tets      = tets;
    s->x         = (double *)malloc(n * sizeof(double));
    s->held      = (uint8_t *)calloc(n, 1u);
    s->tet       = (NhTet *)calloc(tets, sizeof(NhTet));
    s->gradient  = (double *)malloc(n * sizeof(double));
    s->direction = (double *)malloc(n * sizeof(double));
    s->trial     = (double *)malloc(n * sizeof(double));
    s->hessian   = (double *)malloc(n * n * sizeof(double));
    memcpy(s->x, rest, n * sizeof(double));
    for (t = 0u; t < tets; ++t) {
        NhTet        *e = &s->tet[t];
        const double *p[4];
        double        m[3][3], cof[3][3], det;
        for (i = 0; i < 4; ++i) { e->node[i] = tet[4u * t + (uint32_t)i]; p[i] = &rest[3u * e->node[i]]; }
        /* columns of m are the edges from node 0; the rows of its inverse are the gradients of nodes 1..3 */
        for (r = 0; r < 3; ++r)
            for (c = 0; c < 3; ++c) m[r][c] = p[c + 1][r] - p[0][r];
        det = cofactors(m, cof);
        for (r = 0; r < 3; ++r)
            for (c = 0; c < 3; ++c) e->grad[r + 1][c] = cof[c][r] / det;
        for (c = 0; c < 3; ++c) e->grad[0][c] = -e->grad[1][c] - e->grad[2][c] - e->grad[3][c];
        e->volume = det / 6.0;
    }
}

static void deformation(const NhTet *e, const double *x, double f[3][3]) {
    int i, a, b;
    memset(f, 0, sizeof(double[3][3]));
    for (i = 0; i < 4; ++i)
        for (a = 0; a < 3; ++a)
            for (b = 0; b < 3; ++b) f[a][b] += x[3u * e->node[i] + (uint32_t)a] * e->grad[i][b];
}

double nh_energy(const Nh *s, const double *x, double *gradient) {
    const double alpha = 1.0 + s->mu / s->lambda;
    double       energy = 0.0;
    uint32_t     t, k;
    int          i, a, b;
    if (gradient) memset(gradient, 0, (size_t)s->nodes * 3u * sizeof(double));
    for (t = 0u; t < s->tets; ++t) {
        const NhTet *e = &s->tet[t];
        double       f[3][3], cof[3][3], j, norm = 0.0;
        deformation(e, x, f);
        j = cofactors(f, cof);
        for (a = 0; a < 3; ++a)
            for (b = 0; b < 3; ++b) norm += f[a][b] * f[a][b];
        energy += e->volume * (0.5 * s->mu * (norm - 3.0) + 0.5 * s->lambda * (j - alpha) * (j - alpha));
        for (i = 0; i < 4 && gradient; ++i)
            for (a = 0; a < 3; ++a)
                for (b = 0; b < 3; ++b)
                    gradient[3u * e->node[i] + (uint32_t)a] += e->volume * (s->mu * f[a][b] + s->lambda * (j - alpha) * cof[a][b]) * e->grad[i][b];
    }
    for (k = 0u; k < s->nodes * 3u && gradient; ++k)
        if (s->held[k]) gradient[k] = 0.0;
    return energy;
}

double nh_newton(Nh *s) {
    const size_t n = (size_t)s->nodes * 3u;
    double      *h = s->hessian, *g = s->gradient, *d = s->direction;
    double       slope = 0.0, largest = 0.0;
    size_t       r, c, k;
    uint32_t     t;
    int          i, j, a, b, cc, dd;

    s->energy = nh_energy(s, s->x, g);
    memset(h, 0, n * n * sizeof(double));
    for (t = 0u; t < s->tets; ++t) {
        const NhTet *e = &s->tet[t];
        double       f[3][3], hf[9][9], local[144];
        deformation(e, s->x, f);
        nh_hessian_f(s->mu, s->lambda, f, hf);
        /* dF_ab / dx_ia = grad_i[b] */
        for (i = 0; i < 4; ++i)
            for (a = 0; a < 3; ++a)
                for (j = 0; j < 4; ++j)
                    for (cc = 0; cc < 3; ++cc) {
                        double sum = 0.0;
                        for (b = 0; b < 3; ++b)
                            for (dd = 0; dd < 3; ++dd) sum += e->grad[i][b] * e->grad[j][dd] * hf[3 * a + b][3 * cc + dd];
                        local[(3 * i + a) * 12 + 3 * j + cc] = e->volume * sum;
                    }
        nh_filter(local, 12, s->filter, s->epsilon);
        for (i = 0; i < 4; ++i)
            for (a = 0; a < 3; ++a)
                for (j = 0; j < 4; ++j)
                    for (cc = 0; cc < 3; ++cc) h[(3u * e->node[i] + (size_t)a) * n + 3u * e->node[j] + (size_t)cc] += local[(3 * i + a) * 12 + 3 * j + cc];
    }
    for (k = 0u; k < n; ++k) largest = fmax(largest, h[k * n + k]);
    for (k = 0u; k < n; ++k) {
        if (!s->held[k]) continue;
        for (c = 0u; c < n; ++c) h[k * n + c] = h[c * n + k] = 0.0;
        h[k * n + k] = largest;
    }

    /* H = L L^T in the lower triangle. A pivot that is not positive means H is not positive
     * definite: with clamping at 0 it can be singular, and unfiltered it can be anything. It is
     * replaced by a small positive number so that a direction still comes out, and counted. */
    s->replaced = 0u;
    for (c = 0u; c < n; ++c) {
        double pivot = h[c * n + c];
        for (k = 0u; k < c; ++k) pivot -= h[c * n + k] * h[c * n + k];
        if (pivot <= 1e-12 * largest) { pivot = 1e-12 * largest; s->replaced++; }
        pivot = sqrt(pivot);
        h[c * n + c] = pivot;
        for (r = c + 1u; r < n; ++r) {
            double sum = h[r * n + c];
            for (k = 0u; k < c; ++k) sum -= h[r * n + k] * h[c * n + k];
            h[r * n + c] = sum / pivot;
        }
    }
    for (r = 0u; r < n; ++r) {
        double sum = -g[r];
        for (k = 0u; k < r; ++k) sum -= h[r * n + k] * d[k];
        d[r] = sum / h[r * n + r];
    }
    for (r = n; r-- > 0u;) {
        double sum = d[r];
        for (k = r + 1u; k < n; ++k) sum -= h[k * n + r] * d[k];
        d[r] = sum / h[r * n + r];
    }
    for (k = 0u; k < n; ++k) slope += d[k] * g[k];

    /* Backtracking: halve the step until the energy has fallen by a ten-thousandth of what the
     * slope promises (Nocedal and Wright's sufficient decrease). No test for inverted elements:
     * the paper leaves it out on purpose (section 7). */
    for (s->step = 1.0; s->step > 1e-12; s->step *= 0.5) {
        for (k = 0u; k < n; ++k) s->trial[k] = s->x[k] + s->step * d[k];
        if (nh_energy(s, s->trial, NULL) <= s->energy + 1e-4 * s->step * slope) break;
    }
    memcpy(s->x, s->trial, n * sizeof(double));
    return -0.5 * slope;
}
