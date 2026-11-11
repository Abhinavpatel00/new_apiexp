/* method.c — Chen and Desbrun: "Go Green: General Regularized Green's Functions for
 * Elasticity" (SIGGRAPH 2022).
 *
 * The displacement that a smoothed point load causes in an infinite anisotropic elastic body is
 * (eq. 10)
 *
 *     G(x) = 1 / (2 pi^2)  sum_l sum_m  i^l  Y_lm(x / |x|)  R_l(|x|)  P_lm(C)
 *
 * a direction (the harmonic), a distance (R_l, from the shape of the load: eq. 13 for a
 * Gaussian) and a matrix that depends on the material only,
 * P_lm = integral over the unit sphere of (C_ijkl xi_j xi_l)^-1 conj(Y_lm(xi)). The matrices
 * are computed once; G is then a short sum at each point. Eq. 12 is the same for dG/dx.
 *
 * Two things are done differently from the paper and give the same G:
 *   - real harmonics instead of complex ones (sum_m Y_lm(a) conj(Y_lm(b)) is the same sum
 *     with real harmonics), so P is real, i^l is +-1 on the even degrees that do not vanish
 *     and no real part has to be taken;
 *   - the sphere is integrated with a Gauss-Legendre x uniform grid, not Lebedev points.
 */
#include "method.h"

#include <math.h>
#include <string.h>

#define PI 3.14159265358979323846

void gg_isotropic(double C[3][3][3][3], double mu, double nu) {
    const double lambda = 2.0 * mu * nu / (1.0 - 2.0 * nu);
    int          i, j, k, l;
    for (i = 0; i < 3; ++i)
        for (j = 0; j < 3; ++j)
            for (k = 0; k < 3; ++k)
                for (l = 0; l < 3; ++l) C[i][j][k][l] = lambda * (i == j) * (k == l) + mu * ((i == k) * (j == l) + (i == l) * (j == k));
}

void gg_voigt(double C[3][3][3][3], const double V[6][6]) {
    static const int k_pair[3][3] = {{0, 5, 4}, {5, 1, 3}, {4, 3, 2}};
    int              i, j, k, l;
    for (i = 0; i < 3; ++i)
        for (j = 0; j < 3; ++j)
            for (k = 0; k < 3; ++k)
                for (l = 0; l < 3; ++l) C[i][j][k][l] = V[k_pair[i][j]][k_pair[k][l]];
}

void gg_rotate_z(double C[3][3][3][3], double angle) {
    const double R[3][3] = {{cos(angle), -sin(angle), 0.0}, {sin(angle), cos(angle), 0.0}, {0.0, 0.0, 1.0}};
    double       from[3][3][3][3];
    int          i, j, k, l, a, b, c, d;
    memcpy(from, C, sizeof from);
    for (i = 0; i < 3; ++i)
        for (j = 0; j < 3; ++j)
            for (k = 0; k < 3; ++k)
                for (l = 0; l < 3; ++l) {
                    double sum = 0.0;
                    for (a = 0; a < 3; ++a)
                        for (b = 0; b < 3; ++b)
                            for (c = 0; c < 3; ++c)
                                for (d = 0; d < 3; ++d) sum += R[i][a] * R[j][b] * R[k][c] * R[l][d] * from[a][b][c][d];
                    C[i][j][k][l] = sum;
                }
}

void gg_harmonics(int lmax, const double d[3], double *y) {
    const double x = d[2], s = sqrt(fmax(0.0, 1.0 - x * x)), phi = atan2(d[1], d[0]);
    double       pmm = 1.0;   /* associated Legendre P_m^m(x) */
    int          l, m, n;
    for (m = 0; m <= lmax; ++m) {
        double before = 0.0, now = pmm;   /* P_{l-1}^m and P_l^m */
        for (l = m; l <= lmax; ++l) {
            double norm = (2.0 * l + 1.0) / (4.0 * PI);   /* x (l - m)! / (l + m)! */
            for (n = l - m + 1; n <= l + m; ++n) norm /= n;
            norm = sqrt(norm);
            if (m == 0) y[l * l + l] = norm * now;
            if (m > 0) {
                y[l * l + l + m] = sqrt(2.0) * norm * now * cos(m * phi);
                y[l * l + l - m] = sqrt(2.0) * norm * now * sin(m * phi);
            }
            {
                const double next = ((2.0 * l + 1.0) * x * now - (l + m) * before) / (l + 1.0 - m);
                before = now; now = next;
            }
        }
        pmm *= -(2.0 * m + 1.0) * s;
    }
}

