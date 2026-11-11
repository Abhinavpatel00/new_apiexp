/* method.c — see method.h. */
#include "method.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

void snow_weights(const float pos[3], int base[3], double w[3][4], double dw[3][4]) {
    int a;
    for (a = 0; a < 3; ++a) {
        const double cell = floor((double)pos[a]), t = (double)pos[a] - cell, u = 1.0 - t;
        base[a]  = (int)cell - 1;
        w[a][0]  = u * u * u / 6.0;
        w[a][1]  = 0.5 * t * t * t - t * t + 2.0 / 3.0;
        w[a][2]  = 0.5 * u * u * u - u * u + 2.0 / 3.0;
        w[a][3]  = t * t * t / 6.0;
        dw[a][0] = -0.5 * u * u;
        dw[a][1] = 1.5 * t * t - 2.0 * t;
        dw[a][2] = -1.5 * u * u + 2.0 * u;
        dw[a][3] = 0.5 * t * t;
    }
}

void snow_eigen(double c[3][3], double v[3][3], double lam[3]) {
    static const int k_pairs[3][2] = {{0, 1}, {0, 2}, {1, 2}};
    int sweep, pair, k;
    memset(v, 0, 9 * sizeof(double));
    v[0][0] = v[1][1] = v[2][2] = 1.0;
    for (sweep = 0; sweep < 6; ++sweep)
        for (pair = 0; pair < 3; ++pair) {
            const int i = k_pairs[pair][0], j = k_pairs[pair][1];
            double theta, t, cs, sn;
            if (c[i][j] == 0.0) continue;
            theta = (c[j][j] - c[i][i]) / (2.0 * c[i][j]);
            t     = (theta < 0.0 ? -1.0 : 1.0) / (fabs(theta) + sqrt(theta * theta + 1.0));
            cs    = 1.0 / sqrt(t * t + 1.0);
            sn    = t * cs;
            for (k = 0; k < 3; ++k) { const double a = c[k][i], b = c[k][j]; c[k][i] = cs * a - sn * b; c[k][j] = sn * a + cs * b; }
            for (k = 0; k < 3; ++k) { const double a = c[i][k], b = c[j][k]; c[i][k] = cs * a - sn * b; c[j][k] = sn * a + cs * b; }
            for (k = 0; k < 3; ++k) { const double a = v[k][i], b = v[k][j]; v[k][i] = cs * a - sn * b; v[k][j] = sn * a + cs * b; }
        }
    for (k = 0; k < 3; ++k) lam[k] = c[k][k];
}

void snow_polar(const double f[3][3], double r[3][3], double s[3][3]) {
    double c[3][3], v[3][3], lam[3], inv[3][3];
    int i, j, k;
    for (i = 0; i < 3; ++i)
        for (j = 0; j < 3; ++j) c[i][j] = f[0][i] * f[0][j] + f[1][i] * f[1][j] + f[2][i] * f[2][j];
    snow_eigen(c, v, lam);
    for (i = 0; i < 3; ++i)
        for (j = 0; j < 3; ++j) {
            s[i][j] = inv[i][j] = 0.0;
            for (k = 0; k < 3; ++k) {
                const double root = sqrt(fmax(lam[k], 1e-300));
                s[i][j]   += v[i][k] * root * v[j][k];
                inv[i][j] += v[i][k] / root * v[j][k];
            }
        }
    for (i = 0; i < 3; ++i)
        for (j = 0; j < 3; ++j) r[i][j] = f[i][0] * inv[0][j] + f[i][1] * inv[1][j] + f[i][2] * inv[2][j];
}

/* Rows of the cofactor matrix J F^-T are cross products of the rows of F. */
static void cofactor(const double f[3][3], double out[3][3]) {
    int i, k;
    for (i = 0; i < 3; ++i) {
        const double *a = f[(i + 1) % 3], *b = f[(i + 2) % 3];
        for (k = 0; k < 3; ++k) out[i][k] = a[(k + 1) % 3] * b[(k + 2) % 3] - a[(k + 2) % 3] * b[(k + 1) % 3];
    }
}

