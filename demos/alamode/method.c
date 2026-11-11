/* method.c — "Actuators A La Mode: Modal Actuations for Soft Body Locomotion" (Otman Benchekroun, Kaixiang Xie,
 * Hsueh-Ti Derek Liu, Eitan Grinspun, Sheldon Andrews, Victor Zordan; SIGGRAPH Asia 2024, arXiv 2405.18609).
 *
 * A soft character with no skeleton is to move by itself. The paper gives it a small set of shapes it can want to
 * take, makes the simulation small as well, and searches the few numbers that are left with CMA-ES.
 *
 * What the character can want (section 3.1). The vibration modes D of the elastic energy at rest,
 *
 *     H D = M D Lambda                                                          (eq. 4)
 *
 * without the six of rigid motion. The target shape is d = x0 + D a (eq. 5) and each a_i is a sum of sines in
 * time (eq. 6): amplitude, period and phase are what the search finds.
 *
 * What moves it (section 3.2.1). An energy that is 0 when every tetrahedron has the shape of the target, however
 * it is turned:
 *
 *     E_a = 1/2 sum_e gamma V_e |F_e(x) - Omega Y_e(d)|^2,   Omega the best rotation   (eq. 9 to 11)
 *
 * F is the deformation gradient of the body, Y that of the target. Tetrahedra of a cluster share one Omega, which
 * is the rotation nearest to sum_e gamma V_e F_e Y_e' (eq. 12, appendix G). Because of Omega the actuation cannot
 * push or turn the body as a whole.
 *
 * The body (appendices A to D). ARAP elasticity, E_v = 1/2 sum_e mu V_e |F_e - R|^2 with clustered rotations R,
 * implicit Euler, and positions in a skinning subspace: w weight functions W (the lowest eigenvectors of the
 * Laplacian, L W = M W Lambda, eq. 19), each with an affine map, x_i = sum_j W_ij T_j [X_i; 1] (eq. 20). Written
 * with a 3 x 4w matrix Z of all the maps and Phi_i = (W_ij [X_i; 1])_j that is
 *
 *     x_i = Z Phi_i,      F_e = Z Psi_e,      Psi_e = sum over the nodes n of e of Phi_n grad_n'
 *
 * so the three coordinates share every matrix. One step minimises (eq. 14, 24, 25)
 *
 *     1/(2 h^2) |Z - Q|^2_N + gravity + E_v + E_a,      N = Phi' M Phi,  Q = 2 Z_now - Z_before
 *
 * by turns (section 3.3): rotations for the Z at hand (local), then Z for those rotations (global). The global
 * step is Z K = right side with K = N / h^2 + sum_e (mu + gamma) V_e Psi_e Psi_e', which never changes. The
 * sums over tetrahedra that the rotations need are products of Z with matrices made once (eq. 13, appendix F),
 * so a step does not touch the mesh.
 *
 * The ground (appendix E). A sample node that would end below y = 0 is held: its height is 0 and it keeps the
 * share u of its sideways motion (eq. 47). The force f_c added to the right side for that is the smallest one
 * that does it, or with more held numbers than unknowns the one that does it best (eq. 50 to 52). The paper asks
 * for no normal velocity; here the node is put on the ground, and which nodes are held is found from the step
 * without ground. The paper does not say how it picks them.
 *
 * The search (section 4). J = -(way of the centre of mass along the target direction) x (how well the body kept
 * facing that way) (eq. 17), by CMA-ES over the amplitudes, periods and phases.
 */
#include "method.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Eigenvalues (rising) and eigenvectors (the columns of vec; n x n, row after row) of the symmetric a, which is
 * destroyed. Jacobi rotations. */
static void spectrum(int n, double *a, double *vec, double *val) {
    for (int i = 0; i < n * n; ++i) vec[i] = i % (n + 1) ? 0.0 : 1.0;
    for (int sweep = 0; sweep < 100; ++sweep) {
        double off = 0.0, all = 0.0;
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n; ++j) {
                all += a[i * n + j] * a[i * n + j];
                off += i == j ? 0.0 : a[i * n + j] * a[i * n + j];
            }
        if (off <= 1e-30 * all) break;
        for (int p = 0; p < n - 1; ++p)
            for (int q = p + 1; q < n; ++q) {
                const double theta = (a[q * n + q] - a[p * n + p]) / (2.0 * a[p * n + q] + (a[p * n + q] < 0.0 ? -1e-300 : 1e-300));
                const double t = (theta < 0.0 ? -1.0 : 1.0) / (fabs(theta) + sqrt(theta * theta + 1.0)), c = 1.0 / sqrt(t * t + 1.0), s = t * c;
                for (int k = 0; k < n; ++k) {
                    const double kp = a[k * n + p], kq = a[k * n + q], vp = vec[k * n + p], vq = vec[k * n + q];
                    a[k * n + p] = c * kp - s * kq;     a[k * n + q] = s * kp + c * kq;
                    vec[k * n + p] = c * vp - s * vq;   vec[k * n + q] = s * vp + c * vq;
                }
                for (int k = 0; k < n; ++k) {
                    const double pk = a[p * n + k], qk = a[q * n + k];
                    a[p * n + k] = c * pk - s * qk;     a[q * n + k] = s * pk + c * qk;
                }
            }
    }
    for (int i = 0; i < n; ++i) val[i] = a[i * n + i];
    for (int i = 0; i < n - 1; ++i) {
        int least = i;
        for (int j = i + 1; j < n; ++j) least = val[j] < val[least] ? j : least;
        for (int k = 0; k < n && least != i; ++k) { const double swap = vec[k * n + i]; vec[k * n + i] = vec[k * n + least]; vec[k * n + least] = swap; }
        { const double swap = val[i]; val[i] = val[least]; val[least] = swap; }
    }
}

/* a x = b for `count` right sides (the rows of b, n long each; the answers replace them). a is symmetric positive
 * definite and is destroyed (Cholesky). */
static void solve(int n, double *a, int count, double *b) {
    for (int i = 0; i < n; ++i)
        for (int j = 0; j <= i; ++j) {
            double sum = a[i * n + j];
            for (int k = 0; k < j; ++k) sum -= a[i * n + k] * a[j * n + k];
            a[i * n + j] = i == j ? sqrt(sum) : sum / a[j * n + j];
        }
    for (int r = 0; r < count; ++r) {
        double *x = &b[r * n];
        for (int i = 0; i < n; ++i) {
            for (int k = 0; k < i; ++k) x[i] -= a[i * n + k] * x[k];
            x[i] /= a[i * n + i];
        }
        for (int i = n - 1; i >= 0; --i) {
            x[i] /= a[i * n + i];
            for (int k = 0; k < i; ++k) x[k] -= a[i * n + k] * x[i];
        }
    }
}