/* inverse of the symmetric matrix A_ik = C_ijkl xi_j xi_l */
static void acoustic_inverse(const double C[3][3][3][3], const double xi[3], double inv[3][3]) {
    double A[3][3] = {{0}}, det;
    int    i, j, k, l;
    for (i = 0; i < 3; ++i)
        for (j = 0; j < 3; ++j)
            for (k = 0; k < 3; ++k)
                for (l = 0; l < 3; ++l) A[i][k] += C[i][j][k][l] * xi[j] * xi[l];
    inv[0][0] = A[1][1] * A[2][2] - A[1][2] * A[2][1]; inv[0][1] = A[0][2] * A[2][1] - A[0][1] * A[2][2]; inv[0][2] = A[0][1] * A[1][2] - A[0][2] * A[1][1];
    inv[1][0] = A[1][2] * A[2][0] - A[1][0] * A[2][2]; inv[1][1] = A[0][0] * A[2][2] - A[0][2] * A[2][0]; inv[1][2] = A[0][2] * A[1][0] - A[0][0] * A[1][2];
    inv[2][0] = A[1][0] * A[2][1] - A[1][1] * A[2][0]; inv[2][1] = A[0][1] * A[2][0] - A[0][0] * A[2][1]; inv[2][2] = A[0][0] * A[1][1] - A[0][1] * A[1][0];
    det = A[0][0] * inv[0][0] + A[0][1] * inv[1][0] + A[0][2] * inv[2][0];
    for (i = 0; i < 3; ++i)
        for (j = 0; j < 3; ++j) inv[i][j] /= det;
}

void gg_material(Gg *s, int lmax, double epsilon, int n) {
    static double y[GG_TERMS];
    int           a, b, t, i, j, p, it;
    s->lmax = lmax; s->epsilon = epsilon;
    for (t = 0; t < GG_DEGREES; ++t) s->weight[t] = 1.0;
    memset(s->P, 0, sizeof s->P); memset(s->Q, 0, sizeof s->Q);
    for (a = 0; a < n; ++a) {
        /* node a of the n-point Gauss-Legendre rule by Newton's method on the Legendre polynomial */
        double x = cos(PI * (a + 0.75) / (n + 0.5)), slope = 1.0, weight;
        for (it = 0; it < 50; ++it) {
            double before = 1.0, now = x;
            for (t = 2; t <= n; ++t) { const double next = ((2.0 * t - 1.0) * x * now - (t - 1.0) * before) / t; before = now; now = next; }
            slope = n * (x * now - before) / (x * x - 1.0);
            x    -= now / slope;
        }
        weight = 2.0 / ((1.0 - x * x) * slope * slope) * PI / n;   /* x the step of the 2n angles about z */
        for (b = 0; b < 2 * n; ++b) {
            const double phi = PI * (b + 0.5) / n, xi[3] = {sqrt(1.0 - x * x) * cos(phi), sqrt(1.0 - x * x) * sin(phi), x};
            double       inv[3][3];
            acoustic_inverse(s->C, xi, inv);
            gg_harmonics(GG_DEGREES - 1, xi, y);
            for (t = 0; t < GG_TERMS; ++t)
                for (i = 0; i < 3; ++i)
                    for (j = 0; j < 3; ++j) {
                        s->P[t][i][j] += weight * y[t] * inv[i][j];
                        for (p = 0; p < 3; ++p) s->Q[t][p][i][j] += weight * y[t] * inv[i][j] * xi[p];
                    }
        }
    }
}

/* Kummer's function 1F1(a; b; -z) / Gamma(b) for z >= 0 */
static double kummer(double a, double b, double z) {
    double sum = 1.0, term = 1.0;
    int    n;
    if (z > 80.0) {
        /* far out: Gamma(b) / Gamma(b - a) z^-a (1 + a (a - b + 1) / z + ...), as long as the terms shrink */
        for (n = 0; n < 40; ++n) {
            const double next = term * (a + n) * (a - b + 1.0 + n) / ((n + 1.0) * z);
            if (fabs(next) >= fabs(term) || next == 0.0) break;
            term = next; sum += term;
        }
        return sum * pow(z, -a) / tgamma(b - a);
    }
    /* 1F1(a; b; -z) = exp(-z) 1F1(b - a; b; z): all terms positive */
    for (n = 0; n < 2000 && term > 1e-17 * sum; ++n) { term *= (b - a + n) * z / ((b + n) * (n + 1.0)); sum += term; }
    return exp(-z) * sum / tgamma(b);
}