/* Packed snow is stiffer and torn snow weaker: exp(xi (1 - J_P)), kept between 0.1 and 5. */
static double harden(const SnowParams *p, double jp) { return fmin(fmax(exp(p->hardening * (1.0 - jp)), 0.1), 5.0); }

double snow_energy(const SnowParams *p, const double f[3][3], double jp) {
    double r[3][3], s[3][3], sum = 0.0, j;
    int i, k;
    snow_polar(f, r, s);
    for (i = 0; i < 3; ++i)
        for (k = 0; k < 3; ++k) sum += (f[i][k] - r[i][k]) * (f[i][k] - r[i][k]);
    j = f[0][0] * (f[1][1] * f[2][2] - f[1][2] * f[2][1]) - f[0][1] * (f[1][0] * f[2][2] - f[1][2] * f[2][0]) +
        f[0][2] * (f[1][0] * f[2][1] - f[1][1] * f[2][0]);
    return harden(p, jp) * (p->mu * sum + 0.5 * p->lambda * (j - 1.0) * (j - 1.0));
}

void snow_stress(const SnowParams *p, const double f[3][3], double jp, double out[3][3]) {
    const double h = harden(p, jp);
    double r[3][3], s[3][3], cof[3][3], j;
    int i, k;
    snow_polar(f, r, s);
    cofactor(f, cof);
    j = f[0][0] * cof[0][0] + f[0][1] * cof[0][1] + f[0][2] * cof[0][2];
    for (i = 0; i < 3; ++i)
        for (k = 0; k < 3; ++k) out[i][k] = h * (2.0 * p->mu * (f[i][k] - r[i][k]) + p->lambda * (j - 1.0) * cof[i][k]);
}

void snow_stress_change(const SnowParams *p, const double f[3][3], double jp, const double df[3][3], double out[3][3]) {
    const double h = harden(p, jp);
    double r[3][3], s[3][3], cof[3][3], dcof[3][3], w[3][3], m[3][3], inv[3][3], axial[3], omega[3], y[3][3], dr[3][3];
    double j, along = 0.0, det;
    int i, k;
    snow_polar(f, r, s);
    cofactor(f, cof);
    j = f[0][0] * cof[0][0] + f[0][1] * cof[0][1] + f[0][2] * cof[0][2];

    /* dR: R^T dF - dF^T R = (R^T dR) S + S (R^T dR), and R^T dR is skew. For a skew matrix that
     * turns about omega, the right side turns about (tr(S) 1 - S) omega. */
    for (i = 0; i < 3; ++i)
        for (k = 0; k < 3; ++k)
            w[i][k] = r[0][i] * df[0][k] + r[1][i] * df[1][k] + r[2][i] * df[2][k] - (df[0][i] * r[0][k] + df[1][i] * r[1][k] + df[2][i] * r[2][k]);
    axial[0] = w[2][1]; axial[1] = w[0][2]; axial[2] = w[1][0];
    for (i = 0; i < 3; ++i)
        for (k = 0; k < 3; ++k) m[i][k] = (i == k ? s[0][0] + s[1][1] + s[2][2] : 0.0) - s[i][k];
    cofactor(m, inv);     /* inverse = cofactor^T / det */
    det = m[0][0] * inv[0][0] + m[0][1] * inv[0][1] + m[0][2] * inv[0][2];
    for (i = 0; i < 3; ++i) omega[i] = (inv[0][i] * axial[0] + inv[1][i] * axial[1] + inv[2][i] * axial[2]) / det;
    memset(y, 0, sizeof y);
    y[0][1] = -omega[2]; y[0][2] = omega[1]; y[1][0] = omega[2]; y[1][2] = -omega[0]; y[2][0] = -omega[1]; y[2][1] = omega[0];
    for (i = 0; i < 3; ++i)
        for (k = 0; k < 3; ++k) dr[i][k] = r[i][0] * y[0][k] + r[i][1] * y[1][k] + r[i][2] * y[2][k];

    /* d(J F^-T): each row is a cross product of two rows of F, so its change is two cross products. */
    for (i = 0; i < 3; ++i) {
        const double *a = f[(i + 1) % 3], *b = f[(i + 2) % 3], *da = df[(i + 1) % 3], *db = df[(i + 2) % 3];
        for (k = 0; k < 3; ++k) {
            const int k1 = (k + 1) % 3, k2 = (k + 2) % 3;
            dcof[i][k] = da[k1] * b[k2] - da[k2] * b[k1] + a[k1] * db[k2] - a[k2] * db[k1];
            along += cof[i][k] * df[i][k];
        }
    }
    for (i = 0; i < 3; ++i)
        for (k = 0; k < 3; ++k)
            out[i][k] = h * (2.0 * p->mu * (df[i][k] - dr[i][k]) + p->lambda * (cof[i][k] * along + (j - 1.0) * dcof[i][k]));
}