/* The `count` lowest eigenpairs of a x = lambda mass x. a is symmetric and given by its lower band, row after row:
 * entry (i, j) with i - band <= j <= i is a[i * (band + 1) + j - i + band]; mass is the diagonal. vec gets count
 * columns of n with x' mass x = 1. Subspace iteration with (a + shift mass)^-1 mass, which also finds equal
 * eigenvalues (the rigid motions). Returns the iterations. */
static int lowest(int n, int band, const double *a, const double *mass, int count, double *vec, double *val) {
    const int wide = band + 1, block = count + 8 < n ? count + 8 : n;
    double   *factor = (double *)malloc((size_t)n * (size_t)wide * sizeof(double));
    double   *x = (double *)malloc((size_t)block * (size_t)n * sizeof(double)), *image = (double *)malloc((size_t)block * (size_t)n * sizeof(double));
    double   *small = (double *)malloc((size_t)block * (size_t)block * sizeof(double)), *mix = (double *)malloc((size_t)block * (size_t)block * sizeof(double));
    double   *lambda = (double *)malloc((size_t)block * sizeof(double)), *row = (double *)malloc((size_t)block * sizeof(double));
    double    shift = 0.0, weight = 0.0, worst;
    uint32_t  seed = 12345u;
    int       iterations = 0;

    /* Cholesky factor of a + shift mass, in the band. */
    for (int i = 0; i < n; ++i) { shift += a[i * wide + band]; weight += mass[i]; }
    shift *= 1e-5 / weight;
    memcpy(factor, a, (size_t)n * (size_t)wide * sizeof(double));
    for (int i = 0; i < n; ++i) {
        const int first = i - band > 0 ? i - band : 0;
        factor[i * wide + band] += shift * mass[i];
        for (int j = first; j <= i; ++j) {
            double sum = factor[i * wide + j - i + band];
            for (int k = first; k < j; ++k) sum -= factor[i * wide + k - i + band] * factor[j * wide + k - j + band];
            factor[i * wide + j - i + band] = i == j ? sqrt(sum) : sum / factor[j * wide + band];
        }
    }
    for (int i = 0; i < block * n; ++i) {
        seed = seed * 1664525u + 1013904223u;
        x[i] = (double)(seed >> 8) / 16777216.0 - 0.5;
    }
    do {
        for (int k = 0; k < block; ++k) {
            double *v = &x[k * n], norm = 0.0;
            /* v <- (a + shift mass)^-1 mass v */
            for (int i = 0; i < n; ++i) {
                const int first = i - band > 0 ? i - band : 0;
                v[i] *= mass[i];
                for (int e = first; e < i; ++e) v[i] -= factor[i * wide + e - i + band] * v[e];
                v[i] /= factor[i * wide + band];
            }
            for (int i = n - 1; i >= 0; --i) {
                const int first = i - band > 0 ? i - band : 0;
                v[i] /= factor[i * wide + band];
                for (int e = first; e < i; ++e) v[e] -= factor[i * wide + e - i + band] * v[i];
            }
            /* orthonormal to the earlier ones in the mass inner product (twice, against rounding) */
            for (int pass = 0; pass < 2; ++pass)
                for (int j = 0; j < k; ++j) {
                    double dot = 0.0;
                    for (int i = 0; i < n; ++i) dot += mass[i] * x[j * n + i] * v[i];
                    for (int i = 0; i < n; ++i) v[i] -= dot * x[j * n + i];
                }
            for (int i = 0; i < n; ++i) norm += mass[i] * v[i] * v[i];
            for (int i = 0; i < n; ++i) v[i] /= sqrt(norm);
            /* its image under a */
            memset(&image[k * n], 0, (size_t)n * sizeof(double));
            for (int i = 0; i < n; ++i) {
                image[k * n + i] += a[i * wide + band] * v[i];
                for (int e = i - band > 0 ? i - band : 0; e < i; ++e) {
                    image[k * n + i] += a[i * wide + e - i + band] * v[e];
                    image[k * n + e] += a[i * wide + e - i + band] * v[i];
                }
            }
        }
        /* Rayleigh-Ritz: the eigenvectors of x' a x turn the block. */
        for (int j = 0; j < block; ++j)
            for (int k = 0; k < block; ++k) {
                double dot = 0.0;
                for (int i = 0; i < n; ++i) dot += x[j * n + i] * image[k * n + i];
                small[j * block + k] = dot;
            }
        spectrum(block, small, mix, lambda);
        for (int i = 0; i < n; ++i)
            for (int which = 0; which < 2; ++which) {
                double *all = which ? image : x;
                for (int k = 0; k < block; ++k) {
                    row[k] = 0.0;
                    for (int j = 0; j < block; ++j) row[k] += all[j * n + i] * mix[j * block + k];
                }
                for (int k = 0; k < block; ++k) all[k * n + i] = row[k];
            }
        worst = 0.0;
        for (int k = 0; k < count; ++k) {
            double left = 0.0;
            for (int i = 0; i < n; ++i) left += (image[k * n + i] - lambda[k] * mass[i] * x[k * n + i]) * (image[k * n + i] - lambda[k] * mass[i] * x[k * n + i]) / mass[i];
            worst = fmax(worst, sqrt(left) / (fabs(lambda[k]) + shift));
        }
    } while (++iterations < 500 && worst > 1e-9);
    memcpy(vec, x, (size_t)count * (size_t)n * sizeof(double));
    memcpy(val, lambda, (size_t)count * sizeof(double));
    free(factor); free(x); free(image); free(small); free(mix); free(lambda); free(row);
    return iterations;
}

