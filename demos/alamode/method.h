/* method.h — "Actuators A La Mode: Modal Actuations for Soft Body Locomotion" (Benchekroun, Xie, Liu, Grinspun,
 * Andrews, Zordan; SIGGRAPH Asia 2024, arXiv 2405.18609). See method.c. */
#ifndef ALAMODE_METHOD_H
#define ALAMODE_METHOD_H
#include <stdbool.h>
#include <stdint.h>

typedef struct AmSetup {
    int    weights;      /* w skinning eigenmodes; the simulation then has 12 w numbers (appendix A)        */
    int    modes;        /* m vibration modes that the actuation may use (eq. 4)                            */
    int    passive;      /* clusters of tetrahedra that share a rotation in the elastic energy              */
    int    active;       /* the same for the actuation energy (eq. 11)                                      */
    int    samples;      /* nodes that can touch the ground y = 0 (0: there is no ground)                   */
    int    iterations;   /* local-global iterations a step                                                  */
    double step;         /* s                                                                               */
    double density;      /* kg/m^3                                                                          */
    double stiffness;    /* mu of the elastic energy, Pa                                                    */
    double strength;     /* gamma of the actuation energy, Pa                                               */
    double gravity;      /* m/s^2 along -y                                                                  */
    double slide;        /* u of eq. 47: the share of its sideways speed that a touching node keeps a step  */
    bool   pull;         /* no actuation energy; springs pull every node to its target instead (figure 3)   */
} AmSetup;

typedef struct Am {
    AmSetup         setup;
    int             nodes, tets, p;      /* p = 4 w: the numbers of the simulation for one coordinate */
    const double   *rest;                /* the caller's, 3 a node */
    const uint32_t *tet;                 /* the caller's, 4 a tetrahedron */
    double         *mass, *volume;       /* a node; a tetrahedron */
    double         *grad;                /* gradients of the four shape functions, 12 a tetrahedron */
    double         *shape;               /* D: modes columns of 3 a node, each with largest displacement 1 */
    double         *stiff;               /* eigenvalues of eq. 4, 1/s^2: 6 of rigid motion, then the modes' */
    double         *weight;              /* W: weights columns of one a node (eq. 19) */
    double         *basis;               /* Phi, nodes x p: node i is at Z Phi_i for the 3 x p matrix Z */
    double         *strain;              /* Psi, tets x p x 3: the deformation gradient of a tetrahedron is Z Psi */
    int            *passive, *active;    /* the cluster of each tetrahedron */
    int            *sample;              /* the nodes that can touch the ground */
    double          total, spring;       /* mass; 1/s^2 of the springs of `pull` */
    double         *inertia, *system, *inverse;     /* p x p: Phi' M Phi; the matrix of the global step; its inverse */
    double         *heavy;                          /* p: Phi' m */
    double         *fit_passive, *fit_whole;        /* passive x p x 3, p x 3: Z times it is the sum of mu V F */
    double         *fit_active;                     /* active x (modes + 1) x p x 3 (the tensor of eq. 13) */
    double         *pulled;                         /* (modes + 1) x 3 x p */
    double         *touch, *touch_force, *touch_move;   /* samples x p: Phi_s, K^-1 Phi_s, K^-2 Phi_s */
    double         *start, *z, *before;             /* 3 x p: rest; now; a step ago */
    double         *turn_passive, *turn_active;     /* 9 a cluster: the rotations of the last local step */
    bool           *touching;                       /* a sample: held on the ground in the last step */
    int             contacts, sweeps[2];            /* how many; iterations of the two eigenvalue problems */
    double         *scratch;
} Am;

/* `faces` surface triangles (3 nodes each) say which nodes are outside; the ground samples are among them. */
Am  *am_open(int nodes, const double *rest, int tets, const uint32_t *tet, int faces, const uint32_t *face, const AmSetup *setup);
void am_close(Am *m);

/* The rotation nearest to h (3 x 3, row after row): the one with the largest trace of r' h. */
void am_polar(const double h[9], double r[9]);

/* Back to the rest shape with no velocity. */
void am_reset(Am *m);

/* One time step under the actuation a (`modes` numbers: the target shape is rest + D a, eq. 5). */
void am_step(Am *m, const double *a);

/* Node positions from the numbers of the simulation, x = B z (eq. 7). */
void am_lift(const Am *m, const double *z, double *x);

/* The energy that a step from the present state minimises (eq. 14, without what does not depend on z), at z, with
 * the best rotations for that z. slope (3 x p, may be NULL): its derivative. No ground. */
double am_energy(const Am *m, const double *z, const double *a, double *slope);

/* Momentum and angular momentum about the origin. */
void am_momentum(const Am *m, double linear[3], double angular[3]);

/* Eq. 6: a_i(t) = sum over j < waves of A_ij sin(2 pi (t / T_ij + theta_ij)); gait holds A, T, theta for mode 0
 * wave 0, then mode 0 wave 1, ... The first half second fades in. */
void am_wave(const double *gait, int modes, int waves, double t, double *a);

/* Eq. 17: from rest, `steps` steps under the gait. Returns J = -(way of the centre of mass along x) x (the least
 * of the body's forward direction . x over the episode, not below 0). */
double am_episode(Am *m, const double *gait, int waves, int steps, double *way, double *facing);

/* CMA-ES (Hansen 2006), the search the paper uses for the gait. */
typedef struct AmSearch {
    int      n, lambda, generation;
    double   sigma;
    double  *mean, *cov, *axes, *length, *path_sigma, *path_cov, *ask, *unit;
    uint64_t random;
} AmSearch;

AmSearch     *am_search_open(int n, int lambda, const double *mean, double sigma, uint64_t seed);
void          am_search_close(AmSearch *s);
const double *am_search_ask(AmSearch *s);                       /* lambda rows of n to try */
void          am_search_tell(AmSearch *s, const double *cost);  /* their costs; lower is better */

#endif /* ALAMODE_METHOD_H */
