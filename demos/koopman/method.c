/* method.c — "Low-Rank Koopman Deformables with Log-Linear Time Integration" (Yue Chang, Peter Yichen Chen,
 * Eitan Grinspun, Maurizio M. Chiaramonte; arXiv 2602.07687, 2026), the numerical variant of sections 3 and 4.
 *
 * An elastic body is simulated the usual way for a while. From the states it went through, one matrix K is fitted
 * that takes each state to the next (dynamic mode decomposition, section 3). K is kept as eigenvalues and modes,
 * so that many steps at once cost the same as one (section 4.3).
 *
 * The state (section 4.2, eq. 18) is X = [u; u - u_before]: the displacement of every node and its change over
 * the last step, 6 numbers a node. Here the second half is h * v, which is the same thing for implicit Euler.
 *
 * The fit (section 3). The states X_t that a step starts from are the columns of X, the states it ends in the
 * columns of Y. With the singular value decomposition X = U S V' cut to rank r (eq. 12):
 *
 *     K' = U_r' Y V_r S_r^-1          r x r                                          (eq. 13)
 *     K' = phi Lambda phi^-1          eigenvalues and eigenvectors, complex          (eq. 14)
 *     Phi = U_r phi,  Phi^* = phi^-1 U_r'                                            (eq. 15)
 *     X_next = Phi Lambda Phi^* X     project, advance, lift                         (eq. 17)
 *
 * A force (eq. 19) is a kick [0; h^2 a] added to the state before the step, a the acceleration it gives each
 * node. For implicit Euler that is exact: the step only sees the velocity it starts with, and h * (v + h a) is
 * that velocity with the force in it. So a forced step in the data is a pair (X_t + kick, X_t+1), and K is the
 * step with no force.
 *
 * Many steps (eq. 20): X_t+N = Phi Lambda^N Phi^* X_t. Lambda is diagonal, so the power is taken of r complex
 * numbers. Another step size (eq. 21): Lambda^(h'/h) = exp((h'/h) log Lambda). The same kick before each of N
 * steps adds (Lambda + ... + Lambda^N) Phi^* kick, a geometric sum in closed form (eq. 25).
 *
 * The body that makes the data (section 6): tetrahedra with the stable Neo-Hookean energy of Smith, De Goes and
 * Kim 2018,
 *
 *     Psi = mu/2 (I_C - 3) + lambda/2 (J - alpha)^2 - mu/2 log(I_C + 1),   alpha = 1 + 3 mu / (4 lambda)
 *
 * with I_C = |F|^2, J = det F, and mu = 4/3 mu_Lame, lambda = lambda_Lame + 5/6 mu_Lame so that small strains
 * give linear elasticity. One implicit Euler step is the minimum of
 *
 *     (1 + h c) / (2 h^2) |u' - u~|^2_M + Psi(u'),     u~ = u + h (v + h a) / (1 + h c)
 *
 * (c the damping), found by Newton's method with a line search. Each Newton step is solved by conjugate gradients
 * with the matrix at rest, factored once, as the preconditioner.
 *
 * Not here: the neural variant of section 5 (a network gives the modes at any point of any shape of a family).
 */
#include "method.h"

#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