void am_polar(const double h[9], double r[9]) {
    double gram[9], axes[9], size[3], u[3][3], v[3][3], length = 0.0, along = 0.0;

    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) gram[i * 3 + j] = h[i] * h[j] + h[3 + i] * h[3 + j] + h[6 + i] * h[6 + j];
    spectrum(3, gram, axes, size);
    /* v: the two directions that h stretches most, and their cross product; u: where h takes them, made
     * orthonormal the same way. With a mirroring h the weakest direction is the one that gets flipped. */
    for (int i = 0; i < 3; ++i) { v[0][i] = axes[i * 3 + 2]; v[1][i] = axes[i * 3 + 1]; }
    for (int k = 0; k < 2; ++k)
        for (int i = 0; i < 3; ++i) u[k][i] = h[i * 3] * v[k][0] + h[i * 3 + 1] * v[k][1] + h[i * 3 + 2] * v[k][2];
    for (int i = 0; i < 3; ++i) length += u[0][i] * u[0][i];
    for (int i = 0; i < 3; ++i) u[0][i] /= sqrt(length) + 1e-300;
    for (int i = 0; i < 3; ++i) along += u[0][i] * u[1][i];
    length = 0.0;
    for (int i = 0; i < 3; ++i) { u[1][i] -= along * u[0][i]; length += u[1][i] * u[1][i]; }
    for (int i = 0; i < 3; ++i) u[1][i] /= sqrt(length) + 1e-300;
    for (int i = 0; i < 3; ++i) {
        const int j = (i + 1) % 3, k = (i + 2) % 3;
        u[2][i] = u[0][j] * u[1][k] - u[0][k] * u[1][j];
        v[2][i] = v[0][j] * v[1][k] - v[0][k] * v[1][j];
    }
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r[i * 3 + j] = u[0][i] * v[0][j] + u[1][i] * v[1][j] + u[2][i] * v[2][j];
}

/* Groups the tetrahedra into `count` clusters by their mean weights (k-means in weight space, as Jacobson et al.
 * 2012 do). */
static void cluster(const Am *m, int count, int *out) {
    const int w = m->setup.weights;
    double   *feature = (double *)calloc((size_t)m->tets * (size_t)w, sizeof(double)), *centre = (double *)calloc((size_t)count * (size_t)w, sizeof(double));
    double   *far = (double *)malloc((size_t)m->tets * sizeof(double)), *sum = (double *)malloc((size_t)count * (size_t)(w + 1) * sizeof(double));

    for (int e = 0; e < m->tets; ++e) {
        for (int k = 0; k < 4; ++k)
            for (int j = 0; j < w; ++j) feature[e * w + j] += 0.25 * m->weight[j * m->nodes + (int)m->tet[4 * e + k]];
        far[e] = DBL_MAX;
        out[e] = 0;
    }
    /* the first centre is tetrahedron 0, each further one the tetrahedron farthest from those there are */
    for (int c = 0, next = 0; c < count; ++c) {
        int farthest = 0;
        memcpy(&centre[c * w], &feature[next * w], (size_t)w * sizeof(double));
        for (int e = 0; e < m->tets; ++e) {
            double apart = 0.0;
            for (int j = 0; j < w; ++j) apart += (feature[e * w + j] - centre[c * w + j]) * (feature[e * w + j] - centre[c * w + j]);
            far[e] = fmin(far[e], apart);
            farthest = far[e] > far[farthest] ? e : farthest;
        }
        next = farthest;
    }
    for (int round = 0; round < 30; ++round) {
        memset(sum, 0, (size_t)count * (size_t)(w + 1) * sizeof(double));
        for (int e = 0; e < m->tets; ++e) {
            double least = DBL_MAX;
            for (int c = 0; c < count; ++c) {
                double apart = 0.0;
                for (int j = 0; j < w; ++j) apart += (feature[e * w + j] - centre[c * w + j]) * (feature[e * w + j] - centre[c * w + j]);
                if (apart < least) { least = apart; out[e] = c; }
            }
            for (int j = 0; j < w; ++j) sum[out[e] * (w + 1) + j] += feature[e * w + j];
            sum[out[e] * (w + 1) + w] += 1.0;
        }
        for (int c = 0; c < count; ++c)
            for (int j = 0; j < w && sum[c * (w + 1) + w] > 0.0; ++j) centre[c * w + j] = sum[c * (w + 1) + j] / sum[c * (w + 1) + w];
    }
    free(feature); free(centre); free(far); free(sum);
}