void snow_collide(const SnowParams *p, const double x[3], double v[3]) {
    const double low = p->walls, high = (double)SNOW_N - 1.0 - p->walls;
    int surface, k;
    for (surface = 0; surface < 7; ++surface) {
        double n[3] = {0.0, 0.0, 0.0}, moving[3] = {0.0, 0.0, 0.0}, rel[3], vn, vt[3], len;
        if (surface < 6) {
            const int axis = surface / 2;
            if (surface % 2 == 0 ? x[axis] >= low : x[axis] <= high) continue;
            n[axis] = surface % 2 == 0 ? 1.0 : -1.0;
            /* Sticky: everything but the floor holds what is inside it, whichever way it moves. */
            if (p->sticky && surface != 2) { v[0] = v[1] = v[2] = 0.0; continue; }
        } else {
            const double d[3] = {x[0] - p->ball[0], x[1] - p->ball[1], x[2] - p->ball[2]};
            len = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (len >= p->ball[3] || len == 0.0) continue;
            for (k = 0; k < 3; ++k) { n[k] = d[k] / len; moving[k] = p->ball_velocity[k]; }
        }
        /* In the frame of the surface. Moving away: nothing. Pressed hard enough for friction to hold:
         * it sticks. Else it slides and friction slows it. */
        for (k = 0; k < 3; ++k) rel[k] = v[k] - moving[k];
        vn = rel[0] * n[0] + rel[1] * n[1] + rel[2] * n[2];
        if (vn >= 0.0) continue;
        for (k = 0; k < 3; ++k) vt[k] = rel[k] - vn * n[k];
        len = sqrt(vt[0] * vt[0] + vt[1] * vt[1] + vt[2] * vt[2]);
        for (k = 0; k < 3; ++k) v[k] = moving[k] + (len <= -p->friction * vn ? 0.0 : vt[k] + p->friction * vn * vt[k] / len);
    }
}

void snow_volumes(SnowParticle *particles, uint32_t count) {
    float   *mass = calloc(SNOW_NODES, sizeof *mass);
    uint32_t i;
    int      pass, base[3], x, y, z;
    double   w[3][4], dw[3][4];
    for (pass = 0; pass < 2; ++pass)
        for (i = 0u; i < count; ++i) {
            double density = 0.0;
            snow_weights(particles[i].pos, base, w, dw);
            for (z = 0; z < 4; ++z)
                for (y = 0; y < 4; ++y)
                    for (x = 0; x < 4; ++x) {
                        const size_t node = ((size_t)(base[2] + z) * SNOW_N + (size_t)(base[1] + y)) * SNOW_N + (size_t)(base[0] + x);
                        if (pass == 0) mass[node] += (float)(w[0][x] * w[1][y] * w[2][z]);
                        else density += mass[node] * w[0][x] * w[1][y] * w[2][z];
                    }
            if (pass == 1) particles[i].volume = (float)(SNOW_PER_CELL / density);
        }
    free(mass);
}

/* (dPsi/dF) F^T / density of one particle: what its node forces are made of. */
static void kirchhoff(const SnowParams *p, const SnowParticle *q, double out[3][3]) {
    const double f[3][3] = {{q->f[0][0], q->f[0][1], q->f[0][2]}, {q->f[1][0], q->f[1][1], q->f[1][2]}, {q->f[2][0], q->f[2][1], q->f[2][2]}};
    double stress[3][3];
    int i, k;
    snow_stress(p, f, q->jp, stress);
    for (i = 0; i < 3; ++i)
        for (k = 0; k < 3; ++k) out[i][k] = stress[i][0] * f[k][0] + stress[i][1] * f[k][1] + stress[i][2] * f[k][2];
}