KdBody *kd_body_open(int nodes, const double *rest, int tets, const uint32_t *tet, double young, double poisson, double density) {
    const double lame_mu = young / (2.0 * (1.0 + poisson)), lame_lambda = young * poisson / ((1.0 + poisson) * (1.0 - 2.0 * poisson));
    KdBody *b = (KdBody *)calloc(1u, sizeof(KdBody));

    b->nodes   = nodes;
    b->tets    = tets;
    b->tet     = tet;
    b->mu      = 4.0 / 3.0 * lame_mu;
    b->lambda  = lame_lambda + 5.0 / 6.0 * lame_mu;
    b->shape   = (double *)malloc((size_t)tets * 9u * sizeof(double));
    b->volume  = (double *)malloc((size_t)tets * sizeof(double));
    b->mass    = (double *)calloc((size_t)nodes, sizeof(double));
    b->fixed   = (bool *)calloc((size_t)nodes, sizeof(bool));
    b->rest    = (double *)malloc((size_t)nodes * 3u * sizeof(double));
    b->u       = (double *)calloc((size_t)nodes * 3u, sizeof(double));
    b->v       = (double *)calloc((size_t)nodes * 3u, sizeof(double));
    b->scratch = (double *)malloc((size_t)nodes * 30u * sizeof(double));
    memcpy(b->rest, rest, (size_t)nodes * 3u * sizeof(double));
    for (int t = 0; t < tets; ++t) {
        const uint32_t *n = &tet[4 * t];
        double         *s = &b->shape[9 * t], e[3][3], det;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) e[r][c] = rest[3u * n[c + 1] + r] - rest[3u * n[0] + r];
        det = e[0][0] * (e[1][1] * e[2][2] - e[1][2] * e[2][1]) - e[0][1] * (e[1][0] * e[2][2] - e[1][2] * e[2][0]) + e[0][2] * (e[1][0] * e[2][1] - e[1][1] * e[2][0]);
        assert(det > 0.0);
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) {
                const int r1 = (r + 1) % 3, r2 = (r + 2) % 3, c1 = (c + 1) % 3, c2 = (c + 2) % 3;
                s[3 * c + r] = (e[r1][c1] * e[r2][c2] - e[r1][c2] * e[r2][c1]) / det;
            }
        b->volume[t] = det / 6.0;
        for (int c = 0; c < 4; ++c) {
            b->mass[n[c]] += density * det / 24.0;
            for (int d = 0; d < 4; ++d) b->band = n[c] > n[d] && 3 * (int)(n[c] - n[d]) + 2 > b->band ? 3 * (int)(n[c] - n[d]) + 2 : b->band;
        }
    }

    /* The stiffness at rest, a column at a time: minus the change of the forces when one coordinate moves. This
     * walks all tetrahedra for every coordinate, which is fine for a few hundred nodes. */
    b->stiff  = (double *)calloc((size_t)nodes * 3u * (size_t)(b->band + 1), sizeof(double));
    b->factor = (double *)malloc((size_t)nodes * 3u * (size_t)(b->band + 1) * sizeof(double));
    for (int i = 0; i < 3 * nodes; ++i) {
        double *along = b->scratch, *force = along + 3 * nodes, *change = force + 3 * nodes;
        memset(along, 0, (size_t)nodes * 3u * sizeof(double));
        along[i] = 1.0;
        kd_body_energy(b, b->u, force, along, change);
        for (int j = i; j < 3 * nodes && j <= i + b->band; ++j) b->stiff[(size_t)j * (size_t)(b->band + 1) + (size_t)(i - j + b->band)] = -change[j];
    }
    return b;
}

void kd_body_close(KdBody *b) {
    free(b->shape); free(b->volume); free(b->mass); free(b->fixed);
    free(b->rest); free(b->u); free(b->v); free(b->scratch); free(b->stiff); free(b->factor);
    free(b);
}

double kd_body_energy(const KdBody *b, const double *u, double *force, const double *along, double *change) {
    const double alpha = 1.0 + 0.75 * b->mu / b->lambda;
    const double at_rest = 0.5 * b->lambda * (1.0 - alpha) * (1.0 - alpha) - 0.5 * b->mu * log(4.0);
    double energy = 0.0;

    if (force) memset(force, 0, (size_t)b->nodes * 3u * sizeof(double));
    if (change) memset(change, 0, (size_t)b->nodes * 3u * sizeof(double));
    for (int t = 0; t < b->tets; ++t) {
        const uint32_t *n = &b->tet[4 * t];
        const double   *s = &b->shape[9 * t];
        double          e[3][3], f[3][3], cof[3][3], p[3][3], size = 0.0, det, soft;

        /* The deformation gradient F = (edges now) (edges at rest)^-1, |F|^2, det F and its derivative. */
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) e[r][c] = b->rest[3u * n[c + 1] + r] + u[3u * n[c + 1] + r] - b->rest[3u * n[0] + r] - u[3u * n[0] + r];
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) {
                f[r][c] = e[r][0] * s[c] + e[r][1] * s[3 + c] + e[r][2] * s[6 + c];
                size += f[r][c] * f[r][c];
            }
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) {
                const int r1 = (r + 1) % 3, r2 = (r + 2) % 3, c1 = (c + 1) % 3, c2 = (c + 2) % 3;
                cof[r][c] = f[r1][c1] * f[r2][c2] - f[r1][c2] * f[r2][c1];
            }
        det  = f[0][0] * cof[0][0] + f[0][1] * cof[0][1] + f[0][2] * cof[0][2];
        soft = b->mu * (1.0 - 1.0 / (size + 1.0));
        energy += b->volume[t] * (0.5 * b->mu * (size - 3.0) + 0.5 * b->lambda * (det - alpha) * (det - alpha) - 0.5 * b->mu * log(size + 1.0) - at_rest);
        if (!force) continue;

        /* The stress P = dPsi/dF, and the forces -volume P (rest edges)^-T on corners 1, 2, 3. */
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) p[r][c] = soft * f[r][c] + b->lambda * (det - alpha) * cof[r][c];
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) {
                const double pull = -b->volume[t] * (p[r][0] * s[3 * c] + p[r][1] * s[3 * c + 1] + p[r][2] * s[3 * c + 2]);
                force[3u * n[c + 1] + r] += pull;
                force[3u * n[0] + r] -= pull;
            }
        if (!along) continue;

        /* The same for the change of P when the nodes move by `along`. */
        {
            double de[3][3], df[3][3], dcof[3][3], dp[3][3], with_f = 0.0, with_cof = 0.0;
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c) de[r][c] = along[3u * n[c + 1] + r] - along[3u * n[0] + r];
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c) {
                    df[r][c] = de[r][0] * s[c] + de[r][1] * s[3 + c] + de[r][2] * s[6 + c];
                    with_f += f[r][c] * df[r][c];
                    with_cof += cof[r][c] * df[r][c];
                }
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c) {
                    const int r1 = (r + 1) % 3, r2 = (r + 2) % 3, c1 = (c + 1) % 3, c2 = (c + 2) % 3;
                    dcof[r][c] = df[r1][c1] * f[r2][c2] + f[r1][c1] * df[r2][c2] - df[r1][c2] * f[r2][c1] - f[r1][c2] * df[r2][c1];
                }
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c)
                    dp[r][c] = soft * df[r][c] + 2.0 * b->mu * with_f / ((size + 1.0) * (size + 1.0)) * f[r][c] + b->lambda * with_cof * cof[r][c] + b->lambda * (det - alpha) * dcof[r][c];
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c) {
                    const double pull = -b->volume[t] * (dp[r][0] * s[3 * c] + dp[r][1] * s[3 * c + 1] + dp[r][2] * s[3 * c + 2]);
                    change[3u * n[c + 1] + r] += pull;
                    change[3u * n[0] + r] -= pull;
                }
        }
    }
    return energy;
}