Am *am_open(int nodes, const double *rest, int tets, const uint32_t *tet, int faces, const uint32_t *face, const AmSetup *setup) {
    const int    w = setup->weights, p = 4 * w, modes = setup->modes, n3 = 3 * nodes;
    const double h = setup->step;
    Am          *m = (Am *)calloc(1u, sizeof(Am));
    double      *matrix, *mass3, *found, *value, *far, lo[3] = {DBL_MAX, DBL_MAX, DBL_MAX}, hi[3] = {-DBL_MAX, -DBL_MAX, -DBL_MAX}, extent;
    bool        *outside;
    int          band = 0, wide;

    m->setup = *setup;
    m->nodes = nodes; m->tets = tets; m->p = p; m->rest = rest; m->tet = tet;
    m->mass   = (double *)calloc((size_t)nodes, sizeof(double));
    m->volume = (double *)malloc((size_t)tets * sizeof(double));
    m->grad   = (double *)malloc((size_t)tets * 12u * sizeof(double));
    for (int e = 0; e < tets; ++e) {
        const uint32_t *n = &tet[4 * e];
        double          d[3][3], *g = &m->grad[12 * e], det;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) d[r][c] = rest[3u * n[c + 1] + r] - rest[3u * n[0] + r];
        det = d[0][0] * (d[1][1] * d[2][2] - d[1][2] * d[2][1]) - d[0][1] * (d[1][0] * d[2][2] - d[1][2] * d[2][0]) + d[0][2] * (d[1][0] * d[2][1] - d[1][1] * d[2][0]);
        /* the rows of the inverse of d are the gradients of the shape functions of nodes 1 to 3 */
        for (int c = 0; c < 3; ++c) {
            const int c1 = (c + 1) % 3, c2 = (c + 2) % 3;
            for (int r = 0; r < 3; ++r) {
                const int r1 = (r + 1) % 3, r2 = (r + 2) % 3;
                g[3 * (c + 1) + r] = (d[r1][c1] * d[r2][c2] - d[r1][c2] * d[r2][c1]) / det;
            }
        }
        for (int r = 0; r < 3; ++r) g[r] = -g[3 + r] - g[6 + r] - g[9 + r];
        m->volume[e] = det / 6.0;
        for (int k = 0; k < 4; ++k) {
            m->mass[n[k]] += 0.25 * setup->density * m->volume[e];
            for (int l = 0; l < 4; ++l) band = abs((int)n[k] - (int)n[l]) > band ? abs((int)n[k] - (int)n[l]) : band;
        }
    }
    for (int i = 0; i < nodes; ++i) {
        m->total += m->mass[i];
        for (int a = 0; a < 3; ++a) { lo[a] = fmin(lo[a], rest[3 * i + a]); hi[a] = fmax(hi[a], rest[3 * i + a]); }
    }
    extent = fmax(hi[0] - lo[0], fmax(hi[1] - lo[1], hi[2] - lo[2]));

    /* Eq. 19: the weights are the lowest eigenvectors of the Laplacian sum_e mu V grad_a . grad_b. */
    wide   = band + 1;
    matrix = (double *)calloc((size_t)nodes * (size_t)wide, sizeof(double));
    for (int e = 0; e < tets; ++e)
        for (int k = 0; k < 4; ++k)
            for (int l = 0; l < 4; ++l) {
                const int     i = (int)tet[4 * e + k], j = (int)tet[4 * e + l];
                const double *gk = &m->grad[12 * e + 3 * k], *gl = &m->grad[12 * e + 3 * l];
                if (j <= i) matrix[i * wide + j - i + band] += setup->stiffness * m->volume[e] * (gk[0] * gl[0] + gk[1] * gl[1] + gk[2] * gl[2]);
            }
    m->weight = (double *)malloc((size_t)w * (size_t)nodes * sizeof(double));
    value     = (double *)malloc((size_t)(w > 6 + modes ? w : 6 + modes) * sizeof(double));
    m->sweeps[0] = lowest(nodes, band, matrix, m->mass, w, m->weight, value);
    free(matrix);

    /* Eq. 4: the Hessian of the elastic energy at rest. ARAP there is 1/2 mu V |symmetric part of grad u|^2, which
     * for nodes n, m and coordinates i, k gives mu V / 2 (delta_ik grad_n . grad_m + grad_n[k] grad_m[i]). */
    wide   = 3 * band + 3;
    matrix = (double *)calloc((size_t)n3 * (size_t)wide, sizeof(double));
    mass3  = (double *)malloc((size_t)n3 * sizeof(double));
    found  = (double *)malloc((size_t)(6 + modes) * (size_t)n3 * sizeof(double));
    for (int i = 0; i < n3; ++i) mass3[i] = m->mass[i / 3];
    for (int e = 0; e < tets; ++e)
        for (int k = 0; k < 4; ++k)
            for (int l = 0; l < 4; ++l)
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j) {
                        const int     row = 3 * (int)tet[4 * e + k] + i, col = 3 * (int)tet[4 * e + l] + j;
                        const double *gk = &m->grad[12 * e + 3 * k], *gl = &m->grad[12 * e + 3 * l];
                        if (col <= row) matrix[row * wide + col - row + wide - 1] += 0.5 * setup->stiffness * m->volume[e] * ((i == j ? gk[0] * gl[0] + gk[1] * gl[1] + gk[2] * gl[2] : 0.0) + gk[j] * gl[i]);
                    }
    m->stiff     = (double *)malloc((size_t)(6 + modes) * sizeof(double));
    m->sweeps[1] = lowest(n3, wide - 1, matrix, mass3, 6 + modes, found, m->stiff);
    free(matrix); free(mass3);
    /* Each mode is scaled to a largest displacement of 1, and signed by a fixed smooth field so that a finer mesh
     * of the same shape gets the same sign. */
    m->shape = (double *)malloc((size_t)modes * (size_t)n3 * sizeof(double));
    for (int k = 0; k < modes; ++k) {
        const double *d = &found[(6 + k) * n3];
        double        largest = 0.0, side = 0.0;
        for (int i = 0; i < nodes; ++i) {
            const double x = (rest[3 * i] - lo[0]) / extent, y = (rest[3 * i + 1] - lo[1]) / extent, z = (rest[3 * i + 2] - lo[2]) / extent;
            largest = fmax(largest, sqrt(d[3 * i] * d[3 * i] + d[3 * i + 1] * d[3 * i + 1] + d[3 * i + 2] * d[3 * i + 2]));
            side += m->mass[i] * (d[3 * i] * sin(3.0 * x + 1.0) + d[3 * i + 1] * cos(2.0 * y + 0.5 * x) + d[3 * i + 2] * sin(2.0 * z + x + 0.3));
        }
        for (int i = 0; i < n3; ++i) m->shape[k * n3 + i] = (side < 0.0 ? -1.0 : 1.0) * d[i] / largest;
    }
    free(found); free(value);
    m->spring = setup->strength / setup->stiffness * m->stiff[6];

    /* Eq. 20 and what follows from it for a tetrahedron. */
    m->basis  = (double *)malloc((size_t)nodes * (size_t)p * sizeof(double));
    m->strain = (double *)calloc((size_t)tets * (size_t)p * 3u, sizeof(double));
    for (int i = 0; i < nodes; ++i)
        for (int j = 0; j < w; ++j)
            for (int b = 0; b < 4; ++b) m->basis[i * p + 4 * j + b] = m->weight[j * nodes + i] * (b < 3 ? rest[3 * i + b] : 1.0);
    for (int e = 0; e < tets; ++e)
        for (int k = 0; k < 4; ++k)
            for (int q = 0; q < p; ++q)
                for (int j = 0; j < 3; ++j) m->strain[(e * p + q) * 3 + j] += m->basis[(int)tet[4 * e + k] * p + q] * m->grad[12 * e + 3 * k + j];
    m->passive = (int *)malloc((size_t)tets * sizeof(int));
    m->active  = (int *)malloc((size_t)tets * sizeof(int));
    cluster(m, setup->passive, m->passive);
    cluster(m, setup->active, m->active);

    /* The matrices of the reduced step. */
    m->inertia     = (double *)calloc((size_t)p * (size_t)p, sizeof(double));
    m->system      = (double *)calloc((size_t)p * (size_t)p, sizeof(double));
    m->inverse     = (double *)calloc((size_t)p * (size_t)p, sizeof(double));
    m->heavy       = (double *)calloc((size_t)p, sizeof(double));
    m->fit_passive = (double *)calloc((size_t)setup->passive * (size_t)p * 3u, sizeof(double));
    m->fit_whole   = (double *)calloc((size_t)p * 3u, sizeof(double));
    m->fit_active  = (double *)calloc((size_t)setup->active * (size_t)(modes + 1) * (size_t)p * 3u, sizeof(double));
    m->pulled      = (double *)calloc((size_t)(modes + 1) * 3u * (size_t)p, sizeof(double));
    for (int i = 0; i < nodes; ++i)
        for (int q = 0; q < p; ++q) {
            m->heavy[q] += m->mass[i] * m->basis[i * p + q];
            for (int r = 0; r < p; ++r) m->inertia[q * p + r] += m->mass[i] * m->basis[i * p + q] * m->basis[i * p + r];
            for (int v = 0; v <= modes; ++v)
                for (int a = 0; a < 3; ++a) m->pulled[(v * 3 + a) * p + q] += m->mass[i] * m->basis[i * p + q] * (v < modes ? m->shape[v * n3 + 3 * i + a] : rest[3 * i + a]);
        }
    for (int q = 0; q < p * p; ++q) m->system[q] = m->inertia[q] * (1.0 / (h * h) + (setup->pull ? m->spring : 0.0));
    for (int e = 0; e < tets; ++e) {
        const double *psi = &m->strain[e * p * 3], hard = setup->stiffness * m->volume[e], strong = setup->strength * m->volume[e];
        for (int q = 0; q < p; ++q) {
            for (int r = 0; r < p; ++r) m->system[q * p + r] += (hard + (setup->pull ? 0.0 : strong)) * (psi[q * 3] * psi[r * 3] + psi[q * 3 + 1] * psi[r * 3 + 1] + psi[q * 3 + 2] * psi[r * 3 + 2]);
            for (int j = 0; j < 3; ++j) {
                m->fit_passive[(m->passive[e] * p + q) * 3 + j] += hard * psi[q * 3 + j];
                m->fit_whole[q * 3 + j] += hard * psi[q * 3 + j];
            }
        }
        /* Y: the deformation gradient that mode v gives this tetrahedron; the identity for the rest shape */
        for (int v = 0; v <= modes; ++v) {
            double y[3][3] = {{1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}};
            for (int i = 0; i < 3 && v < modes; ++i)
                for (int j = 0; j < 3; ++j) {
                    y[i][j] = 0.0;
                    for (int k = 0; k < 4; ++k) y[i][j] += m->shape[v * n3 + 3 * (int)tet[4 * e + k] + i] * m->grad[12 * e + 3 * k + j];
                }
            for (int q = 0; q < p; ++q)
                for (int i = 0; i < 3; ++i) m->fit_active[((m->active[e] * (modes + 1) + v) * p + q) * 3 + i] += strong * (psi[q * 3] * y[i][0] + psi[q * 3 + 1] * y[i][1] + psi[q * 3 + 2] * y[i][2]);
        }
    }
    matrix = (double *)malloc((size_t)p * (size_t)p * sizeof(double));
    memcpy(matrix, m->system, (size_t)p * (size_t)p * sizeof(double));
    for (int q = 0; q < p; ++q) m->inverse[q * p + q] = 1.0;
    solve(p, matrix, p, m->inverse);
    free(matrix);

    /* The ground samples: outside nodes, the lowest first, then each time the one farthest from those taken. */
    m->sample      = (int *)malloc((size_t)(setup->samples + 1) * sizeof(int));
    m->touch       = (double *)calloc((size_t)(setup->samples + 1) * (size_t)p, sizeof(double));
    m->touch_force = (double *)calloc((size_t)(setup->samples + 1) * (size_t)p, sizeof(double));
    m->touch_move  = (double *)calloc((size_t)(setup->samples + 1) * (size_t)p, sizeof(double));
    m->touching    = (bool *)calloc((size_t)(setup->samples + 1), sizeof(bool));
    outside        = (bool *)calloc((size_t)nodes, sizeof(bool));
    far            = (double *)malloc((size_t)nodes * sizeof(double));
    for (int f = 0; f < 3 * faces; ++f) outside[face[f]] = true;
    for (int i = 0; i < nodes; ++i) far[i] = outside[i] ? DBL_MAX : -1.0;
    for (int s = 0, next = -1; s < setup->samples; ++s) {
        int farthest = -1;
        for (int i = 0; i < nodes && next < 0; ++i) farthest = outside[i] && (farthest < 0 || rest[3 * i + 1] < rest[3 * farthest + 1]) ? i : farthest;
        next = next < 0 ? farthest : next;
        m->sample[s] = next;
        memcpy(&m->touch[s * p], &m->basis[next * p], (size_t)p * sizeof(double));
        farthest = next;
        for (int i = 0; i < nodes; ++i) {
            const double dx = rest[3 * i] - rest[3 * next], dy = rest[3 * i + 1] - rest[3 * next + 1], dz = rest[3 * i + 2] - rest[3 * next + 2];
            far[i] = fmin(far[i], dx * dx + dy * dy + dz * dz);
            farthest = far[i] > far[farthest] ? i : farthest;
        }
        next = farthest;
        for (int which = 0; which < 2; ++which) {
            const double *from = which ? &m->touch_force[s * p] : &m->touch[s * p];
            double       *to = which ? &m->touch_move[s * p] : &m->touch_force[s * p];
            for (int q = 0; q < p; ++q)
                for (int r = 0; r < p; ++r) to[q] += m->inverse[q * p + r] * from[r];
        }
    }
    free(outside); free(far);

    /* Rest: the first weight is the same number at every node; its map is the identity over that number. */
    m->start        = (double *)calloc(3u * (size_t)p, sizeof(double));
    m->z            = (double *)malloc(3u * (size_t)p * sizeof(double));
    m->before       = (double *)malloc(3u * (size_t)p * sizeof(double));
    m->turn_passive = (double *)malloc((size_t)setup->passive * 9u * sizeof(double));
    m->turn_active  = (double *)malloc((size_t)setup->active * 9u * sizeof(double));
    m->scratch      = (double *)malloc(((size_t)setup->active * (size_t)p * 3u + 12u * (size_t)p + (size_t)(setup->samples + p) * (size_t)(setup->samples + p + 4)) * sizeof(double));
    for (int a = 0; a < 3; ++a) m->start[a * p + a] = 1.0 / m->weight[0];
    am_reset(m);
    return m;
}

