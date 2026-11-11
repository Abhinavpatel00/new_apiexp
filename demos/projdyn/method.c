/* method.c — Projective Dynamics.
 *
 * Bouaziz, Martin, Liu, Kavan, Pauly: "Projective Dynamics: Fusing Constraint
 * Projections for Fast Simulation", SIGGRAPH 2014.
 *
 * Implicit Euler is the minimum over q of
 *
 *     |M^(1/2) (q - s_n)|^2 / (2 h^2)  +  sum_i W_i(q)          s_n = q_n + h v_n + h^2 M^-1 f_ext
 *
 * The paper restricts the elastic energies to "distance to a constraint set":
 *
 *     W_i(q) = min over p in C_i of  w_i / 2 |A_i q - p|^2
 *
 * and minimises over q and all p_i in turn:
 *   local   p_i = the point of C_i nearest to A_i q        (each constraint alone)
 *   global  (M / h^2 + sum w_i A_i^T A_i) q = M s_n / h^2 + sum w_i A_i^T p_i
 * The matrix of the global step never changes, so it is factored once. Neither
 * step can raise the objective, so it falls in every iteration whatever h is.
 *
 * Here A_i q is the deformation gradient F of a tetrahedron. Its constraint
 * sets are "singular values within [low, high]" (strain; with low = high = 1
 * the rotations, which is corotated elasticity) and "determinant within
 * [low, high]" (volume). An attachment is A = 1 on one node, C = one point.
 */
#include "method.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

void pd_svd(const double f[3][3], double u[3][3], double sigma[3], double v[3][3]) {
    double a[3][3], lambda[3];
    int    i, j, k, sweep, order[3] = {0, 1, 2};
    /* Eigenvectors of F^T F by Jacobi rotations: they are V, and the eigenvalues are sigma^2. */
    for (i = 0; i < 3; ++i)
        for (j = 0; j < 3; ++j) {
            a[i][j] = f[0][i] * f[0][j] + f[1][i] * f[1][j] + f[2][i] * f[2][j];
            v[i][j] = i == j ? 1.0 : 0.0;
        }
    for (sweep = 0; sweep < 12; ++sweep)
        for (i = 0; i < 2; ++i)
            for (j = i + 1; j < 3; ++j) {
                const double theta = 0.5 * atan2(2.0 * a[i][j], a[i][i] - a[j][j]), c = cos(theta), s = sin(theta);
                if (fabs(a[i][j]) < 1e-300) continue;
                for (k = 0; k < 3; ++k) { const double x = a[k][i], y = a[k][j]; a[k][i] = c * x + s * y; a[k][j] = -s * x + c * y; }
                for (k = 0; k < 3; ++k) { const double x = a[i][k], y = a[j][k]; a[i][k] = c * x + s * y; a[j][k] = -s * x + c * y; }
                for (k = 0; k < 3; ++k) { const double x = v[k][i], y = v[k][j]; v[k][i] = c * x + s * y; v[k][j] = -s * x + c * y; }
            }
    for (i = 0; i < 3; ++i) lambda[i] = a[i][i];
    for (i = 0; i < 2; ++i)
        for (j = i + 1; j < 3; ++j)
            if (lambda[order[j]] > lambda[order[i]]) { const int swap = order[i]; order[i] = order[j]; order[j] = swap; }
    for (i = 0; i < 3; ++i)
        for (k = 0; k < 3; ++k) a[k][i] = v[k][order[i]];
    /* the third column is the cross product of the first two: det V = 1 */
    a[0][2] = a[1][0] * a[2][1] - a[2][0] * a[1][1];
    a[1][2] = a[2][0] * a[0][1] - a[0][0] * a[2][1];
    a[2][2] = a[0][0] * a[1][1] - a[1][0] * a[0][1];
    memcpy(v, a, sizeof a);
    /* U: the first two columns are F v / sigma, made orthonormal; the third is their cross product. */
    for (i = 0; i < 2; ++i) {
        double length = 0.0, along = 0.0;
        for (k = 0; k < 3; ++k) u[k][i] = f[k][0] * v[0][i] + f[k][1] * v[1][i] + f[k][2] * v[2][i];
        for (k = 0; k < 3 && i == 1; ++k) along += u[k][0] * u[k][1];
        for (k = 0; k < 3 && i == 1; ++k) u[k][1] -= along * u[k][0];
        for (k = 0; k < 3; ++k) length += u[k][i] * u[k][i];
        length = sqrt(length);
        if (length < 1e-150) {
            /* F has squashed this direction to nothing: any unit vector at right angles will do */
            const int axis = i == 0 ? 0 : fabs(u[0][0]) < 0.6 ? 0 : 1;
            for (k = 0; k < 3; ++k) u[k][i] = k == axis ? 1.0 : 0.0;
            for (k = 0, along = 0.0; k < 3 && i == 1; ++k) along += u[k][0] * u[k][1];
            for (k = 0; k < 3 && i == 1; ++k) u[k][1] -= along * u[k][0];
            for (k = 0, length = 0.0; k < 3; ++k) length += u[k][i] * u[k][i];
            length = sqrt(length);
        }
        for (k = 0; k < 3; ++k) u[k][i] /= length;
    }
    u[0][2] = u[1][0] * u[2][1] - u[2][0] * u[1][1];
    u[1][2] = u[2][0] * u[0][1] - u[0][0] * u[2][1];
    u[2][2] = u[0][0] * u[1][1] - u[1][0] * u[0][1];
    for (i = 0; i < 3; ++i) {
        sigma[i] = 0.0;
        for (j = 0; j < 3; ++j)
            for (k = 0; k < 3; ++k) sigma[i] += u[j][i] * f[j][k] * v[k][i];
    }
}