void kd_body_step(KdBody *b, double h, const double *accel) {
    const int    n = 3 * b->nodes, wide = b->band + 1;
    const double weight = (1.0 + h * b->damping) / (h * h);
    double      *target = b->scratch, *next = target + n, *grad = next + n, *step = grad + n, *left = step + n;
    double      *dir = left + n, *image = dir + n, *trial = image + n, *force = trial + n, *eased = force + n;

    /* Cholesky factor of weight M + stiffness at rest, in its band; a fixed node's rows and columns are those of
     * the unit matrix. */
    if (weight != b->factor_weight) {
        b->factor_weight = weight;
        for (int i = 0; i < n; ++i)
            for (int j = i - b->band > 0 ? i - b->band : 0; j <= i; ++j) {
                const bool loose = !b->fixed[i / 3] && !b->fixed[j / 3];
                double     sum = i == j ? (loose ? weight * b->mass[i / 3] : 1.0) : 0.0;
                sum += loose ? b->stiff[i * wide + j - i + b->band] : 0.0;
                for (int e = i - b->band > 0 ? i - b->band : 0; e < j; ++e) sum -= b->factor[i * wide + e - i + b->band] * (e >= j - b->band ? b->factor[j * wide + e - j + b->band] : 0.0);
                assert(i != j || sum > 0.0);
                b->factor[i * wide + j - i + b->band] = i == j ? sqrt(sum) : sum / b->factor[j * wide + b->band];
            }
    }

    /* Where the nodes would go with no elastic force. */
    for (int i = 0; i < n; ++i) {
        const double start = b->v[i] + (accel ? h * accel[i] : 0.0);
        target[i] = next[i] = b->u[i] + (b->fixed[i / 3] ? 0.0 : h * start / (1.0 + h * b->damping));
    }
    b->newton = b->cg = 0;
    for (;;) {
        double energy = kd_body_energy(b, next, force, NULL, NULL), norm = 0.0, first, slope = 0.0, size = 0.0, scale = 2.0, tried;

        for (int i = 0; i < n; ++i) {
            const double away = next[i] - target[i];
            energy += 0.5 * weight * b->mass[i / 3] * away * away;
            grad[i] = b->fixed[i / 3] ? 0.0 : weight * b->mass[i / 3] * away - force[i];
            step[i] = 0.0;
            left[i] = -grad[i];
        }

        /* Conjugate gradients on (weight M + stiffness now) step = -grad, eased by the factor (two band
         * substitutions). A direction along which the energy curves down ends them. */
        for (int it = 0; it <= n; ++it) {
            double curve = 0.0, along, after = 0.0;
            for (int i = 0; i < n; ++i) {
                double sum = left[i];
                for (int e = i - b->band > 0 ? i - b->band : 0; e < i; ++e) sum -= b->factor[i * wide + e - i + b->band] * eased[e];
                eased[i] = sum / b->factor[i * wide + b->band];
            }
            for (int i = n - 1; i >= 0; --i) {
                eased[i] /= b->factor[i * wide + b->band];
                for (int e = i - b->band > 0 ? i - b->band : 0; e < i; ++e) eased[e] -= b->factor[i * wide + e - i + b->band] * eased[i];
            }
            for (int i = 0; i < n; ++i) after += left[i] * eased[i];
            first = it ? first : after;
            if (after <= 1e-20 * first) break;
            for (int i = 0; i < n; ++i) dir[i] = eased[i] + (it ? after / norm * dir[i] : 0.0);
            norm = after;
            kd_body_energy(b, next, force, dir, image);
            for (int i = 0; i < n; ++i) {
                image[i] = b->fixed[i / 3] ? 0.0 : weight * b->mass[i / 3] * dir[i] - image[i];
                curve += dir[i] * image[i];
            }
            if (curve <= 0.0) {
                if (it == 0) memcpy(step, dir, (size_t)n * sizeof(double));
                break;
            }
            along = norm / curve;
            for (int i = 0; i < n; ++i) {
                step[i] += along * dir[i];
                left[i] -= along * image[i];
            }
            b->cg++;
        }
        for (int i = 0; i < n; ++i) {
            slope += grad[i] * step[i];
            size = fmax(size, fabs(step[i]));
        }

        /* Halve the step until the energy has gone down by a share of what the slope promises. A step below
         * 1e-12 m is taken as it is and ends the iteration. */
        do {
            scale *= 0.5;
            for (int i = 0; i < n; ++i) trial[i] = next[i] + scale * step[i];
            tried = size < 1e-12 ? -DBL_MAX : kd_body_energy(b, trial, NULL, NULL, NULL);
            for (int i = 0; i < n && size >= 1e-12; ++i) tried += 0.5 * weight * b->mass[i / 3] * (trial[i] - target[i]) * (trial[i] - target[i]);
        } while (tried > energy + 1e-4 * scale * slope && scale > 1e-9);
        memcpy(next, trial, (size_t)n * sizeof(double));
        b->newton++;
        if (size < 1e-12 || b->newton == 60) break;
    }
    for (int i = 0; i < n; ++i) {
        b->v[i] = (next[i] - b->u[i]) / h;
        b->u[i] = next[i];
    }
}

