/* method.h — Seiler's interpolation for polynomial curves. See method.c. */
#ifndef SEILER_METHOD_H
#define SEILER_METHOD_H

enum { SEILER_MAX = 5 };   /* the highest degree the talk gives difference terms for */

typedef double SeilerPoint[2];

/* A Bezier curve of `degree` 2..5 with control points b[0..degree], three usual ways. */
void seiler_bernstein(const SeilerPoint *b, int degree, double t, SeilerPoint at);
void seiler_casteljau(const SeilerPoint *b, int degree, double t, SeilerPoint at);
void seiler_polynomial(const SeilerPoint *b, double t, SeilerPoint at);   /* cubic only */

/* The difference terms d[1..degree-1] (eq. 3, 4, 7 to 10); d[0] and d[degree] are the end points. */
void seiler_differences(const SeilerPoint *b, int degree, SeilerPoint *d);

/* Seiler points: left[0] = b0, left[i] = left[i-1] + d[i]; right[0] = b_degree,
 * right[i] = right[i-1] + d[degree-i]; i up to degree / 2. */
void seiler_points(const SeilerPoint *d, int degree, SeilerPoint *left, SeilerPoint *right);

/* The curve from difference terms (eq. 5 and 6), from Seiler points by lerps only, and from
 * difference terms by offsets and one lerp at the end (eq. 11 and 12). */
void seiler_from_differences(const SeilerPoint *d, int degree, double t, SeilerPoint at);
void seiler_from_points(const SeilerPoint *left, const SeilerPoint *right, int degree, double t, SeilerPoint at);
void seiler_from_offsets(const SeilerPoint *d, int degree, double t, SeilerPoint at);

/* A uniform Catmull-Rom spline through knot[0..count-1] stored as the talk says: one Seiler
 * point a knot. seiler_spline_points fills them (the first and last knot are repeated beyond
 * the ends); seiler_spline evaluates at x in [0, count - 1] with three lerps. */
void seiler_spline_points(const SeilerPoint *knot, int count, SeilerPoint *seiler);
void seiler_spline(const SeilerPoint *knot, const SeilerPoint *seiler, int count, double x, SeilerPoint at);
#endif /* SEILER_METHOD_H */