void pd_project_strain(const double f[3][3], double low, double high, double out[3][3]) {
    double u[3][3], sigma[3], v[3][3];
    int    i, j, k;
    pd_svd(f, u, sigma, v);
    memset(out, 0, sizeof(double[3][3]));
    for (k = 0; k < 3; ++k) {
        const double kept = sigma[k] < low ? low : sigma[k] > high ? high : sigma[k];
        for (i = 0; i < 3; ++i)
            for (j = 0; j < 3; ++j) out[i][j] += u[i][k] * kept * v[j][k];
    }
}

void pd_project_volume(const double f[3][3], double low, double high, double out[3][3]) {
    double u[3][3], sigma[3], v[3][3], d[3], det;
    int    i, j, k, round;
    pd_svd(f, u, sigma, v);
    det = sigma[0] * sigma[1] * sigma[2];
    memcpy(d, sigma, sizeof d);
    if (det < low || det > high) {
        /* The nearest singular values with product `wanted`: step to the tangent plane of the
         * surface d0 d1 d2 = wanted from sigma, again and again (appendix A). */
        const double wanted = det < low ? low : high;
        for (round = 0; round < 40; ++round) {
            const double value = d[0] * d[1] * d[2] - wanted, g[3] = {d[1] * d[2], d[0] * d[2], d[0] * d[1]};
            const double scale = (g[0] * (d[0] - sigma[0]) + g[1] * (d[1] - sigma[1]) + g[2] * (d[2] - sigma[2]) - value) / (g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
            for (k = 0; k < 3; ++k) d[k] = sigma[k] + scale * g[k];
        }
    }
    memset(out, 0, sizeof(double[3][3]));
    for (k = 0; k < 3; ++k)
        for (i = 0; i < 3; ++i)
            for (j = 0; j < 3; ++j) out[i][j] += u[i][k] * d[k] * v[j][k];
}

double pd_factor(Pd *s) {
    const uint32_t n = s->nodes;
    double        *a, *l, worst = 0.0, largest = 0.0;
    uint32_t       t, i, j, k;
    int            x, y, c;
    free(s->factor); free(s->inertia); free(s->rhs); free(s->project);
    s->factor  = l = (double *)calloc((size_t)n * n, sizeof(double));
    s->inertia = (double *)calloc((size_t)n * 3u, sizeof(double));
    s->rhs     = (double *)calloc((size_t)n * 3u, sizeof(double));
    s->project = (double *)calloc((size_t)s->tets * 18u, sizeof(double));
    a = (double *)calloc((size_t)n * n, sizeof(double));
    /* M / h^2 + attachments + sum over tetrahedra of w G G^T. One n x n matrix serves x, y and z,
     * because F's three rows are the same combination of the nodes' x, y and z. */
    for (i = 0u; i < n; ++i) a[(size_t)i * n + i] = s->mass[i] / (s->h * s->h) + s->attach[i];
    for (t = 0u; t < s->tets; ++t) {
        const PdTet *e = &s->tet[t];
        const double w = (s->stiffness + s->volume_stiffness) * e->volume;
        for (x = 0; x < 4; ++x)
            for (y = 0; y < 4; ++y)
                for (c = 0; c < 3; ++c) a[(size_t)e->node[x] * n + e->node[y]] += w * e->grad[x][c] * e->grad[y][c];
    }
    /* Cholesky: A = L L^T */
    for (j = 0u; j < n; ++j) {
        double sum = a[(size_t)j * n + j];
        for (k = 0u; k < j; ++k) sum -= l[(size_t)j * n + k] * l[(size_t)j * n + k];
        l[(size_t)j * n + j] = sqrt(sum);
        for (i = j + 1u; i < n; ++i) {
            sum = a[(size_t)i * n + j];
            for (k = 0u; k < j; ++k) sum -= l[(size_t)i * n + k] * l[(size_t)j * n + k];
            l[(size_t)i * n + j] = sum / l[(size_t)j * n + j];
        }
    }
    /* how well it reproduces A, on a sample of rows */
    for (i = 0u; i < n; i += 1u + n / 40u)
        for (j = 0u; j <= i; ++j) {
            double sum = 0.0;
            for (k = 0u; k <= j; ++k) sum += l[(size_t)i * n + k] * l[(size_t)j * n + k];
            worst   = fmax(worst, fabs(sum - a[(size_t)i * n + j]));
            largest = fmax(largest, fabs(a[(size_t)i * n + j]));
        }
    free(a);
    return worst / largest;
}

/* F of one tetrahedron at positions q. */
static void deformation(const PdTet *e, const double *q, double f[3][3]) {
    int i, r, c;
    memset(f, 0, sizeof(double[3][3]));
    for (i = 0; i < 4; ++i)
        for (r = 0; r < 3; ++r)
            for (c = 0; c < 3; ++c) f[r][c] += q[3u * e->node[i] + (uint32_t)r] * e->grad[i][c];
}

double pd_energy(const Pd *s, const double *q, double *gradient) {
    double   energy = 0.0, f[3][3], p[3][3];
    uint32_t i, t;
    int      n, r, c, pass;
    for (i = 0u; i < s->nodes * 3u; ++i) {
        const double m = s->mass[i / 3u] / (s->h * s->h), w = s->attach[i / 3u];
        energy += 0.5 * m * (q[i] - s->inertia[i]) * (q[i] - s->inertia[i]) + 0.5 * w * (q[i] - s->target[i]) * (q[i] - s->target[i]);
        if (gradient) gradient[i] = m * (q[i] - s->inertia[i]) + w * (q[i] - s->target[i]);
    }
    for (t = 0u; t < s->tets; ++t) {
        const PdTet *e = &s->tet[t];
        deformation(e, q, f);
        for (pass = 0; pass < 2; ++pass) {
            const double w = (pass == 0 ? s->stiffness : s->volume_stiffness) * e->volume;
            if (pass == 0) pd_project_strain(f, s->low, s->high, p);
            else pd_project_volume(f, s->volume_low, s->volume_high, p);
            for (r = 0; r < 3; ++r)
                for (c = 0; c < 3; ++c) {
                    energy += 0.5 * w * (f[r][c] - p[r][c]) * (f[r][c] - p[r][c]);
                    /* the projection is a nearest point, so its own change does not enter the gradient */
                    for (n = 0; n < 4 && gradient; ++n) gradient[3u * e->node[n] + (uint32_t)r] += w * (f[r][c] - p[r][c]) * e->grad[n][c];
                }
        }
    }
    return energy;
}

void pd_step(Pd *s, double *trace) {
    const uint32_t n = s->nodes;
    const double  *l = s->factor;
    double        *q = (double *)malloc((size_t)n * 3u * sizeof(double)), f[3][3];
    uint32_t       i, k, t, iteration;
    int            node, r, c, axis;
    /* s_n, which is also the first guess */
    for (i = 0u; i < n * 3u; ++i) {
        s->inertia[i] = s->q[i] + s->h * s->v[i] - (i % 3u == 1u ? s->h * s->h * s->gravity : 0.0);
        q[i]          = s->inertia[i];
    }
    if (trace) trace[0] = pd_energy(s, q, NULL);
    for (iteration = 0u; iteration < s->iterations; ++iteration) {
        /* local: every constraint projects by itself */
        for (t = 0u; t < s->tets; ++t) {
            deformation(&s->tet[t], q, f);
            pd_project_strain(f, s->low, s->high, (double (*)[3])&s->project[18u * t]);
            pd_project_volume(f, s->volume_low, s->volume_high, (double (*)[3])&s->project[18u * t + 9u]);
        }
        /* global: right-hand side M s / h^2 + sum w A^T p, then two triangular solves an axis */
        for (i = 0u; i < n * 3u; ++i) s->rhs[i] = s->mass[i / 3u] / (s->h * s->h) * s->inertia[i] + s->attach[i / 3u] * s->target[i];
        for (t = 0u; t < s->tets; ++t) {
            const PdTet  *e = &s->tet[t];
            const double *p = &s->project[18u * t];
            for (node = 0; node < 4; ++node)
                for (r = 0; r < 3; ++r)
                    for (c = 0; c < 3; ++c)
                        s->rhs[3u * e->node[node] + (uint32_t)r] +=
                            e->volume * (s->stiffness * p[3 * r + c] + s->volume_stiffness * p[9 + 3 * r + c]) * e->grad[node][c];
        }
        for (axis = 0; axis < 3; ++axis) {
            for (i = 0u; i < n; ++i) {
                double sum = s->rhs[3u * i + (uint32_t)axis];
                for (k = 0u; k < i; ++k) sum -= l[(size_t)i * n + k] * q[3u * k + (uint32_t)axis];
                q[3u * i + (uint32_t)axis] = sum / l[(size_t)i * n + i];
            }
            for (i = n; i-- > 0u;) {
                double sum = q[3u * i + (uint32_t)axis];
                for (k = i + 1u; k < n; ++k) sum -= l[(size_t)k * n + i] * q[3u * k + (uint32_t)axis];
                q[3u * i + (uint32_t)axis] = sum / l[(size_t)i * n + i];
            }
        }
        if (trace) trace[iteration + 1u] = pd_energy(s, q, NULL);
    }
    for (i = 0u; i < n * 3u; ++i) {
        s->v[i] = (q[i] - s->q[i]) / s->h;
        s->q[i] = q[i];
    }
    free(q);
}