/* Eigenvalues and eigenvectors of the real n x n matrix a (row after row): a vec = vec diag(lambda), the
 * eigenvectors in the columns of vec (n x n, row after row), each of length 1. Returns the QR sweeps. */
static int eigen(int n, const double *a, double complex *lambda, double complex *vec) {
    double complex *h = (double complex *)malloc((size_t)n * (size_t)n * sizeof(double complex));
    double complex *q = (double complex *)calloc((size_t)n * (size_t)n, sizeof(double complex));
    double complex *w = (double complex *)malloc((size_t)n * 2u * sizeof(double complex));
    double          total = 0.0;
    int             sweeps = 0, stall = 0, hi = n - 1;

    for (int i = 0; i < n * n; ++i) {
        h[i] = a[i];
        total += a[i] * a[i];
    }
    for (int i = 0; i < n; ++i) q[i * n + i] = 1.0;

    /* Hessenberg form: a reflection for each column clears it below the subdiagonal. q collects them: a = q h q^*. */
    for (int k = 0; k + 2 < n; ++k) {
        double below = 0.0, length = 0.0;
        for (int i = k + 1; i < n; ++i) below += creal(h[i * n + k] * conj(h[i * n + k]));
        if (below == 0.0) continue;
        for (int i = k + 1; i < n; ++i) w[i] = h[i * n + k];
        w[k + 1] += sqrt(below) * (cabs(w[k + 1]) > 0.0 ? w[k + 1] / cabs(w[k + 1]) : 1.0);
        for (int i = k + 1; i < n; ++i) length += creal(w[i] * conj(w[i]));
        for (int j = 0; j < n; ++j) {
            double complex dot = 0.0;
            for (int i = k + 1; i < n; ++i) dot += conj(w[i]) * h[i * n + j];
            for (int i = k + 1; i < n; ++i) h[i * n + j] -= 2.0 / length * w[i] * dot;
        }
        for (int i = 0; i < n; ++i) {
            double complex dot = 0.0, turn = 0.0;
            for (int j = k + 1; j < n; ++j) {
                dot += h[i * n + j] * w[j];
                turn += q[i * n + j] * w[j];
            }
            for (int j = k + 1; j < n; ++j) {
                h[i * n + j] -= 2.0 / length * dot * conj(w[j]);
                q[i * n + j] -= 2.0 / length * turn * conj(w[j]);
            }
        }
    }

    /* Shifted QR sweeps on the block lo..hi that is not yet triangular, until h is triangular (Schur form). */
    while (hi > 0) {
        double complex shift, half, root;
        int            lo = hi;
        while (lo > 0 && cabs(h[lo * n + lo - 1]) > DBL_EPSILON * (cabs(h[(lo - 1) * n + lo - 1]) + cabs(h[lo * n + lo]))) --lo;
        if (lo == hi) {
            h[hi * n + hi - 1] = 0.0;
            --hi;
            stall = 0;
            continue;
        }
        /* Wilkinson's shift: the eigenvalue of the last 2 x 2 block nearer its last entry. Every tenth sweep
         * on the same block takes another one, which breaks cycles. */
        half  = 0.5 * (h[(hi - 1) * n + hi - 1] + h[hi * n + hi]);
        root  = csqrt(0.25 * (h[(hi - 1) * n + hi - 1] - h[hi * n + hi]) * (h[(hi - 1) * n + hi - 1] - h[hi * n + hi]) + h[(hi - 1) * n + hi] * h[hi * n + hi - 1]);
        shift = cabs(half + root - h[hi * n + hi]) < cabs(half - root - h[hi * n + hi]) ? half + root : half - root;
        if (stall % 10 == 9) shift = h[hi * n + hi] + cabs(h[hi * n + hi - 1]);
        for (int i = lo; i <= hi; ++i) h[i * n + i] -= shift;
        for (int k = lo; k < hi; ++k) {
            const double         length = hypot(cabs(h[k * n + k]), cabs(h[(k + 1) * n + k]));
            const double complex c = length > 0.0 ? h[k * n + k] / length : 1.0, s = length > 0.0 ? h[(k + 1) * n + k] / length : 0.0;
            w[2 * k] = c; w[2 * k + 1] = s;
            for (int j = k; j < n; ++j) {
                const double complex up = h[k * n + j], down = h[(k + 1) * n + j];
                h[k * n + j]       = conj(c) * up + conj(s) * down;
                h[(k + 1) * n + j] = -s * up + c * down;
            }
        }
        for (int k = lo; k < hi; ++k) {
            const double complex c = w[2 * k], s = w[2 * k + 1];
            for (int i = 0; i <= (k + 2 < hi ? k + 2 : hi); ++i) {
                const double complex one = h[i * n + k], two = h[i * n + k + 1];
                h[i * n + k]     = one * c + two * s;
                h[i * n + k + 1] = -one * conj(s) + two * conj(c);
            }
            for (int i = 0; i < n; ++i) {
                const double complex one = q[i * n + k], two = q[i * n + k + 1];
                q[i * n + k]     = one * c + two * s;
                q[i * n + k + 1] = -one * conj(s) + two * conj(c);
            }
        }
        for (int i = lo; i <= hi; ++i) h[i * n + i] += shift;
        ++sweeps;
        ++stall;
        assert(stall < 200);
    }

    /* Eigenvector k of the triangle by back substitution, turned back by q. Equal eigenvalues are held apart by
     * a rounding error's worth. */
    for (int k = 0; k < n; ++k) {
        double length = 0.0;
        lambda[k] = h[k * n + k];
        w[k] = 1.0;
        for (int i = k - 1; i >= 0; --i) {
            double complex sum = 0.0, gap = h[i * n + i] - h[k * n + k];
            for (int j = i + 1; j <= k; ++j) sum += h[i * n + j] * w[j];
            if (cabs(gap) < DBL_EPSILON * sqrt(total)) gap = DBL_EPSILON * sqrt(total);
            w[i] = -sum / gap;
        }
        for (int i = 0; i < n; ++i) {
            double complex sum = 0.0;
            for (int j = 0; j <= k; ++j) sum += q[i * n + j] * w[j];
            vec[i * n + k] = sum;
            length += creal(sum * conj(sum));
        }
        for (int i = 0; i < n; ++i) vec[i * n + k] /= sqrt(length);
    }
    free(h); free(q); free(w);
    return sweeps;
}

