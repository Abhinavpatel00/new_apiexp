/* method.h — a class of C2 interpolating splines (Yuksel 2020), in the plane. See method.c. */
#ifndef C2SPLINE_METHOD_H
#define C2SPLINE_METHOD_H

enum { C2_BEZIER, C2_CIRCULAR, C2_ELLIPTICAL, C2_HYBRID };   /* the paper's four interpolation functions */
enum { C2_PARABOLA, C2_CIRCLE, C2_ELLIPSE };                 /* what a C2Fn holds */

/* An interpolation function: the curve through three consecutive control points. */
typedef struct C2Fn {
    int    kind;
    double b[3][2];    /* parabola: the Bézier points b0, b1, b2; arc: centre q, then u and v of q + cos(phi) u + sin(phi) v */
    double from, to;   /* arc: phi at the first and at the last point; the middle point is at phi 0 */
    double t;          /* the local parameter (0 .. 1) at the middle point: the paper's t_i */
} C2Fn;

/* The interpolation function of `type` through a, p and c. */
void c2_function(C2Fn *f, int type, const double a[2], const double p[2], const double c[2]);

/* F(theta): a at 0, p at pi / 2, c at pi (eq. 1). */
void c2_at(const C2Fn *f, double theta, double out[2]);

/* The curve between two control points from the functions centred on them, theta in 0 .. pi / 2 (eq. 2). */
void c2_segment(const C2Fn *f, const C2Fn *g, double theta, double out[2]);

#endif /* C2SPLINE_METHOD_H */
