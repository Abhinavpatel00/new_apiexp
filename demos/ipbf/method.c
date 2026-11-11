/* method.c — Diaz, Hsu, Montalvo-Ruiz, Giles, Yuksel: "Implicit Position-Based Fluids"
 * (SIGGRAPH Asia 2025), Algorithm 1, in the plane.
 *
 * A time step minimises (eq. 7)
 *
 *     alpha / (2 h^2) |x - y|^2_M  +  1/2 sum_i C_i(x)^2,      C_i = density_i / rest - 1,
 *
 * with y where the particles would go without pressure (eq. 3) and C clamped at zero where the
 * density is below rest. Each particle takes a Newton step on this energy with all others held
 * (eq. 10, 11, 14), all particles at once, and half of the step is applied (section 3.3). The
 * part of the Hessian that can be indefinite is replaced by a diagonal of the norms of its
 * columns (section 3.5). alpha = 0 leaves only the density term. The damping of section 3.6
 * compares the result with one more compliant solution and takes kinetic energy out (eq. 16-18).
 *
 * The comparison method is Macklin and Mueller, "Position Based Fluids" (2013), eq. 1, 2, 8, 11,
 * 12 and Algorithm 1, without its tensile correction, vorticity confinement and viscosity, and
 * with the same kernel and the same clamp as above.
 *
 * Not in either paper as far as they were read: the walls (three layers of fixed particles that
 * add to the density, and positions clamped to the box).
 */
#include "method.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.14159265358979323846

/* The cubic spline in the form of the SPH tutorial of Koschier et al. that the paper cites, in
 * two dimensions: W = 40 / (7 pi R^2) f(r / R), f = 6 (q^3 - q^2) + 1 up to q = 1/2 and
 * 2 (1 - q)^3 up to 1. (The form is the one this author knows; the tutorial was not reread.
 * The check integrates it to 1.) */
void fl_kernel(const Fl *s, double r, double w[3]) {
    const double R = s->radius, q = r / R, norm = 40.0 / (7.0 * PI * R * R);
    w[0] = w[1] = w[2] = 0.0;
    if (q >= 1.0) return;
    if (q <= 0.5) {
        w[0] = norm * (6.0 * (q * q * q - q * q) + 1.0);
        w[1] = norm * 6.0 * (3.0 * q * q - 2.0 * q) / R;
        w[2] = norm * 6.0 * (6.0 * q - 2.0) / (R * R);
        return;
    }
    w[0] = norm * 2.0 * (1.0 - q) * (1.0 - q) * (1.0 - q);
    w[1] = -norm * 6.0 * (1.0 - q) * (1.0 - q) / R;
    w[2] = norm * 12.0 * (1.0 - q) / (R * R);
}

void fl_close(Fl *s) {
    free(s->x); free(s->v); free(s->y); free(s->guess); free(s->next); free(s->other); free(s->c); free(s->lambda);
    free(s->list); free(s->count); free(s->cell); free(s->order);
    memset(s, 0, sizeof *s);
}

static void reserve(Fl *s, uint32_t total) {
    if (total <= s->capacity) return;
    s->capacity = 2u * total;
    s->x      = (double *)realloc(s->x, 2u * s->capacity * sizeof(double));
    s->v      = (double *)realloc(s->v, 2u * s->capacity * sizeof(double));
    s->y      = (double *)realloc(s->y, 2u * s->capacity * sizeof(double));
    s->guess  = (double *)realloc(s->guess, 2u * s->capacity * sizeof(double));
    s->next   = (double *)realloc(s->next, 2u * s->capacity * sizeof(double));
    s->other  = (double *)realloc(s->other, 2u * s->capacity * sizeof(double));
    s->c      = (double *)realloc(s->c, s->capacity * sizeof(double));
    s->lambda = (double *)realloc(s->lambda, s->capacity * sizeof(double));
    s->list   = (uint32_t *)realloc(s->list, (size_t)s->capacity * FL_NEIGHBOURS * sizeof(uint32_t));
    s->count  = (uint32_t *)realloc(s->count, s->capacity * sizeof(uint32_t));
    s->order  = (uint32_t *)realloc(s->order, s->capacity * sizeof(uint32_t));
}