void am_close(Am *m) {
    free(m->mass); free(m->volume); free(m->grad); free(m->shape); free(m->stiff); free(m->weight); free(m->basis); free(m->strain);
    free(m->passive); free(m->active); free(m->sample); free(m->inertia); free(m->system); free(m->inverse); free(m->heavy);
    free(m->fit_passive); free(m->fit_whole); free(m->fit_active); free(m->pulled); free(m->touch); free(m->touch_force); free(m->touch_move);
    free(m->start); free(m->z); free(m->before); free(m->turn_passive); free(m->turn_active); free(m->touching); free(m->scratch);
    free(m);
}

void am_reset(Am *m) {
    memcpy(m->z, m->start, 3u * (size_t)m->p * sizeof(double));
    memcpy(m->before, m->start, 3u * (size_t)m->p * sizeof(double));
    memset(m->touching, 0, (size_t)m->setup.samples * sizeof(bool));
    m->contacts = 0;
}

void am_lift(const Am *m, const double *z, double *x) {
    const int p = m->p;
    for (int i = 0; i < m->nodes; ++i)
        for (int a = 0; a < 3; ++a) {
            double sum = 0.0;
            for (int q = 0; q < p; ++q) sum += z[a * p + q] * m->basis[i * p + q];
            x[3 * i + a] = sum;
        }
}