void snow_p2g(const SnowParams *p, const SnowParticle *particles, uint32_t count, int32_t *grid) {
    uint32_t i;
    int      base[3], x, y, z, k;
    double   w[3][4], dw[3][4], tau[3][3];
    memset(grid, 0, (size_t)SNOW_NODES * 8u * sizeof *grid);
    for (i = 0u; i < count; ++i) {
        const SnowParticle *q = &particles[i];
        snow_weights(q->pos, base, w, dw);
        kirchhoff(p, q, tau);
        for (z = 0; z < 4; ++z)
            for (y = 0; y < 4; ++y)
                for (x = 0; x < 4; ++x) {
                    /* Adds wrap as the GPU's do, so the sums are done unsigned. */
                    uint32_t    *node = (uint32_t *)&grid[(((size_t)(base[2] + z) * SNOW_N + (size_t)(base[1] + y)) * SNOW_N + (size_t)(base[0] + x)) * 8u];
                    const double weight = w[0][x] * w[1][y] * w[2][z];
                    const double slope[3] = {dw[0][x] * w[1][y] * w[2][z], w[0][x] * dw[1][y] * w[2][z], w[0][x] * w[1][y] * dw[2][z]};
                    node[0] += (uint32_t)(int32_t)floor(weight * SNOW_MASS_SCALE + 0.5);
                    for (k = 0; k < 3; ++k) {
                        const double impulse = -p->dt * q->volume * (tau[k][0] * slope[0] + tau[k][1] * slope[1] + tau[k][2] * slope[2]);
                        node[1 + k] += (uint32_t)(int32_t)floor(weight * q->vel[k] * SNOW_MOMENTUM_SCALE + 0.5);
                        node[4 + k] += (uint32_t)(int32_t)floor(impulse * SNOW_IMPULSE_SCALE + 0.5);
                    }
                }
    }
}

void snow_velocities(const SnowParams *p, const int32_t *grid, double *mass, double *before, double *after, uint8_t *held) {
    size_t node;
    int    k;
    for (node = 0u; node < SNOW_NODES; ++node) {
        const int32_t *g = &grid[node * 8u];
        const double   x[3] = {(double)(node % SNOW_N), (double)(node / SNOW_N % SNOW_N), (double)(node / (SNOW_N * SNOW_N))};
        double         free[3] = {0.0, 0.0, 0.0};
        mass[node] = g[0] > 0 ? g[0] / SNOW_MASS_SCALE : 0.0;
        for (k = 0; k < 3; ++k) {
            before[node * 3u + k] = g[0] > 0 ? g[1 + k] / SNOW_MOMENTUM_SCALE / mass[node] : 0.0;
            after[node * 3u + k]  = g[0] > 0 ? before[node * 3u + k] + g[4 + k] / SNOW_IMPULSE_SCALE / mass[node] - (k == 1 ? p->dt * p->gravity : 0.0) : 0.0;
            free[k] = after[node * 3u + k];
        }
        if (g[0] > 0) snow_collide(p, x, &after[node * 3u]);
        if (held) held[node] = free[0] != after[node * 3u] || free[1] != after[node * 3u + 1u] || free[2] != after[node * 3u + 2u];
    }
}