double gg_radial(int l, double r, double epsilon) {
    return 0.5 * sqrt(PI) * pow(r, l) * pow(epsilon, -l - 1.0) * tgamma(0.5 * (l + 1.0)) * kummer(0.5 * (l + 1.0), l + 1.5, r * r / (epsilon * epsilon));
}
double gg_radial_affine(int l, double r, double epsilon) {
    return sqrt(PI) * pow(r, l) * pow(epsilon, -l - 2.0) * tgamma(0.5 * l + 1.0) * kummer(0.5 * l + 1.0, l + 1.5, r * r / (epsilon * epsilon));
}

void gg_green(const Gg *s, const double x[3], double G[3][3]) {
    const double r = sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
    const double d[3] = {r > 0.0 ? x[0] / r : 0.0, r > 0.0 ? x[1] / r : 0.0, r > 0.0 ? x[2] / r : 1.0};
    double       y[GG_TERMS];
    int          l, m, i, j;
    gg_harmonics(s->lmax, d, y);
    memset(G, 0, 9 * sizeof(double));
    for (l = 0; l <= s->lmax; l += 2) {
        const double radial = (l % 4 ? -1.0 : 1.0) * s->weight[l] * gg_radial(l, r, s->epsilon) / (2.0 * PI * PI);   /* i^l R_l / (2 pi^2) */
        for (m = -l; m <= l; ++m)
            for (i = 0; i < 3; ++i)
                for (j = 0; j < 3; ++j) G[i][j] += radial * y[l * l + l + m] * s->P[l * l + l + m][i][j];
    }
}

void gg_gradient(const Gg *s, const double x[3], double D[3][3][3]) {
    const double r = sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
    const double d[3] = {r > 0.0 ? x[0] / r : 0.0, r > 0.0 ? x[1] / r : 0.0, r > 0.0 ? x[2] / r : 1.0};
    double       y[GG_TERMS];
    int          l, m, i, j, p;
    gg_harmonics(s->lmax + 1, d, y);
    memset(D, 0, 27 * sizeof(double));
    for (l = 1; l <= s->lmax + 1; l += 2) {
        const double radial = ((l + 1) % 4 ? -1.0 : 1.0) * s->weight[l] * gg_radial_affine(l, r, s->epsilon) / (2.0 * PI * PI);   /* i^(l+1) */
        for (m = -l; m <= l; ++m)
            for (p = 0; p < 3; ++p)
                for (i = 0; i < 3; ++i)
                    for (j = 0; j < 3; ++j) D[p][i][j] += radial * y[l * l + l + m] * s->Q[l * l + l + m][p][i][j];
    }
}

bool gg_fit(const Gg *s, int k, const double *p, const double *u, double *h) {
    enum { MOST = 3 * 32 };
    static double A[MOST][MOST + 1];
    const int     n = 3 * k;
    int           a, b, i, j, row, col;
    for (a = 0; a < k; ++a)
        for (b = 0; b < k; ++b) {
            const double x[3] = {p[3 * a] - p[3 * b], p[3 * a + 1] - p[3 * b + 1], p[3 * a + 2] - p[3 * b + 2]};
            double       G[3][3];
            gg_green(s, x, G);
            for (i = 0; i < 3; ++i)
                for (j = 0; j < 3; ++j) A[3 * a + i][3 * b + j] = G[i][j];
        }
    for (i = 0; i < n; ++i) A[i][n] = u[i];
    /* Gaussian elimination with row pivoting */
    for (col = 0; col < n; ++col) {
        int pivot = col;
        for (row = col + 1; row < n; ++row)
            if (fabs(A[row][col]) > fabs(A[pivot][col])) pivot = row;
        if (fabs(A[pivot][col]) < 1e-300) return false;
        for (j = col; j <= n; ++j) { const double keep = A[col][j]; A[col][j] = A[pivot][j]; A[pivot][j] = keep; }
        for (row = col + 1; row < n; ++row) {
            const double factor = A[row][col] / A[col][col];
            for (j = col; j <= n; ++j) A[row][j] -= factor * A[col][j];
        }
    }
    for (i = n - 1; i >= 0; --i) {
        double sum = A[i][n];
        for (j = i + 1; j < n; ++j) sum -= A[i][j] * h[j];
        h[i] = sum / A[i][i];
    }
    return true;
}