/* What a step's right side has before any rotation: inertia, gravity, the springs of `pull` (base, 3 x p); and
 * the tensor of eq. 13 contracted with [a; 1] (act, active x p x 3). */
static void drive(const Am *m, const double *a, double *act, double *base) {
    const int    p = m->p, modes = m->setup.modes;
    const double h = m->setup.step;

    memset(act, 0, (size_t)m->setup.active * (size_t)p * 3u * sizeof(double));
    for (int c = 0; c < m->setup.active; ++c)
        for (int v = 0; v <= modes; ++v) {
            const double *from = &m->fit_active[(c * (modes + 1) + v) * p * 3], much = v < modes ? a[v] : 1.0;
            for (int q = 0; q < 3 * p; ++q) act[c * p * 3 + q] += much * from[q];
        }
    for (int k = 0; k < 3; ++k)
        for (int q = 0; q < p; ++q) {
            double sum = k == 1 ? -m->setup.gravity * m->heavy[q] : 0.0;
            for (int r = 0; r < p; ++r) sum += (2.0 * m->z[k * p + r] - m->before[k * p + r]) * m->inertia[r * p + q] / (h * h);
            for (int v = 0; v <= modes && m->setup.pull; ++v) sum += m->spring * (v < modes ? a[v] : 1.0) * m->pulled[(v * 3 + k) * p + q];
            base[k * p + q] = sum;
        }
}

/* The local step (eq. 12, 13): the rotations that fit z, and the right side that they give the global step. */
static void local(const Am *m, const double *z, const double *act, const double *base, double *turn_passive, double *turn_active, double *right) {
    const int p = m->p;

    memcpy(right, base, 3u * (size_t)p * sizeof(double));
    for (int c = 0; c < m->setup.passive + (m->setup.pull ? 0 : m->setup.active); ++c) {
        const bool    elastic = c < m->setup.passive;
        const double *fit = elastic ? &m->fit_passive[c * p * 3] : &act[(c - m->setup.passive) * p * 3];
        double       *turn = elastic ? &turn_passive[9 * c] : &turn_active[9 * (c - m->setup.passive)], sum[9] = {0};
        for (int k = 0; k < 3; ++k)
            for (int q = 0; q < p; ++q)
                for (int j = 0; j < 3; ++j) sum[k * 3 + j] += z[k * p + q] * fit[q * 3 + j];
        am_polar(sum, turn);
        for (int k = 0; k < 3; ++k)
            for (int q = 0; q < p; ++q) right[k * p + q] += turn[k * 3] * fit[q * 3] + turn[k * 3 + 1] * fit[q * 3 + 1] + turn[k * 3 + 2] * fit[q * 3 + 2];
    }
}

void am_step(Am *m, const double *a) {
    const int p = m->p, samples = m->setup.samples, most = samples + p;
    double   *act = m->scratch, *base = act + m->setup.active * p * 3, *right = base + 3 * p, *loose = right + 3 * p, *next = loose + 3 * p;
    double   *gram = next + 3 * p, *held = gram + most * most;   /* held: 3 rows of `most` */
    int       list[512];

    drive(m, a, act, base);
    memcpy(next, m->z, 3u * (size_t)p * sizeof(double));
    for (int round = 0; round < m->setup.iterations; ++round) {
        /* local, then global with no ground: loose = right K^-1 */
        local(m, next, act, base, m->turn_passive, m->turn_active, right);
        for (int k = 0; k < 3; ++k)
            for (int q = 0; q < p; ++q) {
                double sum = 0.0;
                for (int r = 0; r < p; ++r) sum += right[k * p + r] * m->inverse[r * p + q];
                loose[k * p + q] = sum;
            }
        memcpy(next, loose, 3u * (size_t)p * sizeof(double));

        /* Appendix E. Samples that end below the ground are held; if holding them pushes others below, those are
         * held too. */
        m->contacts = 0;
        memset(m->touching, 0, (size_t)samples * sizeof(bool));
        for (int pass = 0; pass < 4; ++pass) {
            const int count = m->contacts;
            for (int s = 0; s < samples; ++s) {
                double height = 0.0;
                for (int q = 0; q < p; ++q) height += next[p + q] * m->touch[s * p + q];
                if (m->touching[s] || height >= (pass ? -1e-9 : 0.0)) continue;
                m->touching[s] = true;
                list[m->contacts++] = s;
            }
            if (m->contacts == count) break;
            /* what each held number must become, less what it is without ground */
            for (int k = 0; k < 3; ++k)
                for (int i = 0; i < m->contacts; ++i) {
                    const double *phi = &m->touch[list[i] * p];
                    double        now = 0.0, old = 0.0, is = 0.0;
                    for (int q = 0; q < p; ++q) { now += m->z[k * p + q] * phi[q]; old += m->before[k * p + q] * phi[q]; is += loose[k * p + q] * phi[q]; }
                    held[k * most + i] = (k == 1 ? 0.0 : now + m->setup.slide * (now - old)) - is;
                }
            if (m->contacts <= p) {
                /* eq. 52: the smallest force, f = L' (L L')^-1 b with the rows K^-1 Phi_s of L; it moves Z by f K^-1 */
                double trace = 0.0;
                for (int i = 0; i < m->contacts; ++i)
                    for (int j = 0; j < m->contacts; ++j) {
                        double dot = 0.0;
                        for (int q = 0; q < p; ++q) dot += m->touch_force[list[i] * p + q] * m->touch_force[list[j] * p + q];
                        gram[i * m->contacts + j] = dot;
                        trace += i == j ? dot : 0.0;
                    }
                for (int i = 0; i < m->contacts; ++i) gram[i * m->contacts + i] += 1e-10 * trace / m->contacts;
                for (int k = 0; k < 3; ++k) memmove(&held[k * m->contacts], &held[k * most], (size_t)m->contacts * sizeof(double));
                solve(m->contacts, gram, 3, held);
                for (int k = 0; k < 3; ++k)
                    for (int q = 0; q < p; ++q) {
                        double sum = loose[k * p + q];
                        for (int i = 0; i < m->contacts; ++i) sum += held[k * m->contacts + i] * m->touch_move[list[i] * p + q];
                        next[k * p + q] = sum;
                    }
            } else {
                /* eq. 51: more held numbers than unknowns; the force that comes nearest, L' L f = L' b */
                double trace = 0.0, *force = right;
                for (int q = 0; q < p; ++q)
                    for (int r = 0; r < p; ++r) {
                        double dot = 0.0;
                        for (int i = 0; i < m->contacts; ++i) dot += m->touch_force[list[i] * p + q] * m->touch_force[list[i] * p + r];
                        gram[q * p + r] = dot;
                        trace += q == r ? dot : 0.0;
                    }
                for (int q = 0; q < p; ++q) gram[q * p + q] += 1e-10 * trace / p;
                for (int k = 0; k < 3; ++k)
                    for (int q = 0; q < p; ++q) {
                        double sum = 0.0;
                        for (int i = 0; i < m->contacts; ++i) sum += m->touch_force[list[i] * p + q] * held[k * most + i];
                        force[k * p + q] = sum;
                    }
                solve(p, gram, 3, force);
                for (int k = 0; k < 3; ++k)
                    for (int q = 0; q < p; ++q) {
                        double sum = loose[k * p + q];
                        for (int r = 0; r < p; ++r) sum += force[k * p + r] * m->inverse[r * p + q];
                        next[k * p + q] = sum;
                    }
            }
        }
    }
    memcpy(m->before, m->z, 3u * (size_t)p * sizeof(double));
    memcpy(m->z, next, 3u * (size_t)p * sizeof(double));
}

