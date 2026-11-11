/* method.h — cubic Catmull-Rom curves with the parameterizations between uniform and
 * chordal (Yuksel, Schaefer and Keyser 2009). See method.c. */
#ifndef CATMULLROM_METHOD_H
#define CATMULLROM_METHOD_H

/* The parameter values of four consecutive control points: t[0] = 0 and each next one
 * |P_i+1 - P_i|^alpha further (eq. 1). alpha 0 uniform, 0.5 centripetal, 1 chordal. */
void cr_knots(const double p[4][2], double alpha, double t[4]);

/* The curve between p[1] and p[2] at u in t[1] .. t[2], by the pyramid of the paper's figure 3. */
void cr_point(const double p[4][2], const double t[4], double u, double out[2]);

/* The same piece as a cubic Bézier curve (eq. 2). */
void cr_bezier(const double p[4][2], double alpha, double b[4][2]);

#endif /* CATMULLROM_METHOD_H */
