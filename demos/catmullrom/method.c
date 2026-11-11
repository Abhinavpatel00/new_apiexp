/* method.c — "On the Parameterization of Catmull-Rom Curves" (Cem Yuksel, Scott Schaefer,
 * John Keyser; SIAM / ACM Joint Conference on Geometric and Physical Modeling 2009).
 *
 * A cubic Catmull-Rom curve passes through its control points; the piece between P1 and P2
 * depends on P0 .. P3 and on the parameter value t_i given to each point. The paper studies
 *
 *     t_i+1 = t_i + |P_i+1 - P_i|^alpha                                   (eq. 1)
 *
 * and proves that alpha = 1/2 (centripetal) is the only value for which a piece can never
 * have a cusp or cross itself.
 *
 * cr_point is Barry and Goldman's pyramid (figure 3): three rounds of linear interpolation,
 * first between neighbouring points over their own parameter interval, then over two
 * intervals, then over the middle one. cr_bezier is the paper's eq. 2.
 */
#include "method.h"

#include <math.h>

void cr_knots(const double p[4][2], double alpha, double t[4]) {
    int i;
    t[0] = 0.0;
    for (i = 0; i < 3; ++i) t[i + 1] = t[i] + pow(hypot(p[i + 1][0] - p[i][0], p[i + 1][1] - p[i][1]), alpha);
}

void cr_point(const double p[4][2], const double t[4], double u, double out[2]) {
    int k;
    for (k = 0; k < 2; ++k) {
        const double l01 = ((t[1] - u) * p[0][k] + (u - t[0]) * p[1][k]) / (t[1] - t[0]);
        const double l12 = ((t[2] - u) * p[1][k] + (u - t[1]) * p[2][k]) / (t[2] - t[1]);
        const double l23 = ((t[3] - u) * p[2][k] + (u - t[2]) * p[3][k]) / (t[3] - t[2]);
        const double l012 = ((t[2] - u) * l01 + (u - t[0]) * l12) / (t[2] - t[0]);
        const double l123 = ((t[3] - u) * l12 + (u - t[1]) * l23) / (t[3] - t[1]);
        out[k] = ((t[2] - u) * l012 + (u - t[1]) * l123) / (t[2] - t[1]);
    }
}

void cr_bezier(const double p[4][2], double alpha, double b[4][2]) {
    /* a, m, c: d1^alpha, d2^alpha, d3^alpha, the three parameter intervals */
    const double a = pow(hypot(p[1][0] - p[0][0], p[1][1] - p[0][1]), alpha), m = pow(hypot(p[2][0] - p[1][0], p[2][1] - p[1][1]), alpha);
    const double c = pow(hypot(p[3][0] - p[2][0], p[3][1] - p[2][1]), alpha);
    int          k;
    for (k = 0; k < 2; ++k) {
        b[0][k] = p[1][k];
        b[1][k] = (a * a * p[2][k] - m * m * p[0][k] + (2.0 * a * a + 3.0 * a * m + m * m) * p[1][k]) / (3.0 * a * (a + m));
        b[2][k] = (c * c * p[1][k] - m * m * p[3][k] + (2.0 * c * c + 3.0 * c * m + m * m) * p[2][k]) / (3.0 * c * (c + m));
        b[3][k] = p[2][k];
    }
}
