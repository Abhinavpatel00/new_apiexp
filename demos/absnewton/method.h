/* method.h — projected Newton on the stable Neo-Hookean energy, with the two ways of filtering
 * each element's eigenvalues that Chen et al. 2024 compare. See method.c. */
#ifndef ABSNEWTON_METHOD_H
#define ABSNEWTON_METHOD_H
#include <stdbool.h>
#include <stdint.h>

typedef enum NhFilter {
    NH_CLAMP,   /* eq. 5: eigenvalues below epsilon become epsilon (Teran et al. 2005) */
    NH_ABS,     /* eq. 10: every eigenvalue becomes its absolute value                 */
    NH_NONE     /* the Hessian as it is                                                */
} NhFilter;

typedef struct NhTet {
    uint32_t node[4];
    double   grad[4][3];   /* F = sum_i x_i grad_i^T */
    double   volume;
} NhTet;

typedef struct Nh {
    uint32_t nodes, tets;
    double  *x;          /* 3 a node                                       */
    uint8_t *held;       /* 3 a node: 1 = this coordinate does not move    */
    NhTet   *tet;
    double   mu, lambda; /* eq. 13                                         */
    NhFilter filter;
    double   epsilon;    /* for NH_CLAMP; the paper's experiments use 0    */
    /* results of the last nh_newton */
    double   energy;     /* before the step                                */
    double   step;       /* the line search's step length                  */
    uint32_t replaced;   /* pivots of the factorisation that were not positive and were replaced */
    /* owned by nh_mesh / nh_newton */
    double  *gradient, *direction, *trial, *hessian;
} Nh;

/* Eigenvalues and eigenvectors (columns of `vectors`) of the symmetric n x n matrix `a`, by
 * Jacobi rotations. `a` is overwritten. */
void nh_eigen(double *a, int n, double *values, double *vectors);

/* Replace the symmetric n x n matrix `h` (n <= 12) by U f(Lambda) U^T. */
void nh_filter(double *h, int n, NhFilter filter, double epsilon);

/* The second derivative of eq. 13 with respect to F; row and column 3 a + b stand for F_ab. */
void nh_hessian_f(double mu, double lambda, const double f[3][3], double h[9][9]);

/* Set the tetrahedra from a mesh at rest and allocate everything; x = rest, nothing held. */
void nh_mesh(Nh *s, uint32_t nodes, const double *rest, uint32_t tets, const uint32_t *tet);

/* The energy at node positions x, and its gradient if `gradient` is not NULL (zero on held
 * coordinates). */
double nh_energy(const Nh *s, const double *x, double *gradient);

/* One iteration of projected Newton with a backtracking line search. Returns the Newton
 * decrement -d.g / 2 of the direction it took: the paper's measure of convergence. */
double nh_newton(Nh *s);
#endif /* ABSNEWTON_METHOD_H */