void fl_open(Fl *s, double width, double height, double spacing) {
    const int across = (int)floor(width / spacing + 0.5), up = (int)floor(height / spacing + 0.5);
    double    sum = 0.0, w[3];
    int       i, j;
    fl_close(s);
    s->spacing = spacing; s->radius = 2.0 * spacing; s->rest = 1.0;
    s->box[0] = width; s->box[1] = height;
    s->h = 1.0 / 240.0; s->gravity = 9.81; s->iterations = 4u; s->method = FL_IPBF;
    s->compliance = 0.0; s->soft = 1.0; s->beta = 60.0; s->exact = false; s->damping = true;
    s->relaxation = 0.01;
    /* the grid of cells one kernel radius wide covers the box and the walls */
    s->cells[0] = (uint32_t)ceil((width + 6.0 * spacing) / s->radius) + 1u;
    s->cells[1] = (uint32_t)ceil((height + 6.0 * spacing) / s->radius) + 1u;
    s->cell     = (uint32_t *)malloc(((size_t)s->cells[0] * s->cells[1] + 1u) * sizeof(uint32_t));
    /* the mass that gives a square lattice the rest density, and the size of its pressure Hessian */
    for (j = -2; j <= 2; ++j)
        for (i = -2; i <= 2; ++i) {
            fl_kernel(s, spacing * sqrt((double)(i * i + j * j)), w);
            sum      += w[0];
            s->scale += w[1] * w[1];
        }
    s->mass   = s->rest / sum;
    s->scale *= (s->mass / s->rest) * (s->mass / s->rest);
    /* walls: three layers outside each side, the first half a spacing out */
    for (j = -3; j < up + 3; ++j)
        for (i = -3; i < across + 3; ++i) {
            if (i >= 0 && i < across && j >= 0 && j < up) continue;
            reserve(s, s->walls + 1u);
            s->x[2u * s->walls] = (i + 0.5) * spacing; s->x[2u * s->walls + 1u] = (j + 0.5) * spacing;
            s->v[2u * s->walls] = s->v[2u * s->walls + 1u] = 0.0;
            s->walls++;
        }
}

void fl_block(Fl *s, double x0, double y0, double x1, double y1, double step) {
    const int across = (int)floor((x1 - x0) / step + 0.5), up = (int)floor((y1 - y0) / step + 0.5), added = across * up;
    int       i, j;
    reserve(s, s->fluid + s->walls + (uint32_t)added);
    memmove(s->x + 2u * (s->fluid + (uint32_t)added), s->x + 2u * s->fluid, 2u * s->walls * sizeof(double));
    memmove(s->v + 2u * (s->fluid + (uint32_t)added), s->v + 2u * s->fluid, 2u * s->walls * sizeof(double));
    for (j = 0; j < up; ++j)
        for (i = 0; i < across; ++i) {
            s->x[2u * s->fluid] = x0 + (i + 0.5) * step; s->x[2u * s->fluid + 1u] = y0 + (j + 0.5) * step;
            s->v[2u * s->fluid] = s->v[2u * s->fluid + 1u] = 0.0;
            s->fluid++;
        }
}

