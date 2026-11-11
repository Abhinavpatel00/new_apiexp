/* method.c — "A Class of C2 Interpolating Splines" (Cem Yuksel, ACM Transactions on Graphics
 * 39(5), 2020), for control points in the plane.
 *
 * Every control point p_i gets an interpolation function F_i: some curve through p_i-1, p_i
 * and p_i+1, with F_i(0) = p_i-1, F_i(pi / 2) = p_i, F_i(pi) = p_i+1 (eq. 1). The curve
 * between p_i and p_i+1 is a blend of the two functions that pass through both (eq. 2):
 *
 *     C_i(theta) = cos^2(theta) F_i(theta + pi / 2) + sin^2(theta) F_i+1(theta),   theta in 0 .. pi / 2
 *
 * The weights and their first derivatives are 0 or 1 at both ends, so position, first and
 * second derivative at a control point are those of one function alone: the curve is C2
 * whatever F is, and a segment depends on four control points.
 *
 * The four functions of the paper's section 4:
 *   Bézier      a quadratic Bézier curve whose point of largest curvature is p_i (eq. 6, 7)
 *   circular    the circle through the three points
 *   elliptical  an ellipse with p_i at the end of one axis, the farther neighbour at the end
 *               of the other, and the nearer neighbour on the other side of p_i
 *   hybrid      the circle while both of its arcs are at most a quarter turn, else the ellipse
 *
 * A function is parametrized by t in 0 .. 1 with p_i at t_i; theta maps to t linearly on
 * each side of p_i. Then the curve is C2 in the paper's global parameter (eq. 8, 9), in
 * which every F_i is linear in t over its whole length.
 *
 * Choices of this file: three points in a line give the parabola that is the straight line
 * through them (the paper: "we simply use a line segment"); arcs are measured by the angle
 * from p_i instead of the alpha and delta of eq. 10; the ellipse is found by bisection on
 * the angle between its first axis and the line to the farther neighbour.
 */
#include "method.h"

#include <math.h>

#define PI 3.14159265358979323846

/* the quadratic Bézier curve through a, p (at parameter t) and c: eq. 6 */
static void parabola(C2Fn *f, const double a[2], const double p[2], const double c[2], double t) {
    int k;
    f->kind = C2_PARABOLA; f->t = t;
    for (k = 0; k < 2; ++k) {
        f->b[0][k] = a[k]; f->b[2][k] = c[k];
        f->b[1][k] = (p[k] - (1.0 - t) * (1.0 - t) * a[k] - t * t * c[k]) / (2.0 * (1.0 - t) * t);
    }
}

