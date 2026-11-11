/* method.h — Points as Tori on the CPU: the network, the torus at a point,
 * the signed distance. main.c fits the tori with it (--fit) and checks the
 * shaders against it (--check); pat.slang is the same distance in float. */
#ifndef PAT_METHOD_H
#define PAT_METHOD_H

#include <stdbool.h>
#include <stdint.h>

#define PAT_NEIGHBOURS 64u   /* points the network sees around each point, the point itself first */
#define PAT_NEAREST    32u   /* points summed in one distance query */
#define PAT_C          64.0  /* largest exponent of a weight: exp(+-64) fits a float */

/* The network of section 4.3: weights as a flat array, in the order
 * pat_net_forward() reads them (scripts/pat_export_weights.py writes it). */
typedef struct PatNet {
    uint32_t embed, layers, heads, mlp, k;
    float   *w;
} PatNet;

/* One point and its torus, as the shaders read it (80 bytes).
 *
 * The torus is kept by the point it passes through and its two curvatures
 * rather than by centre and radii: a nearly flat or nearly cylindrical
 * neighbourhood gives radii of 1e6 and more, and |x - centre| - R then has no
 * digits left in a float. See pat_torus_distance(). */
typedef struct PatPoint {
    float pos[3], kmin;      /* |curvature| along the equator, 1 / (distance to the axis) */
    float normal[3], kmax;   /* |curvature| around the tube, 1 / r */
    float anchor[3], sign;   /* Q*(0,0), the point moved by a00 along its normal; +1 solid torus, -1 hollow */
    float axis[3], side;     /* u; +1 the anchor is on the outer equator, -1 on the inner */
    float spoke[3], pad;     /* unit vector from the axis to the anchor */
} PatPoint;
_Static_assert(sizeof(PatPoint) == 80, "PatPoint must match pat.slang");

/* The same torus the way the paper writes it (equations 22, 23), in double. */
typedef struct PatTorus {
    double centre[3], axis[3], major, minor, sign;
    double curvature[2];     /* k_min, k_max with their signs */
} PatTorus;

enum { PAT_TORI, PAT_PLANES, PAT_SNAP };   /* what is blended: tori (the paper), tangent planes (eq. 29), the nearest torus alone */

bool pat_net_load(PatNet *net, const char *path);

/* Six coefficients a00 a01 a10 a11 a02 a20 of the height polynomial over the
 * tangent plane of pos[0], from the k points pos[0..k). */
void pat_net_forward(const PatNet *net, const float (*pos)[3], const float (*normal)[3], double coef[6]);

/* The frame the polynomial is written in: s and t perpendicular to n. */
void pat_frame(const double n[3], double s[3], double t[3]);

/* Equations 19 to 22: the torus through p + a00 n whose curvatures are the polynomial's. */
void pat_fit_torus(const float pos[3], const float normal[3], const double coef[6], PatPoint *out, PatTorus *paper);

/* Equation 23 times the sign, from the curvature form; gradient may be NULL. */
double pat_torus_distance(const PatPoint *p, const double x[3], double gradient[3]);

/* Equations 25 and 26 over the PAT_NEAREST nearest points; gradient (appendix D) may be NULL. */
double pat_distance(const PatPoint *points, uint32_t count, const double x[3], int method, double gradient[3]);

#endif