void fl_constraints(Fl *s, const double *x) {
    const uint32_t total = s->fluid + s->walls, cells = s->cells[0] * s->cells[1];
    const double   margin = 3.0 * s->spacing;
    uint32_t       i, at;
    int            cx, cy, dx, dy;
    double         w[3];
    /* counting sort of all particles into cells */
    memset(s->cell, 0, ((size_t)cells + 1u) * sizeof(uint32_t));
    for (i = 0u; i < total; ++i) {
        cx = (int)((x[2u * i] + margin) / s->radius); cy = (int)((x[2u * i + 1u] + margin) / s->radius);
        s->cell[(uint32_t)cy * s->cells[0] + (uint32_t)cx + 1u]++;
    }
    for (at = 0u; at < cells; ++at) s->cell[at + 1u] += s->cell[at];
    for (i = total; i-- > 0u;) {
        cx = (int)((x[2u * i] + margin) / s->radius); cy = (int)((x[2u * i + 1u] + margin) / s->radius);
        s->order[--s->cell[(uint32_t)cy * s->cells[0] + (uint32_t)cx + 1u]] = i;
    }
    /* after the loop above cell[k + 1] is where cell k starts */
    s->overflow = 0u;
    for (i = 0u; i < s->fluid; ++i) {
        double density;
        fl_kernel(s, 0.0, w);
        density     = s->mass * w[0];
        s->count[i] = 0u;
        cx = (int)((x[2u * i] + margin) / s->radius); cy = (int)((x[2u * i + 1u] + margin) / s->radius);
        for (dy = -1; dy <= 1; ++dy)
            for (dx = -1; dx <= 1; ++dx) {
                const int nx = cx + dx, ny = cy + dy;
                uint32_t  first, last;
                if (nx < 0 || ny < 0 || nx >= (int)s->cells[0] || ny >= (int)s->cells[1]) continue;
                first = s->cell[(uint32_t)ny * s->cells[0] + (uint32_t)nx + 1u];
                last  = (uint32_t)ny * s->cells[0] + (uint32_t)nx + 2u <= cells ? s->cell[(uint32_t)ny * s->cells[0] + (uint32_t)nx + 2u] : total;
                for (at = first; at < last; ++at) {
                    const uint32_t k = s->order[at];
                    const double   ex = x[2u * i] - x[2u * k], ey = x[2u * i + 1u] - x[2u * k + 1u], r2 = ex * ex + ey * ey;
                    if (k == i || r2 >= s->radius * s->radius) continue;
                    fl_kernel(s, sqrt(r2), w);
                    density += s->mass * w[0];
                    if (s->count[i] == FL_NEIGHBOURS) { s->overflow++; continue; }
                    s->list[(size_t)i * FL_NEIGHBOURS + s->count[i]++] = k;
                }
            }
        s->c[i] = density / s->rest - 1.0;
    }
}

double fl_energy(const Fl *s, const double *x, const double *y, double compliance) {
    const double weight = compliance * s->scale / (240.0 * 240.0) / (2.0 * s->h * s->h);   /* alpha m / (2 h^2) */
    double       energy = 0.0;
    uint32_t     i;
    for (i = 0u; i < s->fluid; ++i) {
        const double c = fmax(s->c[i], 0.0), dx = x[2u * i] - y[2u * i], dy = x[2u * i + 1u] - y[2u * i + 1u];
        energy += weight * (dx * dx + dy * dy) + 0.5 * c * c;
    }
    return energy;
}

void fl_local(const Fl *s, const double *x, const double *y, double compliance, bool exact, uint32_t i, double f[2], double H[3]) {
    const double weight = compliance * s->scale / (240.0 * 240.0) / (s->h * s->h), ratio = s->mass / s->rest;   /* alpha m / h^2 */
    const double ci = fmax(s->c[i], 0.0);
    double       own[2] = {0.0, 0.0}, second[3] = {0.0, 0.0, 0.0}, w[3];
    uint32_t     n;
    f[0] = -weight * (x[2u * i] - y[2u * i]); f[1] = -weight * (x[2u * i + 1u] - y[2u * i + 1u]);
    H[0] = H[2] = weight; H[1] = 0.0;
    for (n = 0u; n < s->count[i]; ++n) {
        const uint32_t k = s->list[(size_t)i * FL_NEIGHBOURS + n];
        const double   e[2] = {x[2u * i] - x[2u * k], x[2u * i + 1u] - x[2u * k + 1u]}, r = sqrt(e[0] * e[0] + e[1] * e[1]);
        const double   ck = k < s->fluid ? fmax(s->c[k], 0.0) : 0.0;   /* a wall particle has no constraint of its own */
        double         g[2], across, along;
        if (r == 0.0) continue;
        fl_kernel(s, r, w);
        g[0] = ratio * w[1] * e[0] / r; g[1] = ratio * w[1] * e[1] / r;   /* dC_i/dx_i's share, and dC_k/dx_i */
        own[0] += g[0]; own[1] += g[1];
        /* C_k dC_k/dx_i and, where C_k > 0, the first term of eq. 15 for constraint k */
        f[0] -= ck * g[0]; f[1] -= ck * g[1];
        if (ck > 0.0 || (!s->active && k < s->fluid)) { H[0] += g[0] * g[0]; H[1] += g[0] * g[1]; H[2] += g[1] * g[1]; }
        /* second derivative of W: W'' along e, W' / r across it; it enters with C_i + C_k */
        along = ratio * w[2]; across = ratio * w[1] / r;
        second[0] += (ci + ck) * (across + (along - across) * e[0] * e[0] / (r * r));
        second[1] += (ci + ck) * (along - across) * e[0] * e[1] / (r * r);
        second[2] += (ci + ck) * (across + (along - across) * e[1] * e[1] / (r * r));
    }
    f[0] -= ci * own[0]; f[1] -= ci * own[1];
    if (ci > 0.0 || !s->active) { H[0] += own[0] * own[0]; H[1] += own[0] * own[1]; H[2] += own[1] * own[1]; }
    if (exact) { H[0] += second[0]; H[1] += second[1]; H[2] += second[2]; return; }
    /* section 3.5: a diagonal of the norms of the columns in place of the second term */
    H[0] += sqrt(second[0] * second[0] + second[1] * second[1]);
    H[2] += sqrt(second[1] * second[1] + second[2] * second[2]);
}