Kd *kd_open(int dim, int pairs, const double *x, const double *y) {
    const size_t rows = (size_t)dim, cols = (size_t)pairs;
    Kd          *k = (Kd *)calloc(1u, sizeof(Kd));
    double      *mirror = (double *)malloc(rows * cols * sizeof(double));   /* X, then the reflections and R */
    double      *small = (double *)calloc(cols * cols, sizeof(double));     /* R, then its U S */
    int          turned, sweeps = 0;

    assert(dim >= pairs);
    k->dim   = dim;
    k->pairs = pairs;
    k->left  = (double *)calloc(rows * cols, sizeof(double));
    k->y     = (double *)malloc(rows * cols * sizeof(double));
    k->right = (double *)calloc(cols * cols, sizeof(double));
    k->sigma = (double *)malloc(cols * sizeof(double));
    memcpy(mirror, x, rows * cols * sizeof(double));
    memcpy(k->y, y, rows * cols * sizeof(double));

    /* X = Q R by reflections: reflection j clears column j below the diagonal. Its vector stays in that place. */
    for (size_t j = 0; j < cols; ++j) {
        double *v = &mirror[j * rows], length = 0.0, twice;
        for (size_t i = j; i < rows; ++i) length += v[i] * v[i];
        small[j * cols + j] = -copysign(sqrt(length), v[j]);
        for (size_t i = 0; i < j; ++i) small[j * cols + i] = v[i];
        v[j] -= small[j * cols + j];
        twice = 1.0 / (length - small[j * cols + j] * (v[j] + small[j * cols + j]));   /* 2 / |v|^2 */
        for (size_t c = j + 1; c < cols && length > 0.0; ++c) {
            double *a = &mirror[c * rows], dot = 0.0;
            for (size_t i = j; i < rows; ++i) dot += v[i] * a[i];
            for (size_t i = j; i < rows; ++i) a[i] -= twice * dot * v[i];
        }
    }

    /* One-sided Jacobi on R: turn two columns in their plane so that they become orthogonal, and V with them,
     * until all pairs are. The columns are then (U of R) S. */
    for (size_t t = 0; t < cols; ++t) k->right[t * cols + t] = 1.0;
    do {
        turned = 0;
        for (size_t p = 0; p + 1 < cols; ++p)
            for (size_t q = p + 1; q < cols; ++q) {
                double *a = &small[p * cols], *b = &small[q * cols], *c = &k->right[p * cols], *d = &k->right[q * cols];
                double  aa = 0.0, bb = 0.0, ab = 0.0, zeta, tangent, cosine, sine;
                for (size_t i = 0; i < cols; ++i) {
                    aa += a[i] * a[i];
                    bb += b[i] * b[i];
                    ab += a[i] * b[i];
                }
                if (fabs(ab) <= 1e-14 * sqrt(aa * bb)) continue;
                zeta    = 0.5 * (bb - aa) / ab;
                tangent = copysign(1.0, zeta) / (fabs(zeta) + sqrt(1.0 + zeta * zeta));
                cosine  = 1.0 / sqrt(1.0 + tangent * tangent);
                sine    = cosine * tangent;
                for (size_t i = 0; i < cols; ++i) {
                    const double one = a[i], two = b[i], three = c[i], four = d[i];
                    a[i] = cosine * one - sine * two;
                    b[i] = sine * one + cosine * two;
                    c[i] = cosine * three - sine * four;
                    d[i] = sine * three + cosine * four;
                }
                ++turned;
            }
    } while (turned && ++sweeps < 60);

    /* The lengths of the columns are the singular values. Largest first; U = Q (U of R), the reflections applied
     * last to first. */
    for (size_t t = 0; t < cols; ++t) {
        double sum = 0.0;
        for (size_t i = 0; i < cols; ++i) sum += small[t * cols + i] * small[t * cols + i];
        k->sigma[t] = sqrt(sum);
    }
    for (size_t t = 0; t < cols; ++t) {
        size_t  best = t;
        double  swap, *u = &k->left[t * rows];
        for (size_t s = t + 1; s < cols; ++s)
            if (k->sigma[s] > k->sigma[best]) best = s;
        swap = k->sigma[t]; k->sigma[t] = k->sigma[best]; k->sigma[best] = swap;
        for (size_t i = 0; i < cols; ++i) {
            swap = small[t * cols + i]; small[t * cols + i] = small[best * cols + i]; small[best * cols + i] = swap;
            swap = k->right[t * cols + i]; k->right[t * cols + i] = k->right[best * cols + i]; k->right[best * cols + i] = swap;
            u[i] = small[t * cols + i] / (k->sigma[t] > 0.0 ? k->sigma[t] : 1.0);
        }
        for (size_t j = cols; j-- > 0;) {
            const double *v = &mirror[j * rows];
            double        dot = 0.0, length = 0.0;
            for (size_t i = j; i < rows; ++i) {
                dot += v[i] * u[i];
                length += v[i] * v[i];
            }
            for (size_t i = j; i < rows && length > 0.0; ++i) u[i] -= 2.0 * dot / length * v[i];
        }
    }
    free(mirror); free(small);
    return k;
}