double am_energy(const Am *m, const double *z, const double *a, double *slope) {
    const int p = m->p;
    double   *act = (double *)malloc(((size_t)m->setup.active * (size_t)p * 3u + 6u * (size_t)p + 9u * (size_t)(m->setup.passive + m->setup.active)) * sizeof(double));
    double   *base = act + m->setup.active * p * 3, *right = base + 3 * p, *turn = right + 3 * p, energy = 0.0;

    drive(m, a, act, base);
    local(m, z, act, base, turn, turn + 9 * m->setup.passive, right);
    /* 1/2 Z K Z' - Z : right; its derivative for fixed rotations, which are the best ones, is Z K - right */
    for (int k = 0; k < 3; ++k)
        for (int q = 0; q < p; ++q) {
            double sum = 0.0;
            for (int r = 0; r < p; ++r) sum += z[k * p + r] * m->system[r * p + q];
            energy += z[k * p + q] * (0.5 * sum - right[k * p + q]);
            if (slope) slope[k * p + q] = sum - right[k * p + q];
        }
    free(act);
    return energy;
}

void am_momentum(const Am *m, double linear[3], double angular[3]) {
    const int p = m->p;
    for (int k = 0; k < 3; ++k) {
        const int i = (k + 1) % 3, j = (k + 2) % 3;
        linear[k] = angular[k] = 0.0;
        for (int q = 0; q < p; ++q) {
            double vi = 0.0, vj = 0.0;
            for (int r = 0; r < p; ++r) {
                vi += m->inertia[q * p + r] * (m->z[i * p + r] - m->before[i * p + r]) / m->setup.step;
                vj += m->inertia[q * p + r] * (m->z[j * p + r] - m->before[j * p + r]) / m->setup.step;
            }
            linear[k] += m->heavy[q] * (m->z[k * p + q] - m->before[k * p + q]) / m->setup.step;
            angular[k] += m->z[i * p + q] * vj - m->z[j * p + q] * vi;
        }
    }
}

void am_wave(const double *gait, int modes, int waves, double t, double *a) {
    for (int i = 0; i < modes; ++i) {
        a[i] = 0.0;
        for (int j = 0; j < waves; ++j) {
            const double *g = &gait[3 * (i * waves + j)];
            a[i] += fmin(1.0, 2.0 * t) * g[0] * sin(6.283185307179586 * (t / g[1] + g[2]));
        }
    }
}

double am_episode(Am *m, const double *gait, int waves, int steps, double *way, double *facing) {
    const int p = m->p;
    double    a[64], from = 0.0, to = 0.0;

    am_reset(m);
    *facing = 1.0;
    for (int q = 0; q < p; ++q) from += m->z[q] * m->heavy[q] / m->total;
    for (int s = 0; s < steps; ++s) {
        double sum[9] = {0}, turn[9];
        am_wave(gait, m->setup.modes, waves, (s + 1) * m->setup.step, a);
        am_step(m, a);
        /* the body's forward direction: the x axis under the rotation that fits the whole body */
        for (int k = 0; k < 3; ++k)
            for (int q = 0; q < p; ++q)
                for (int j = 0; j < 3; ++j) sum[k * 3 + j] += m->z[k * p + q] * m->fit_whole[q * 3 + j];
        am_polar(sum, turn);
        *facing = fmin(*facing, turn[0] / (hypot(turn[0], turn[6]) + 1e-300));
    }
    for (int q = 0; q < p; ++q) to += m->z[q] * m->heavy[q] / m->total;
    *way = to - from;
    /* a gait that blows the simulation up counts as the worst */
    return isfinite(to) ? -*way * fmax(*facing, 0.0) : DBL_MAX;
}

