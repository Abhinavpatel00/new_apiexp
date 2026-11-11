/* method.h — Projective Dynamics (Bouaziz et al. 2014) on the CPU. See method.c. */
#ifndef PROJDYN_METHOD_H
#define PROJDYN_METHOD_H
#include <stdbool.h>
#include <stdint.h>

/* A tetrahedron's two constraints share one map: q -> its deformation gradient F. */
typedef struct PdTet {
    uint32_t node[4];
    double   grad[4][3];   /* F = sum_i q_i grad_i^T: the gradients of the four shape functions */
    double   volume;
} PdTet;

typedef struct Pd {
    uint32_t nodes, tets;
    double  *q, *v;            /* 3 a node                                                    */
    double  *mass;             /* a node; a quarter of each tetrahedron it belongs to         */
    PdTet   *tet;
    double  *target;           /* 3 a node: where an attached node is held                    */
    double  *attach;           /* a node: weight of that attachment, 0 = free                 */
    double   h, gravity;
    double   stiffness;        /* strain constraints: w = stiffness x volume, Pa              */
    double   low, high;        /* the singular values of F the strain constraint allows       */
    double   volume_stiffness; /* volume constraints: w = this x volume; 0 = none             */
    double   volume_low, volume_high;   /* the det F the volume constraint allows             */
    uint32_t iterations;
    /* owned by pd_factor / pd_step */
    double  *factor;           /* nodes x nodes: Cholesky factor of M / h^2 + sum w A^T A     */
    double  *inertia, *rhs, *project;   /* s_n; right-hand side; 9 a tetrahedron x 2: the projections */
} Pd;

/* F = U diag(sigma) V^T with det U = det V = 1 and sigma_0 >= sigma_1 >= |sigma_2|; sigma_2 is
 * negative when F turns the tetrahedron inside out. */
void pd_svd(const double f[3][3], double u[3][3], double sigma[3], double v[3][3]);

/* The matrix nearest to F with all singular values in [low, high] (section 5.1, eq. 22 and 23)
 * and the one nearest with a determinant in [low, high] (section 5.2, eq. 24 to 26). */
void pd_project_strain(const double f[3][3], double low, double high, double out[3][3]);
void pd_project_volume(const double f[3][3], double low, double high, double out[3][3]);

/* Build and factor the system matrix. Call again after changing h, a stiffness or an attachment
 * weight; nothing else changes it. Returns the largest |L L^T - A| / largest |A|. */
double pd_factor(Pd *s);

/* The objective one time step minimises (eq. 8 with the projections taken at q), and its
 * gradient with respect to q if `gradient` is not NULL. Uses s->inertia. */
double pd_energy(const Pd *s, const double *q, double *gradient);

/* One time step: Algorithm 1. `trace`, if not NULL, receives iterations + 1 values of the
 * objective: before the first iteration and after each one. */
void pd_step(Pd *s, double *trace);
#endif /* PROJDYN_METHOD_H */