/* A coordinate past a wall is mirrored back in, which keeps two particles that crossed apart
 * (clamping put them on one line, and in a corner on one point, where the kernel's gradient is
 * zero and nothing separates them again). */
static double inside(double x, double lo, double hi) {
    x = x < lo ? 2.0 * lo - x : x;
    x = x > hi ? 2.0 * hi - x : x;
    return fmin(fmax(x, lo), hi);
}

void fl_step(Fl *s) {
    const uint32_t total = s->fluid + s->walls;
    const double   lo = 0.5 * s->spacing, ratio = s->mass / s->rest;
    uint32_t       i, n, l;
    int            k;
    double         w[3];

    /* Algorithm 1, lines 1 and 2: y = x + h v + h^2 a, and the iterations start from y */
    memcpy(s->y, s->x, 2u * total * sizeof(double));
    for (i = 0u; i < s->fluid; ++i) {
        s->y[2u * i]      += s->h * s->v[2u * i];
        s->y[2u * i + 1u] += s->h * s->v[2u * i + 1u] - s->h * s->h * s->gravity;
    }
    memcpy(s->guess, s->y, 2u * total * sizeof(double));
    for (i = 0u; i < s->fluid; ++i)
        for (k = 0; k < 2; ++k) s->guess[2u * i + (uint32_t)k] = inside(s->guess[2u * i + (uint32_t)k], lo, s->box[k] - lo);
    memcpy(s->next, s->guess, 2u * total * sizeof(double));
    memcpy(s->other, s->guess, 2u * total * sizeof(double));

    for (l = 0u; l < s->iterations; ++l) {
        fl_constraints(s, s->guess);
        if (s->method == FL_PBF) {
            /* its eq. 11: lambda_i = -C_i / (sum_k |dC_i/dx_k|^2 + epsilon) */
            for (i = 0u; i < s->fluid; ++i) {
                double own[2] = {0.0, 0.0}, sum = 0.0;
                for (n = 0u; n < s->count[i]; ++n) {
                    const uint32_t j = s->list[(size_t)i * FL_NEIGHBOURS + n];
                    const double   e[2] = {s->guess[2u * i] - s->guess[2u * j], s->guess[2u * i + 1u] - s->guess[2u * j + 1u]}, r = sqrt(e[0] * e[0] + e[1] * e[1]);
                    if (r == 0.0) continue;
                    fl_kernel(s, r, w);
                    own[0] += ratio * w[1] * e[0] / r; own[1] += ratio * w[1] * e[1] / r;
                    sum    += ratio * ratio * w[1] * w[1];
                }
                s->lambda[i] = -fmax(s->c[i], 0.0) / (sum + own[0] * own[0] + own[1] * own[1] + s->relaxation * s->scale);
            }
            /* its eq. 12: dp_i = 1 / rest sum_j (lambda_i + lambda_j) grad W */
            for (i = 0u; i < s->fluid; ++i)
                for (n = 0u; n < s->count[i]; ++n) {
                    const uint32_t j = s->list[(size_t)i * FL_NEIGHBOURS + n];
                    const double   e[2] = {s->guess[2u * i] - s->guess[2u * j], s->guess[2u * i + 1u] - s->guess[2u * j + 1u]}, r = sqrt(e[0] * e[0] + e[1] * e[1]);
                    if (r == 0.0) continue;
                    fl_kernel(s, r, w);
                    for (k = 0; k < 2; ++k) s->next[2u * i + (uint32_t)k] += (s->lambda[i] + (j < s->fluid ? s->lambda[j] : 0.0)) * ratio * w[1] * e[k] / r;
                }
        }
        for (i = 0u; i < s->fluid && s->method == FL_IPBF; ++i) {
            /* lines 7 to 10: dx = H^-1 f, and half of it is applied */
            double f[2], H[3], det;
            fl_local(s, s->guess, s->y, s->compliance, s->exact, i, f, H);
            det = H[0] * H[2] - H[1] * H[1];
            if (fabs(det) > 1e-30) {
                s->next[2u * i]      += 0.5 * (H[2] * f[0] - H[1] * f[1]) / det;
                s->next[2u * i + 1u] += 0.5 * (H[0] * f[1] - H[1] * f[0]) / det;
            }
            /* section 3.6: in the last iteration, the same from a more compliant energy */
            if (l + 1u < s->iterations) continue;
            fl_local(s, s->guess, s->y, s->soft, s->exact, i, f, H);
            det = H[0] * H[2] - H[1] * H[1];
            s->other[2u * i]      = s->guess[2u * i] + 0.5 * (H[2] * f[0] - H[1] * f[1]) / det;
            s->other[2u * i + 1u] = s->guess[2u * i + 1u] + 0.5 * (H[0] * f[1] - H[1] * f[0]) / det;
        }
        for (i = 0u; i < s->fluid; ++i)
            for (k = 0; k < 2; ++k) s->next[2u * i + (uint32_t)k] = inside(s->next[2u * i + (uint32_t)k], lo, s->box[k] - lo);
        memcpy(s->guess, s->next, 2u * s->fluid * sizeof(double));
    }

    for (i = 0u; i < s->fluid; ++i) {
        const double v[2] = {(s->guess[2u * i] - s->x[2u * i]) / s->h, (s->guess[2u * i + 1u] - s->x[2u * i + 1u]) / s->h};
        const double o[2] = {(s->other[2u * i] - s->x[2u * i]) / s->h, (s->other[2u * i + 1u] - s->x[2u * i + 1u]) / s->h};   /* eq. 16 */
        const double apart = sqrt((s->other[2u * i] - s->guess[2u * i]) * (s->other[2u * i] - s->guess[2u * i]) + (s->other[2u * i + 1u] - s->guess[2u * i + 1u]) * (s->other[2u * i + 1u] - s->guess[2u * i + 1u]));
        const double v2 = v[0] * v[0] + v[1] * v[1], o2 = o[0] * o[0] + o[1] * o[1];
        double       keep = 1.0;
        /* eq. 17, 18 */
        if (s->method == FL_IPBF && s->damping && apart < s->beta * s->radius && o2 < v2) keep = sqrt(1.0 - (1.0 - apart / (s->beta * s->radius)) * (v2 - o2) / v2);
        s->v[2u * i] = keep * v[0]; s->v[2u * i + 1u] = keep * v[1];
        s->x[2u * i] = s->guess[2u * i]; s->x[2u * i + 1u] = s->guess[2u * i + 1u];
    }

    fl_constraints(s, s->x);
    s->error = s->densest = 0.0;
    for (i = 0u; i < s->fluid; ++i) {
        s->error  += fmax(s->c[i], 0.0) / s->fluid;
        s->densest = fmax(s->densest, s->c[i] + 1.0);
    }
}