void kd_close(Kd *k) {
    free(k->left); free(k->right); free(k->sigma); free(k->y);
    free(k->reduced); free(k->lambda); free(k->modes); free(k->inverse);
    free(k);
}

void kd_rank(Kd *k, int rank) {
    const size_t    r = (size_t)rank, dim = (size_t)k->dim;
    double         *image = (double *)calloc(dim * r, sizeof(double));
    double complex *work = (double complex *)malloc(r * r * sizeof(double complex));

    assert(rank >= 1 && rank <= k->pairs && rank <= k->dim && k->sigma[rank - 1] > 0.0);
    free(k->reduced); free(k->lambda); free(k->modes); free(k->inverse);
    k->rank    = rank;
    k->reduced = (double *)malloc(r * r * sizeof(double));
    k->lambda  = (double complex *)malloc(r * sizeof(double complex));
    k->modes   = (double complex *)malloc(r * r * sizeof(double complex));
    k->inverse = (double complex *)calloc(r * r, sizeof(double complex));

    /* K' = U_r' (Y V_r S_r^-1) */
    for (size_t j = 0; j < r; ++j)
        for (size_t t = 0; t < (size_t)k->pairs; ++t) {
            const double weight = k->right[j * (size_t)k->pairs + t] / k->sigma[j];
            for (size_t i = 0; i < dim; ++i) image[j * dim + i] += weight * k->y[t * dim + i];
        }
    for (size_t i = 0; i < r; ++i)
        for (size_t j = 0; j < r; ++j) {
            double sum = 0.0;
            for (size_t e = 0; e < dim; ++e) sum += k->left[i * dim + e] * image[j * dim + e];
            k->reduced[i * r + j] = sum;
        }
    k->sweeps = eigen(rank, k->reduced, k->lambda, k->modes);

    /* The inverse of the eigenvector matrix by Gauss-Jordan elimination with row pivoting. */
    memcpy(work, k->modes, r * r * sizeof(double complex));
    for (size_t i = 0; i < r; ++i) k->inverse[i * r + i] = 1.0;
    for (size_t c = 0; c < r; ++c) {
        size_t         best = c;
        double complex pivot;
        for (size_t i = c + 1; i < r; ++i)
            if (cabs(work[i * r + c]) > cabs(work[best * r + c])) best = i;
        for (size_t j = 0; j < r; ++j) {
            double complex swap = work[c * r + j]; work[c * r + j] = work[best * r + j]; work[best * r + j] = swap;
            swap = k->inverse[c * r + j]; k->inverse[c * r + j] = k->inverse[best * r + j]; k->inverse[best * r + j] = swap;
        }
        pivot = work[c * r + c];
        for (size_t j = 0; j < r; ++j) {
            work[c * r + j] /= pivot;
            k->inverse[c * r + j] /= pivot;
        }
        for (size_t i = 0; i < r; ++i) {
            const double complex factor = i == c ? 0.0 : work[i * r + c];
            for (size_t j = 0; j < r; ++j) {
                work[i * r + j] -= factor * work[c * r + j];
                k->inverse[i * r + j] -= factor * k->inverse[c * r + j];
            }
        }
    }
    free(image); free(work);
}

