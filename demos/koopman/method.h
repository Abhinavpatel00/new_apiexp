/* method.h — low-rank Koopman deformables (Chang, Chen, Grinspun, Chiaramonte 2026). See method.c. */
#ifndef KOOPMAN_METHOD_H
#define KOOPMAN_METHOD_H
#include <complex.h>
#include <stdbool.h>
#include <stdint.h>

/* The full-space body that makes the data: tetrahedra of stable Neo-Hookean material, lumped masses,
 * implicit Euler. */
typedef struct KdBody {
    int             nodes, tets;
    const uint32_t *tet;          /* 4 a tetrahedron (the caller's) */
    double         *shape;        /* 9 a tetrahedron: the inverse of its rest edge matrix, row after row */
    double         *volume;       /* 1 a tetrahedron */
    double         *mass;         /* 1 a node */
    bool           *fixed;        /* 1 a node: it does not move */
    double          mu, lambda;   /* of the energy in method.c (not the Lame parameters) */
    double          damping;      /* 1/s: force -damping * mass * velocity */
    double         *rest;         /* 3 a node */
    double         *u, *v;        /* 3 a node: displacement, velocity */
    int             newton, cg;   /* iterations of the last step */
    int             band;         /* the largest distance between the numbers of two coupled coordinates */
    double         *stiff;        /* 3 nodes x (band + 1): the stiffness at rest, row i holds columns i - band .. i */
    double         *factor;       /* the same shape: the Cholesky factor of factor_weight * mass + stiff */
    double          factor_weight; /* (1 + h damping) / h^2 of the step that made `factor` */
    double         *scratch;      /* 30 a node */
} KdBody;

/* `rest` is copied, `tet` is kept. Nodes are free and at rest; set `fixed` and `damping` after. */
KdBody *kd_body_open(int nodes, const double *rest, int tets, const uint32_t *tet, double young, double poisson, double density);
void    kd_body_close(KdBody *b);

/* Elastic energy at displacement u. If `force` is given: minus its gradient, 3 a node. If `along` is given too:
 * `change` is the change of the force when u changes by `along`, to first order. */
double kd_body_energy(const KdBody *b, const double *u, double *force, const double *along, double *change);

/* One implicit Euler step of length h. Before it every node's velocity gets h * accel (3 a node; NULL: none).
 * The first step, and every step with another h or damping than the one before, also factors a band matrix;
 * `fixed` must be set by then. */
void kd_body_step(KdBody *b, double h, const double *accel);

/* The operator. */
typedef struct Kd {
    int             dim, pairs;   /* size of a state, number of snapshot pairs */
    double         *left;         /* dim x pairs, column after column: U of X = U S V'; the first `rank` columns are U_r */
    double         *right;        /* pairs x pairs, column after column: V */
    double         *sigma;        /* pairs: singular values, largest first */
    double         *y;            /* dim x pairs: the second states of the pairs */
    /* set by kd_rank */
    int             rank;
    double         *reduced;      /* rank x rank, row after row: K' = U_r' Y V_r S_r^-1 (eq. 13) */
    double complex *lambda;       /* rank: eigenvalues of K' (eq. 14); may be edited (section 7.2) */
    double complex *modes;        /* rank x rank, row after row: the eigenvectors of K' are its columns */
    double complex *inverse;      /* rank x rank: the inverse of `modes` */
    int             sweeps;       /* QR sweeps the eigenvalues took */
} Kd;

/* Dynamic mode decomposition of the pairs x[:, t] -> y[:, t] (dim x pairs each, column after column, copied;
 * dim >= pairs): the singular value decomposition of x (eq. 12). A forced pair has its kick already added to x
 * (eq. 19). */
Kd  *kd_open(int dim, int pairs, const double *x, const double *y);
void kd_close(Kd *k);

/* Truncates to `rank` (at most min(dim, pairs)) and finds K', its eigenvalues and eigenvectors (eq. 13 to 15). */
void kd_rank(Kd *k, int rank);

/* Reduced coordinates z = Phi^* x (rank complex numbers), and back: x = Re(Phi z). kd_lift returns the largest
 * |Im(Phi z)|: 0 up to rounding for a z that came from a real state by whole steps. */
void   kd_project(const Kd *k, const double *x, double complex *z);
double kd_lift(const Kd *k, const double complex *z, double *x);

/* `steps` steps at once (eq. 20, 21): z <- lambda^steps z. With `kick` (the reduced coordinates of a kick that
 * is added to the state before every one of the steps, eq. 19):
 * z <- lambda^steps z + (lambda + ... + lambda^steps) kick, the sum in closed form (eq. 25).
 * `steps` need not be whole: lambda^steps = exp(steps log lambda). */
void kd_advance(const Kd *k, double steps, const double complex *kick, double complex *z);

/* The real operator of section 5.2 for `steps` steps: 2 rank x 2 rank, row after row, acting on [Re z; Im z]. It
 * advances, lifts, drops the imaginary part of the state and projects back. */
void kd_real(const Kd *k, double steps, double *out);

/* Section 7.3. Control j adds kicks[:, j] (dim x count, column after column) times c[j] to the state before
 * every step. Finds the c that after `steps` steps from the reduced state z (NULL: rest) bring the state's
 * entries pick[i] to goal[i] (`picks` of them), in the least squares sense, where every entry of the state's
 * second half also counts, `still` times its square. Returns the root mean square miss of the picked entries
 * that the operator itself predicts. */
double kd_control(const Kd *k, double steps, const double complex *z, int count, const double *kicks, int picks, const int *pick, const double *goal, double still, double *c);

#endif /* KOOPMAN_METHOD_H */