void snow_forces(const SnowParams *p, const SnowParticle *particles, uint32_t count, const double *offset, double *force, double *energy) {
    uint32_t i;
    int      base[3], x, y, z, r, c;
    double   w[3][4], dw[3][4];
    if (force) memset(force, 0, (size_t)SNOW_NODES * 3u * sizeof *force);
    if (energy) *energy = 0.0;
    for (i = 0u; i < count; ++i) {
        const SnowParticle *q = &particles[i];
        double grow[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}, moved[3][3], stress[3][3], tau[3][3];
        snow_weights(q->pos, base, w, dw);
        /* (1): F^_E = (1 + sum (x^_i - x_i) grad w_i^T) F_E */
        for (z = 0; z < 4 && offset; ++z)
            for (y = 0; y < 4; ++y)
                for (x = 0; x < 4; ++x) {
                    const double *o = &offset[(((size_t)(base[2] + z) * SNOW_N + (size_t)(base[1] + y)) * SNOW_N + (size_t)(base[0] + x)) * 3u];
                    const double  slope[3] = {dw[0][x] * w[1][y] * w[2][z], w[0][x] * dw[1][y] * w[2][z], w[0][x] * w[1][y] * dw[2][z]};
                    for (r = 0; r < 3; ++r)
                        for (c = 0; c < 3; ++c) grow[r][c] += o[r] * slope[c];
                }
        for (r = 0; r < 3; ++r)
            for (c = 0; c < 3; ++c) moved[r][c] = grow[r][0] * q->f[0][c] + grow[r][1] * q->f[1][c] + grow[r][2] * q->f[2][c];
        if (energy) *energy += q->volume * snow_energy(p, moved, q->jp);
        if (!force) continue;
        /* (2): -f_i = sum V (dPsi/dF)(F^_E) F_E^T grad w_i */
        snow_stress(p, moved, q->jp, stress);
        for (r = 0; r < 3; ++r)
            for (c = 0; c < 3; ++c) tau[r][c] = stress[r][0] * q->f[c][0] + stress[r][1] * q->f[c][1] + stress[r][2] * q->f[c][2];
        for (z = 0; z < 4; ++z)
            for (y = 0; y < 4; ++y)
                for (x = 0; x < 4; ++x) {
                    double      *out = &force[(((size_t)(base[2] + z) * SNOW_N + (size_t)(base[1] + y)) * SNOW_N + (size_t)(base[0] + x)) * 3u];
                    const double slope[3] = {dw[0][x] * w[1][y] * w[2][z], w[0][x] * dw[1][y] * w[2][z], w[0][x] * w[1][y] * dw[2][z]};
                    for (r = 0; r < 3; ++r) out[r] -= q->volume * (tau[r][0] * slope[0] + tau[r][1] * slope[1] + tau[r][2] * slope[2]);
                }
    }
}

void snow_force_change(const SnowParams *p, const SnowParticle *particles, uint32_t count, const double *du, double *df) {
    uint32_t i;
    int      base[3], x, y, z, r, c;
    double   w[3][4], dw[3][4];
    memset(df, 0, (size_t)SNOW_NODES * 3u * sizeof *df);
    for (i = 0u; i < count; ++i) {
        const SnowParticle *q = &particles[i];
        const double f[3][3] = {{q->f[0][0], q->f[0][1], q->f[0][2]}, {q->f[1][0], q->f[1][1], q->f[1][2]}, {q->f[2][0], q->f[2][1], q->f[2][2]}};
        double grad[3][3] = {{0}}, change[3][3], a[3][3], af[3][3];
        snow_weights(q->pos, base, w, dw);
        /* (4): A_p = d2Psi/dF2 : (sum du_j grad w_j^T F_E) */
        for (z = 0; z < 4; ++z)
            for (y = 0; y < 4; ++y)
                for (x = 0; x < 4; ++x) {
                    const double *u = &du[(((size_t)(base[2] + z) * SNOW_N + (size_t)(base[1] + y)) * SNOW_N + (size_t)(base[0] + x)) * 3u];
                    const double  slope[3] = {dw[0][x] * w[1][y] * w[2][z], w[0][x] * dw[1][y] * w[2][z], w[0][x] * w[1][y] * dw[2][z]};
                    for (r = 0; r < 3; ++r)
                        for (c = 0; c < 3; ++c) grad[r][c] += u[r] * slope[c];
                }
        for (r = 0; r < 3; ++r)
            for (c = 0; c < 3; ++c) change[r][c] = grad[r][0] * f[0][c] + grad[r][1] * f[1][c] + grad[r][2] * f[2][c];
        snow_stress_change(p, f, q->jp, change, a);
        /* (3): -df_i = sum V A_p F_E^T grad w_i */
        for (r = 0; r < 3; ++r)
            for (c = 0; c < 3; ++c) af[r][c] = a[r][0] * f[c][0] + a[r][1] * f[c][1] + a[r][2] * f[c][2];
        for (z = 0; z < 4; ++z)
            for (y = 0; y < 4; ++y)
                for (x = 0; x < 4; ++x) {
                    double      *out = &df[(((size_t)(base[2] + z) * SNOW_N + (size_t)(base[1] + y)) * SNOW_N + (size_t)(base[0] + x)) * 3u];
                    const double slope[3] = {dw[0][x] * w[1][y] * w[2][z], w[0][x] * dw[1][y] * w[2][z], w[0][x] * w[1][y] * dw[2][z]};
                    for (r = 0; r < 3; ++r) out[r] -= q->volume * (af[r][0] * slope[0] + af[r][1] * slope[1] + af[r][2] * slope[2]);
                }
    }
}