void kd_project(const Kd *k, const double *x, double complex *z) {
    const size_t r = (size_t)k->rank, dim = (size_t)k->dim;
    memset(z, 0, r * sizeof(double complex));
    for (size_t j = 0; j < r; ++j) {
        double sum = 0.0;
        for (size_t i = 0; i < dim; ++i) sum += k->left[j * dim + i] * x[i];
        for (size_t i = 0; i < r; ++i) z[i] += k->inverse[i * r + j] * sum;
    }
}

double kd_lift(const Kd *k, const double complex *z, double *x) {
    const size_t r = (size_t)k->rank, dim = (size_t)k->dim;
    double       unreal = 0.0;
    memset(x, 0, dim * sizeof(double));
    for (size_t j = 0; j < r; ++j) {
        double complex sum = 0.0;
        for (size_t i = 0; i < r; ++i) sum += k->modes[j * r + i] * z[i];
        for (size_t i = 0; i < dim; ++i) x[i] += k->left[j * dim + i] * creal(sum);
        unreal = fmax(unreal, fabs(cimag(sum)));
    }
    return unreal;
}

void kd_advance(const Kd *k, double steps, const double complex *kick, double complex *z) {
    for (int i = 0; i < k->rank; ++i) {
        const double complex l = k->lambda[i], power = cabs(l) > 0.0 ? cexp(steps * clog(l)) : 0.0;
        z[i] *= power;
        if (kick) z[i] += kick[i] * (cabs(l - 1.0) > 1e-12 ? l * (power - 1.0) / (l - 1.0) : steps);
    }
}