AmSearch *am_search_open(int n, int lambda, const double *mean, double sigma, uint64_t seed) {
    AmSearch *s = (AmSearch *)calloc(1u, sizeof(AmSearch));
    s->n = n; s->lambda = lambda; s->sigma = sigma; s->random = seed;
    s->mean       = (double *)malloc((size_t)n * sizeof(double));
    s->cov        = (double *)calloc((size_t)n * (size_t)n, sizeof(double));
    s->axes       = (double *)calloc((size_t)n * (size_t)n, sizeof(double));
    s->length     = (double *)malloc((size_t)n * sizeof(double));
    s->path_sigma = (double *)calloc((size_t)n, sizeof(double));
    s->path_cov   = (double *)calloc((size_t)n, sizeof(double));
    s->ask        = (double *)malloc((size_t)lambda * (size_t)n * sizeof(double));
    s->unit       = (double *)malloc((size_t)lambda * (size_t)n * sizeof(double));
    memcpy(s->mean, mean, (size_t)n * sizeof(double));
    for (int i = 0; i < n; ++i) { s->cov[i * n + i] = s->axes[i * n + i] = 1.0; s->length[i] = 1.0; }
    return s;
}

void am_search_close(AmSearch *s) {
    free(s->mean); free(s->cov); free(s->axes); free(s->length); free(s->path_sigma); free(s->path_cov); free(s->ask); free(s->unit);
    free(s);
}

const double *am_search_ask(AmSearch *s) {
    const int n = s->n;
    for (int k = 0; k < s->lambda; ++k) {
        /* n normal numbers (splitmix64, Box-Muller), then x = mean + sigma B D z */
        for (int i = 0; i < n; ++i) {
            double uniform[2];
            for (int which = 0; which < 2; ++which) {
                uint64_t bits = (s->random += 0x9E3779B97F4A7C15ull);
                bits = (bits ^ (bits >> 30)) * 0xBF58476D1CE4E5B9ull;
                bits = (bits ^ (bits >> 27)) * 0x94D049BB133111EBull;
                uniform[which] = ((double)((bits ^ (bits >> 31)) >> 11) + 0.5) / 9007199254740992.0;
            }
            s->unit[k * n + i] = sqrt(-2.0 * log(uniform[0])) * cos(6.283185307179586 * uniform[1]);
        }
        for (int i = 0; i < n; ++i) {
            double sum = 0.0;
            for (int j = 0; j < n; ++j) sum += s->axes[i * n + j] * s->length[j] * s->unit[k * n + j];
            s->ask[k * n + i] = s->mean[i] + s->sigma * sum;
        }
    }
    return s->ask;
}

void am_search_tell(AmSearch *s, const double *cost) {
    /* Hansen's standard settings ("The CMA Evolution Strategy: A Tutorial"). */
    const int    n = s->n, lambda = s->lambda, mu = lambda / 2;
    const double expected = sqrt((double)n) * (1.0 - 1.0 / (4.0 * n) + 1.0 / (21.0 * n * n));
    double      *weight = (double *)malloc((size_t)mu * sizeof(double)), *step = (double *)calloc(2u * (size_t)n, sizeof(double)), *unit = step + n;
    double      *scratch = (double *)malloc((size_t)n * (size_t)n * sizeof(double));
    double       total = 0.0, squares = 0.0, effective, c_sigma, d_sigma, c_cov, c_one, c_mu, long_path = 0.0, stall;
    int         *order = (int *)malloc((size_t)lambda * sizeof(int));

    for (int i = 0; i < lambda; ++i) {
        int at = i;
        for (; at > 0 && cost[order[at - 1]] > cost[i]; --at) order[at] = order[at - 1];
        order[at] = i;
    }
    for (int i = 0; i < mu; ++i) { weight[i] = log(mu + 0.5) - log(i + 1.0); total += weight[i]; }
    for (int i = 0; i < mu; ++i) { weight[i] /= total; squares += weight[i] * weight[i]; }
    effective = 1.0 / squares;
    c_sigma   = (effective + 2.0) / (n + effective + 5.0);
    d_sigma   = 1.0 + 2.0 * fmax(0.0, sqrt((effective - 1.0) / (n + 1.0)) - 1.0) + c_sigma;
    c_cov     = (4.0 + effective / n) / (n + 4.0 + 2.0 * effective / n);
    c_one     = 2.0 / ((n + 1.3) * (n + 1.3) + effective);
    c_mu      = fmin(1.0 - c_one, 2.0 * (effective - 2.0 + 1.0 / effective) / ((n + 2.0) * (n + 2.0) + effective));

    /* the weighted mean step of the best half, in units of sigma (step) and before B D (unit) */
    for (int k = 0; k < mu; ++k)
        for (int i = 0; i < n; ++i) {
            step[i] += weight[k] * (s->ask[order[k] * n + i] - s->mean[i]) / s->sigma;
            unit[i] += weight[k] * s->unit[order[k] * n + i];
        }
    for (int i = 0; i < n; ++i) {
        double turned = 0.0;
        for (int j = 0; j < n; ++j) turned += s->axes[i * n + j] * unit[j];
        s->path_sigma[i] = (1.0 - c_sigma) * s->path_sigma[i] + sqrt(c_sigma * (2.0 - c_sigma) * effective) * turned;
        long_path += s->path_sigma[i] * s->path_sigma[i];
    }
    long_path = sqrt(long_path);
    stall     = long_path / sqrt(1.0 - pow(1.0 - c_sigma, 2.0 * (s->generation + 1))) / expected < 1.4 + 2.0 / (n + 1.0) ? 1.0 : 0.0;
    for (int i = 0; i < n; ++i) s->path_cov[i] = (1.0 - c_cov) * s->path_cov[i] + stall * sqrt(c_cov * (2.0 - c_cov) * effective) * step[i];
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) {
            double rank_mu = 0.0;
            for (int k = 0; k < mu; ++k) rank_mu += weight[k] * (s->ask[order[k] * n + i] - s->mean[i]) * (s->ask[order[k] * n + j] - s->mean[j]) / (s->sigma * s->sigma);
            s->cov[i * n + j] = (1.0 - c_one - c_mu) * s->cov[i * n + j] + c_one * (s->path_cov[i] * s->path_cov[j] + (1.0 - stall) * c_cov * (2.0 - c_cov) * s->cov[i * n + j]) + c_mu * rank_mu;
        }
    for (int i = 0; i < n; ++i) s->mean[i] += s->sigma * step[i];
    s->sigma *= exp(c_sigma / d_sigma * (long_path / expected - 1.0));
    memcpy(scratch, s->cov, (size_t)n * (size_t)n * sizeof(double));
    spectrum(n, scratch, s->axes, s->length);
    for (int i = 0; i < n; ++i) s->length[i] = sqrt(fmax(s->length[i], 1e-30));
    s->generation++;
    free(weight); free(step); free(scratch); free(order);
}