uint32_t snow_implicit(const SnowParams *p, const SnowParticle *particles, uint32_t count, double beta, const double *mass,
                       const uint8_t *held, double *after, double *residual) {
    const size_t n = (size_t)SNOW_NODES * 3u;
    const double scale = beta * p->dt * p->dt;
    double   *y = calloc(n, sizeof *y), *r = calloc(n, sizeof *r), *dir = calloc(n, sizeof *dir), *ar = calloc(n, sizeof *ar),
             *adir = calloc(n, sizeof *adir), *v = calloc(n, sizeof *v);
    uint32_t *live = malloc(SNOW_NODES * sizeof *live), lives = 0u, iteration = 0u, at, k;
    double    size = 0.0, r_ar = 0.0, left = 0.0;
    size_t    i;

    /* Only nodes with mass that no collision holds take part; the vectors are zero elsewhere, so a
     * held node keeps v* and the others feel it through df(v*) in the first residual. */
    for (i = 0u; i < SNOW_NODES; ++i)
        if (mass[i] > 0.0 && !(held && held[i])) live[lives++] = (uint32_t)i;

    /* Node masses differ by a factor of millions between the middle of the snow and its rim, so the
     * unknown is y = sqrt(m) v:   A y = y - beta dt^2 df(y / sqrt(m)) / sqrt(m),   b = sqrt(m) v*.
     * A is symmetric and has the eigenvalues of the paper's 1 + beta dt^2 m^-1 d2Phi/dx2.
     * Start from y = b: r = b - A b = beta dt^2 df(v*) / sqrt(m). */
    snow_force_change(p, particles, count, after, r);
    for (at = 0u; at < lives; ++at)
        for (k = 0u; k < 3u; ++k) {
            const double root = sqrt(mass[live[at]]);
            i      = (size_t)live[at] * 3u + k;
            y[i]   = root * after[i];
            r[i]  *= scale / root;
            dir[i] = r[i];
            v[i]   = r[i] / root;
            size  += y[i] * y[i];
        }
    snow_force_change(p, particles, count, v, ar);
    for (at = 0u; at < lives; ++at)
        for (k = 0u; k < 3u; ++k) {
            i       = (size_t)live[at] * 3u + k;
            ar[i]   = r[i] - scale * ar[i] / sqrt(mass[live[at]]);
            adir[i] = ar[i];
            r_ar   += r[i] * ar[i];
            left   += r[i] * r[i];
        }
    while (iteration < 60u && left > 1e-14 * size) {
        double step, a_a = 0.0, next = 0.0, mix;
        for (at = 0u; at < lives; ++at)
            for (k = 0u; k < 3u; ++k) a_a += adir[(size_t)live[at] * 3u + k] * adir[(size_t)live[at] * 3u + k];
        if (a_a == 0.0) break;
        step = r_ar / a_a;
        left = 0.0;
        for (at = 0u; at < lives; ++at)
            for (k = 0u; k < 3u; ++k) {
                i     = (size_t)live[at] * 3u + k;
                y[i] += step * dir[i];
                r[i] -= step * adir[i];
                v[i]  = r[i] / sqrt(mass[live[at]]);
                left += r[i] * r[i];
            }
        snow_force_change(p, particles, count, v, ar);
        for (at = 0u; at < lives; ++at)
            for (k = 0u; k < 3u; ++k) {
                i     = (size_t)live[at] * 3u + k;
                ar[i] = r[i] - scale * ar[i] / sqrt(mass[live[at]]);
                next += r[i] * ar[i];
            }
        mix = next / r_ar;
        for (at = 0u; at < lives; ++at)
            for (k = 0u; k < 3u; ++k) {
                i       = (size_t)live[at] * 3u + k;
                dir[i]  = r[i] + mix * dir[i];
                adir[i] = ar[i] + mix * adir[i];
            }
        r_ar = next;
        ++iteration;
    }
    for (at = 0u; at < lives; ++at)
        for (k = 0u; k < 3u; ++k) after[(size_t)live[at] * 3u + k] = y[(size_t)live[at] * 3u + k] / sqrt(mass[live[at]]);
    if (residual) *residual = size > 0.0 ? sqrt(left / size) : 0.0;
    free(y); free(r); free(dir); free(ar); free(adir); free(v); free(live);
    return iteration;
}

