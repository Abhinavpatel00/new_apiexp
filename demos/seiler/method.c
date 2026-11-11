/* method.c — Seiler's interpolation.
 *
 * Cem Yuksel: "Seiler's Interpolation for Evaluating Polynomial Curves",
 * SIGGRAPH Talks 2024; first given for texture filtering hardware in Lin,
 * Seiler, Yuksel: "Hardware Adaptive High-Order Interpolation for Real-Time
 * Graphics", HPG 2021.
 *
 * A cubic Bezier curve is
 *
 *     C(t) = lerp(b0, b3, t) + (1 - t) t lerp(d1, d2, t)
 *     d1 = 3 (b1 - b0) - (b3 - b0),   d2 = 3 (b2 - b3) - (b0 - b3)
 *
 * With the Seiler points s1 = b0 + d1 and s2 = b3 + d2 it is three lerps:
 *
 *     C(t) = lerp(lerp(b0, b3, t), lerp(s1, s2, t), (1 - t) t)
 *
 * against six for de Casteljau. The first two have the same weight t and
 * disjoint inputs, which is what a texture filter does on its own.
 */
#include "method.h"

static void lerp(const SeilerPoint a, const SeilerPoint b, double t, SeilerPoint at) {
    at[0] = a[0] + t * (b[0] - a[0]);
    at[1] = a[1] + t * (b[1] - a[1]);
}

void seiler_bernstein(const SeilerPoint *b, int degree, double t, SeilerPoint at) {
    static const double k_choose[SEILER_MAX + 1][SEILER_MAX + 1] = {
        {1}, {1, 1}, {1, 2, 1}, {1, 3, 3, 1}, {1, 4, 6, 4, 1}, {1, 5, 10, 10, 5, 1}};
    int i, k;
    at[0] = at[1] = 0.0;
    for (i = 0; i <= degree; ++i) {
        double weight = k_choose[degree][i];
        for (k = 0; k < i; ++k) weight *= t;
        for (k = i; k < degree; ++k) weight *= 1.0 - t;
        at[0] += weight * b[i][0];
        at[1] += weight * b[i][1];
    }
}

void seiler_casteljau(const SeilerPoint *b, int degree, double t, SeilerPoint at) {
    SeilerPoint p[SEILER_MAX + 1];
    int         i, level;
    for (i = 0; i <= degree; ++i) { p[i][0] = b[i][0]; p[i][1] = b[i][1]; }
    for (level = degree; level > 0; --level)
        for (i = 0; i < level; ++i) lerp(p[i], p[i + 1], t, p[i]);
    at[0] = p[0][0]; at[1] = p[0][1];
}

void seiler_polynomial(const SeilerPoint *b, double t, SeilerPoint at) {
    int k;
    for (k = 0; k < 2; ++k) {
        const double p1 = 3.0 * (b[1][k] - b[0][k]), p2 = 3.0 * (b[0][k] - 2.0 * b[1][k] + b[2][k]);
        const double p3 = -b[0][k] + 3.0 * b[1][k] - 3.0 * b[2][k] + b[3][k];
        at[k] = ((p3 * t + p2) * t + p1) * t + b[0][k];
    }
}

void seiler_differences(const SeilerPoint *b, int degree, SeilerPoint *d) {
    const double n = (double)degree, pairs = n * (n - 1.0) / 2.0, fewer = (n - 2.0) * (n - 3.0) / 2.0;
    int          k;
    for (k = 0; k < 2; ++k) {
        d[0][k]          = b[0][k];
        d[degree][k]     = b[degree][k];
        d[1][k]          = n * (b[1][k] - b[0][k]) - (b[degree][k] - b[0][k]);
        d[degree - 1][k] = n * (b[degree - 1][k] - b[degree][k]) - (b[0][k] - b[degree][k]);
    }
    for (k = 0; k < 2 && degree >= 4; ++k) {
        /* eq. 9 and 10; for degree 4 both are the one middle term and agree */
        d[2][k]          = pairs * (b[2][k] - b[1][k]) - fewer * (b[1][k] - b[0][k])
                         - (n - 3.0) * (b[degree - 1][k] - b[degree][k]) - 3.0 * (b[degree - 1][k] - b[1][k]);
        d[degree - 2][k] = pairs * (b[degree - 2][k] - b[degree - 1][k]) - fewer * (b[degree - 1][k] - b[degree][k])
                         - (n - 3.0) * (b[1][k] - b[0][k]) - 3.0 * (b[1][k] - b[degree - 1][k]);
    }
}

void seiler_points(const SeilerPoint *d, int degree, SeilerPoint *left, SeilerPoint *right) {
    int i, k;
    for (k = 0; k < 2; ++k) {
        left[0][k]  = d[0][k];
        right[0][k] = d[degree][k];
        for (i = 1; i <= degree / 2; ++i) {
            left[i][k]  = left[i - 1][k] + d[i][k];
            right[i][k] = right[i - 1][k] + d[degree - i][k];
        }
    }
}

void seiler_from_differences(const SeilerPoint *d, int degree, double t, SeilerPoint at) {
    const double u = (1.0 - t) * t;
    SeilerPoint  pair;
    int          i = degree / 2;
    /* the innermost term: the middle one of an even degree, a lerp of the middle two of an odd */
    lerp(d[i], d[degree - i], t, at);
    while (i-- > 0) {
        lerp(d[i], d[degree - i], t, pair);
        at[0] = pair[0] + u * at[0];
        at[1] = pair[1] + u * at[1];
    }
}

void seiler_from_points(const SeilerPoint *left, const SeilerPoint *right, int degree, double t, SeilerPoint at) {
    const double u = (1.0 - t) * t;
    SeilerPoint  pair;
    int          i = degree / 2;
    lerp(left[i], right[i], t, at);
    while (i-- > 0) {
        lerp(left[i], right[i], t, pair);
        lerp(pair, at, u, at);
    }
}

void seiler_from_offsets(const SeilerPoint *d, int degree, double t, SeilerPoint at) {
    const double u = (1.0 - t) * t;
    SeilerPoint  up, down;
    int          i = degree / 2, k;   /* the first i with degree - 1 <= 2 i */
    for (k = 0; k < 2; ++k) { up[k] = d[i][k]; down[k] = d[degree - i][k]; }
    while (i-- > 0)
        for (k = 0; k < 2; ++k) {
            up[k]   = d[i][k] + u * up[k];
            down[k] = d[degree - i][k] + u * down[k];
        }
    lerp(up, down, t, at);
}

void seiler_spline_points(const SeilerPoint *knot, int count, SeilerPoint *seiler) {
    int j, k;
    for (j = 0; j < count; ++j) {
        const double *before = knot[j > 0 ? j - 1 : 0], *after = knot[j + 1 < count ? j + 1 : count - 1];
        /* b1 = P_j + (P_j+1 - P_j-1) / 6 in d1 = 3 (b1 - b0) - (b3 - b0), plus b0 */
        for (k = 0; k < 2; ++k) seiler[j][k] = 2.0 * knot[j][k] - 0.5 * (before[k] + after[k]);
    }
}

void seiler_spline(const SeilerPoint *knot, const SeilerPoint *seiler, int count, double x, SeilerPoint at) {
    const int    piece = x >= (double)(count - 1) ? count - 2 : (int)x;
    const double t = x - (double)piece;
    SeilerPoint  ends, inner;
    /* the two lerps a linear texture filter does when knots and Seiler points are two textures */
    lerp(knot[piece], knot[piece + 1], t, ends);
    lerp(seiler[piece], seiler[piece + 1], t, inner);
    lerp(ends, inner, (1.0 - t) * t, at);
}