void c2_function(C2Fn *f, int type, const double a[2], const double p[2], const double c[2]) {
    const double ax = a[0] - p[0], ay = a[1] - p[1], cx = c[0] - p[0], cy = c[1] - p[1];
    const double la = hypot(ax, ay), lc = hypot(cx, cy), cross = ax * cy - ay * cx;
    int          k;

    if (type == C2_BEZIER) {
        /* eq. 7: the cubic whose one root in 0 .. 1 puts the largest curvature at p */
        const double wx = c[0] - a[0], wy = c[1] - a[1];
        const double k3 = wx * wx + wy * wy, k2 = 3.0 * (wx * ax + wy * ay), k1 = (2.0 * ax - wx) * ax + (2.0 * ay - wy) * ay, k0 = -la * la;
        double       lo = 0.0, hi = 1.0;
        for (k = 0; k < 60; ++k) {
            const double t = 0.5 * (lo + hi);
            if (((k3 * t + k2) * t + k1) * t + k0 < 0.0) lo = t; else hi = t;
        }
        parabola(f, a, p, c, 0.5 * (lo + hi));
        return;
    }
    if (fabs(cross) <= 1e-12 * la * lc) {
        parabola(f, a, p, c, la / (la + lc));
        return;
    }

    {
        /* the circle through the three points; v is u turned the way the curve runs */
        const double qx = (la * la * cy - lc * lc * ay) / (2.0 * cross), qy = (lc * lc * ax - la * la * cx) / (2.0 * cross);
        const double way = cross < 0.0 ? 1.0 : -1.0, ux = -qx, uy = -qy, vx = -way * uy, vy = way * ux;
        const double back = atan2((ax - qx) * vx + (ay - qy) * vy, (ax - qx) * ux + (ay - qy) * uy);
        const double ahead = atan2((cx - qx) * vx + (cy - qy) * vy, (cx - qx) * ux + (cy - qy) * uy);
        f->kind = C2_CIRCLE;
        f->b[0][0] = p[0] + qx; f->b[0][1] = p[1] + qy; f->b[1][0] = ux; f->b[1][1] = uy; f->b[2][0] = vx; f->b[2][1] = vy;
        f->from = back < 0.0 ? back : back - 2.0 * PI;
        f->to = ahead > 0.0 ? ahead : ahead + 2.0 * PI;
        f->t = -f->from / (f->to - f->from);
    }
    if (type == C2_CIRCULAR || (type == C2_HYBRID && -f->from <= 0.5 * PI && f->to <= 0.5 * PI)) return;

    {
        /* A frame with p at the origin, the farther neighbour at (d, 0) and the nearer one at
         * (x, y), y > 0. The centre is on the circle that has p and the farther neighbour
         * across a diameter: at d cos(beta) (cos(beta), sin(beta)). In the ellipse's axes the
         * nearer neighbour is at (m, n); it is on the ellipse when m m + n n = 1. */
        const int    far_is_c = lc >= la;
        const double d = far_is_c ? lc : la, ex = (far_is_c ? cx : ax) / d, ey = (far_is_c ? cy : ay) / d;
        const double nx = far_is_c ? ax : cx, ny = far_is_c ? ay : cy, side = -ey * nx + ex * ny < 0.0 ? -1.0 : 1.0;
        const double fx = -side * ey, fy = side * ex, x = nx * ex + ny * ey, y = nx * fx + ny * fy;
        double       lo = 0.0, hi = fmin(fmin(atan2(y, x), 0.5 * PI), atan2(d - x, y)), beta, m, n, qx, qy, at;
        for (k = 0; k < 60; ++k) {
            beta = 0.5 * (lo + hi);
            m = 1.0 - (x * cos(beta) + y * sin(beta)) / (d * cos(beta));
            n = (x * sin(beta) - y * cos(beta)) / (d * sin(beta));
            if (m * m + n * n > 1.0) lo = beta; else hi = beta;
        }
        beta = 0.5 * (lo + hi);
        m = 1.0 - (x * cos(beta) + y * sin(beta)) / (d * cos(beta));
        n = (x * sin(beta) - y * cos(beta)) / (d * sin(beta));
        at = atan2(n, m);   /* between -pi / 2 and 0 */
        qx = d * cos(beta) * (cos(beta) * ex + sin(beta) * fx); qy = d * cos(beta) * (cos(beta) * ey + sin(beta) * fy);
        f->kind = C2_ELLIPSE;
        f->b[0][0] = p[0] + qx; f->b[0][1] = p[1] + qy; f->b[1][0] = -qx; f->b[1][1] = -qy;
        f->b[2][0] = (far_is_c ? 1.0 : -1.0) * (d * ex - qx); f->b[2][1] = (far_is_c ? 1.0 : -1.0) * (d * ey - qy);
        f->from = far_is_c ? at : -0.5 * PI;
        f->to = far_is_c ? 0.5 * PI : -at;
        f->t = -f->from / (f->to - f->from);
    }
}

void c2_at(const C2Fn *f, double theta, double out[2]) {
    const double t = theta < 0.5 * PI ? f->t * theta / (0.5 * PI) : f->t + (1.0 - f->t) * (theta - 0.5 * PI) / (0.5 * PI);
    const double phi = f->from + t * (f->to - f->from);
    int          k;
    for (k = 0; k < 2; ++k)
        out[k] = f->kind == C2_PARABOLA ? (1.0 - t) * (1.0 - t) * f->b[0][k] + 2.0 * (1.0 - t) * t * f->b[1][k] + t * t * f->b[2][k]
                                        : f->b[0][k] + cos(phi) * f->b[1][k] + sin(phi) * f->b[2][k];
}

void c2_segment(const C2Fn *f, const C2Fn *g, double theta, double out[2]) {
    const double c = cos(theta) * cos(theta), s = sin(theta) * sin(theta);
    double       a[2], b[2];
    c2_at(f, theta + 0.5 * PI, a);
    c2_at(g, theta, b);
    out[0] = c * a[0] + s * b[0]; out[1] = c * a[1] + s * b[1];
}