void snow_g2p(const SnowParams *p, SnowParticle *q, const double *before, const double *after) {
    int    base[3], x, y, z, r, c, k;
    double w[3][4], dw[3][4], pic[3] = {0, 0, 0}, change[3] = {0, 0, 0}, grad[3][3] = {{0}}, moved[3][3], cc[3][3], v[3][3], lam[3];
    double vel[3], pos[3], fix[3][3], ratio = 1.0;
    snow_weights(q->pos, base, w, dw);
    for (z = 0; z < 4; ++z)
        for (y = 0; y < 4; ++y)
            for (x = 0; x < 4; ++x) {
                const size_t node = ((size_t)(base[2] + z) * SNOW_N + (size_t)(base[1] + y)) * SNOW_N + (size_t)(base[0] + x);
                const double weight = w[0][x] * w[1][y] * w[2][z];
                const double slope[3] = {dw[0][x] * w[1][y] * w[2][z], w[0][x] * dw[1][y] * w[2][z], w[0][x] * w[1][y] * dw[2][z]};
                for (r = 0; r < 3; ++r) {
                    pic[r]    += weight * after[node * 3u + r];
                    change[r] += weight * (after[node * 3u + r] - before[node * 3u + r]);
                    for (c = 0; c < 3; ++c) grad[r][c] += after[node * 3u + r] * slope[c];
                }
            }
    /* Step 7: F^_E = (1 + dt grad v) F_E; its singular values are clamped; what is cut off goes to F_P. */
    for (r = 0; r < 3; ++r)
        for (c = 0; c < 3; ++c)
            moved[r][c] = q->f[r][c] + p->dt * (grad[r][0] * q->f[0][c] + grad[r][1] * q->f[1][c] + grad[r][2] * q->f[2][c]);
    for (r = 0; r < 3; ++r)
        for (c = 0; c < 3; ++c) cc[r][c] = moved[0][r] * moved[0][c] + moved[1][r] * moved[1][c] + moved[2][r] * moved[2][c];
    snow_eigen(cc, v, lam);
    memset(fix, 0, sizeof fix);
    for (k = 0; k < 3; ++k) {
        const double sigma = sqrt(fmax(lam[k], 1e-12)), kept = fmin(fmax(sigma, 1.0 - p->compress), 1.0 + p->stretch);
        ratio *= sigma / kept;
        for (r = 0; r < 3; ++r)
            for (c = 0; c < 3; ++c) fix[r][c] += v[r][k] * (kept / sigma) * v[c][k];
    }
    for (r = 0; r < 3; ++r)
        for (c = 0; c < 3; ++c) q->f[r][c] = (float)(moved[r][0] * fix[0][c] + moved[r][1] * fix[1][c] + moved[r][2] * fix[2][c]);
    q->jp = (float)fmin(q->jp * ratio, 1e6);
    /* Steps 8 to 10. */
    for (k = 0; k < 3; ++k) {
        vel[k] = (1.0 - p->flip) * pic[k] + p->flip * (q->vel[k] + change[k]);
        pos[k] = q->pos[k];
    }
    snow_collide(p, pos, vel);
    for (k = 0; k < 3; ++k) {
        q->vel[k] = (float)vel[k];
        q->pos[k] = fminf(fmaxf((float)(pos[k] + p->dt * vel[k]), 1.0f), (float)SNOW_N - 2.001f);
    }
}
