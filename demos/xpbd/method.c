/* method.c — XPBD: position-based dynamics with compliant constraints.
 *
 * Macklin, Mueller, Chentanez: "XPBD: Position-Based Simulation of Compliant
 * Constrained Dynamics", Motion in Games 2016.
 *
 * A constraint C(x) = 0 with compliance alpha stands for the energy
 * C^2 / (2 alpha). Implicit Euler on that energy is the pair of equations
 *
 *     M (x - x~) - grad C^T lambda = 0          x~ = x_n + dt v_n + dt^2 g
 *     C(x) + alpha~ lambda = 0                  alpha~ = alpha / dt^2
 *
 * XPBD solves it one constraint at a time (eq. 18 and 17):
 *
 *     dlambda = (-C - alpha~ lambda) / (sum_i w_i |grad_i C|^2 + alpha~)
 *     x_i    += w_i grad_i C dlambda,      lambda += dlambda
 *
 * lambda starts each time step at zero and is kept, which is the whole
 * difference from PBD: the term alpha~ lambda stops the constraint from
 * being pulled tighter by every extra iteration.
 */
#include "method.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

double xpbd_value(const Xpbd *s, const XpbdConstraint *c, double grad[4][3]) {
    const double *p[4] = {&s->x[3u * c->node[0]], &s->x[3u * c->node[1]], &s->x[3u * c->node[2]], &s->x[3u * c->node[3]]};
    double        e[3][3], length;
    int           i, k;
    if (c->kind == XPBD_DISTANCE) {
        /* C = |p0 - p1| - rest */
        for (k = 0; k < 3; ++k) e[0][k] = p[0][k] - p[1][k];
        length = sqrt(e[0][0] * e[0][0] + e[0][1] * e[0][1] + e[0][2] * e[0][2]);
        for (k = 0; k < 3; ++k) {
            grad[0][k] = e[0][k] / (length + 1e-300);
            grad[1][k] = -grad[0][k];
            grad[2][k] = grad[3][k] = 0.0;
        }
        return length - c->rest;
    }
    /* C = (p1 - p0) x (p2 - p0) . (p3 - p0) / 6 - rest */
    for (i = 0; i < 3; ++i)
        for (k = 0; k < 3; ++k) e[i][k] = p[i + 1][k] - p[0][k];
    for (i = 0; i < 3; ++i) {
        const double *a = e[(i + 1) % 3], *b = e[(i + 2) % 3];
        grad[i + 1][0] = (a[1] * b[2] - a[2] * b[1]) / 6.0;
        grad[i + 1][1] = (a[2] * b[0] - a[0] * b[2]) / 6.0;
        grad[i + 1][2] = (a[0] * b[1] - a[1] * b[0]) / 6.0;
    }
    for (k = 0; k < 3; ++k) grad[0][k] = -grad[1][k] - grad[2][k] - grad[3][k];
    return e[0][0] * grad[1][0] + e[0][1] * grad[1][1] + e[0][2] * grad[1][2] - c->rest;
}

void xpbd_step(Xpbd *s) {
    uint32_t i, j, iteration;
    int      k, n;
    /* x~: where the nodes would go with no constraints */
    memcpy(s->before, s->x, (size_t)s->nodes * 3u * sizeof(double));
    for (i = 0u; i < s->nodes; ++i) {
        if (s->w[i] == 0.0) continue;
        s->v[3u * i + 1u] -= s->dt * s->gravity;
        for (k = 0; k < 3; ++k) s->x[3u * i + k] += s->dt * s->v[3u * i + k];
    }
    for (j = 0u; j < s->constraints; ++j) s->c[j].lambda = 0.0;

    for (iteration = 0u; iteration < s->iterations; ++iteration) {
        for (j = 0u; j < s->constraints; ++j) {
            XpbdConstraint *c = &s->c[j];
            const int       count = c->kind == XPBD_DISTANCE ? 2 : 4;
            double          grad[4][3], weighted = 0.0, moving = 0.0, dlambda;
            const double    value = xpbd_value(s, c, grad);
            const double    alpha = c->compliance / (s->dt * s->dt);
            /* gamma = alpha~ beta~ / dt with beta~ = dt^2 beta (eq. 26) */
            const double    gamma = alpha * s->damping * s->dt;
            for (n = 0; n < count; ++n)
                for (k = 0; k < 3; ++k) {
                    const uint32_t at = 3u * c->node[n] + (uint32_t)k;
                    weighted += s->w[c->node[n]] * grad[n][k] * grad[n][k];
                    moving   += grad[n][k] * (s->x[at] - s->before[at]);
                }
            if (weighted == 0.0) continue;
            dlambda = s->pbd ? -s->stiffness * value / weighted
                             : (-value - alpha * c->lambda - gamma * moving) / ((1.0 + gamma) * weighted + alpha);
            c->lambda += dlambda;
            for (n = 0; n < count; ++n)
                for (k = 0; k < 3; ++k) s->x[3u * c->node[n] + (uint32_t)k] += s->w[c->node[n]] * grad[n][k] * dlambda;
        }
        /* Contacts: constraints with no compliance that only push. Not part of the paper. */
        for (i = 0u; i < s->nodes; ++i) {
            double *x = &s->x[3u * i], d[3], distance, push = 0.0;
            if (s->w[i] == 0.0) continue;
            for (k = 0; k < 3; ++k) d[k] = x[k] - s->ball[k];
            distance = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (distance < s->ball[3]) {
                push = s->ball[3] - distance;
                for (k = 0; k < 3; ++k) x[k] += push * d[k] / (distance + 1e-300);
            }
            if (x[1] < s->floor) { push = s->floor - x[1]; x[1] = s->floor; }
            if (push > 0.0)
                for (k = 0; k < 3; k += 2) x[k] -= s->friction * (x[k] - s->before[3u * i + (uint32_t)k]);
        }
    }
    for (i = 0u; i < s->nodes * 3u; ++i) s->v[i] = (s->x[i] - s->before[i]) / s->dt;
}