void kd_real(const Kd *k, double steps, double *out) {
    const size_t    r = (size_t)k->rank;
    double complex *z = (double complex *)malloc(r * 2u * sizeof(double complex));

    /* Column by column: what the operator does to a z that is 1 or i in one place. */
    for (size_t column = 0; column < 2u * r; ++column) {
        memset(z, 0, r * sizeof(double complex));
        z[column % r] = column < r ? 1.0 : I;
        kd_advance(k, steps, NULL, z);
        for (size_t i = 0; i < r; ++i) {
            double complex sum = 0.0;
            for (size_t j = 0; j < r; ++j) sum += k->modes[i * r + j] * z[j];
            z[r + i] = creal(sum);
        }
        for (size_t i = 0; i < r; ++i) {
            double complex sum = 0.0;
            for (size_t j = 0; j < r; ++j) sum += k->inverse[i * r + j] * z[r + j];
            out[i * 2u * r + column]       = creal(sum);
            out[(r + i) * 2u * r + column] = cimag(sum);
        }
    }
    free(z);
}

double kd_control(const Kd *k, double steps, const double complex *z, int count, const double *kicks, int picks, const int *pick, const double *goal, double still, double *c) {
    const size_t    r = (size_t)k->rank, dim = (size_t)k->dim, n = (size_t)count;
    double complex *w = (double complex *)calloc(r, sizeof(double complex));
    double         *end = (double *)malloc((n + 1u) * dim * sizeof(double));
    double         *m = (double *)calloc(n * (n + 1u), sizeof(double));
    double          miss = 0.0;

    /* The end state is end[count] + sum of c[j] end[j]: what the start alone leads to, and what each control adds. */
    if (z) memcpy(w, z, r * sizeof(double complex));
    kd_advance(k, steps, NULL, w);
    kd_lift(k, w, &end[n * dim]);
    for (size_t j = 0; j < n; ++j) {
        double complex *kick = (double complex *)malloc(r * sizeof(double complex));
        kd_project(k, &kicks[j * dim], kick);
        memset(w, 0, r * sizeof(double complex));
        kd_advance(k, steps, kick, w);
        kd_lift(k, w, &end[j * dim]);
        free(kick);
    }

    /* Normal equations, row after row with the right side as the last column, then elimination. */
    for (size_t i = 0; i < n; ++i)
        for (size_t j = 0; j <= n; ++j) {
            double sum = 0.0;
            for (int p = 0; p < picks; ++p) sum += end[i * dim + (size_t)pick[p]] * (j < n ? end[j * dim + (size_t)pick[p]] : goal[p] - end[n * dim + (size_t)pick[p]]);
            for (size_t e = dim / 2u; e < dim; ++e) sum += still * end[i * dim + e] * (j < n ? end[j * dim + e] : -end[n * dim + e]);
            m[i * (n + 1u) + j] = sum;
        }
    for (size_t i = 0; i < n; ++i)
        for (size_t row = 0; row < n; ++row) {
            const double factor = row == i ? 0.0 : m[row * (n + 1u) + i] / m[i * (n + 1u) + i];
            for (size_t j = 0; j <= n; ++j) m[row * (n + 1u) + j] -= factor * m[i * (n + 1u) + j];
        }
    for (size_t i = 0; i < n; ++i) c[i] = m[i * (n + 1u) + n] / m[i * (n + 1u) + i];
    for (int p = 0; p < picks; ++p) {
        double at = end[n * dim + (size_t)pick[p]];
        for (size_t j = 0; j < n; ++j) at += c[j] * end[j * dim + (size_t)pick[p]];
        miss += (at - goal[p]) * (at - goal[p]);
    }
    free(w); free(end); free(m);
    return sqrt(miss / picks);
}
