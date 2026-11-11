/* method.h — regularized Green's functions of anisotropic linear elasticity as a sum of
 * spherical harmonics (Chen and Desbrun 2022). See method.c. */
#ifndef GOGREEN_METHOD_H
#define GOGREEN_METHOD_H
#include <stdbool.h>

#define GG_DEGREES 22                          /* degrees 0 .. 21 can be kept */
#define GG_TERMS   (GG_DEGREES * GG_DEGREES)   /* harmonic (l, m) is entry l l + l + m */

typedef struct Gg {
    double C[3][3][3][3];        /* elasticity tensor: C_ijkl d2u_k / dx_l dx_j + f_i = 0 (eq. 1) */
    int    lmax;                 /* loads use even degrees up to lmax, affine loads odd ones up to lmax + 1 */
    double epsilon;              /* width of the Gaussian load */
    double weight[GG_DEGREES];   /* a factor on the radial function of each degree (1: the Gaussian's) */
    double P[GG_TERMS][3][3];    /* material matrices of eq. 10, for real harmonics; even degrees */
    double Q[GG_TERMS][3][3][3]; /* Q[.][p]: those of eq. 12 for d/dx_p; odd degrees */
} Gg;

/* Elasticity tensors. */
void gg_isotropic(double C[3][3][3][3], double mu, double nu);
void gg_voigt(double C[3][3][3][3], const double V[6][6]);     /* from the 6 x 6 form, order 11 22 33 23 13 12 */
void gg_rotate_z(double C[3][3][3][3], double angle);          /* the same material turned about z */

/* Real spherical harmonics of degrees 0 .. lmax in the unit direction d; y has (lmax + 1)^2 entries. */
void gg_harmonics(int lmax, const double d[3], double *y);

/* Sets lmax, epsilon, weights of 1 and the matrices P and Q of s->C, by a quadrature of
 * n x 2n points on the sphere (Gauss-Legendre in cos theta, even in the angle about z). */
void gg_material(Gg *s, int lmax, double epsilon, int n);

/* Radial functions of a Gaussian load (eq. 13): for loads, and for affine loads. */
double gg_radial(int l, double r, double epsilon);
double gg_radial_affine(int l, double r, double epsilon);

/* G(x) of eq. 10: the displacement at x for a load h at the origin is G h. */
void gg_green(const Gg *s, const double x[3], double G[3][3]);

/* D[p][i][j] = dG_ij / dx_p of eq. 12: an affine load H at the origin displaces x by
 * u_i = sum_jp D[p][i][j] H[j][p] (eq. 11). */
void gg_gradient(const Gg *s, const double x[3], double D[3][3][3]);

/* Loads h (3 each) at the k points p (3 each) that displace point i by u_i (section on
 * constrained deformation): a dense 3k x 3k solve. False if the system is singular. */
bool gg_fit(const Gg *s, int k, const double *p, const double *u, double *h);
#endif /* GOGREEN_METHOD_H */