void xpbd_reference_step(Xpbd *s) {
    const uint32_t size = s->nodes * 3u;
    double *target = (double *)malloc(size * sizeof(double)), *gradient = (double *)malloc(size * sizeof(double));
    double *hessian = (double *)malloc((size_t)size * size * sizeof(double));
    uint32_t i, j, r, col, newton;
    int      a, b, k, l;
    memcpy(s->before, s->x, size * sizeof(double));
    for (i = 0u; i < s->nodes; ++i) {
        if (s->w[i] > 0.0) s->v[3u * i + 1u] -= s->dt * s->gravity;
        for (k = 0; k < 3; ++k) target[3u * i + k] = s->x[3u * i + k] + (s->w[i] > 0.0 ? s->dt * s->v[3u * i + k] : 0.0);
    }
    memcpy(s->x, target, size * sizeof(double));
    for (newton = 0u; newton < 50u; ++newton) {
        double largest = 0.0;
        /* gradient and Hessian of  (x - x~)^T M (x - x~) / (2 dt^2) + sum C^2 / (2 alpha) */
        memset(hessian, 0, (size_t)size * size * sizeof(double));
        for (i = 0u; i < size; ++i) {
            const double w = s->w[i / 3u];
            gradient[i]                     = w > 0.0 ? (s->x[i] - target[i]) / (w * s->dt * s->dt) : 0.0;
            hessian[(size_t)i * size + i] = w > 0.0 ? 1.0 / (w * s->dt * s->dt) : 1.0;
        }
        for (j = 0u; j < s->constraints; ++j) {
            const XpbdConstraint *c = &s->c[j];
            double                grad[4][3];
            const double          value = xpbd_value(s, c, grad), length = value + c->rest;
            for (a = 0; a < 2; ++a)
                for (k = 0; k < 3; ++k) {
                    const uint32_t row = 3u * c->node[a] + (uint32_t)k;
                    if (s->w[c->node[a]] == 0.0) continue;
                    gradient[row] += value / c->compliance * grad[a][k];
                    for (b = 0; b < 2; ++b)
                        for (l = 0; l < 3; ++l) {
                            /* grad C grad C^T + C x (second derivative of a length) */
                            const double sign = a == b ? 1.0 : -1.0;
                            const double second = sign * ((k == l ? 1.0 : 0.0) - grad[0][k] * grad[0][l]) / length;
                            if (s->w[c->node[b]] == 0.0) continue;
                            hessian[(size_t)row * size + 3u * c->node[b] + (uint32_t)l] += (grad[a][k] * grad[b][l] + value * second) / c->compliance;
                        }
                }
        }
        /* solve hessian step = gradient by elimination with row exchange */
        for (col = 0u; col < size; ++col) {
            uint32_t pivot = col;
            for (r = col + 1u; r < size; ++r)
                if (fabs(hessian[(size_t)r * size + col]) > fabs(hessian[(size_t)pivot * size + col])) pivot = r;
            for (i = 0u; i < size; ++i) {
                const double swap = hessian[(size_t)col * size + i];
                hessian[(size_t)col * size + i]   = hessian[(size_t)pivot * size + i];
                hessian[(size_t)pivot * size + i] = swap;
            }
            { const double swap = gradient[col]; gradient[col] = gradient[pivot]; gradient[pivot] = swap; }
            for (r = col + 1u; r < size; ++r) {
                const double factor = hessian[(size_t)r * size + col] / hessian[(size_t)col * size + col];
                for (i = col; i < size; ++i) hessian[(size_t)r * size + i] -= factor * hessian[(size_t)col * size + i];
                gradient[r] -= factor * gradient[col];
            }
        }
        for (r = size; r-- > 0u;) {
            for (i = r + 1u; i < size; ++i) gradient[r] -= hessian[(size_t)r * size + i] * gradient[i];
            gradient[r] /= hessian[(size_t)r * size + r];
            s->x[r]     -= gradient[r];
            largest      = fmax(largest, fabs(gradient[r]));
        }
        if (largest < 1e-13) break;
    }
    for (i = 0u; i < size; ++i) s->v[i] = (s->x[i] - s->before[i]) / s->dt;
    free(target); free(gradient); free(hessian);
}
