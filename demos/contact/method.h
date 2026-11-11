/* method.h — rigid bodies in contact with friction (Andrews, Erleben, Ferguson 2022). See method.c. */
#ifndef CONTACT_METHOD_H
#define CONTACT_METHOD_H
#include <stdbool.h>

#define CF_SPHERE 0
#define CF_BOX    1

#define CF_SET_BOX     0   /* friction bounded in each direction by itself: the BLCP of section 1.7 */
#define CF_SET_ELLIPSE 1   /* friction in an ellipse, with torsion an ellipsoid: section 3.4.1 */

#define CF_JACOBI       0  /* algorithm 11: every contact from the last iterate, one r for all */
#define CF_GAUSS_SEIDEL 1  /* algorithm 12; with the box set and r = 1 / A_ii it is projected Gauss-Seidel */
#define CF_NNCG         2  /* algorithm 10: conjugate gradients on what a Gauss-Seidel sweep changes */

typedef struct CfBody {
    int    shape;
    double half[3];                    /* box: half extents; sphere: the radius three times */
    double p[3], q[4], v[3], w[3];     /* centre, orientation (w, x, y, z), velocity, angular velocity */
    double mass_inv, inertia_inv[3];   /* 0: the body does not move */
    double mu[3];                      /* friction along its own x axis, across it, and about the normal (m) */
    double r[9], inertia_inv_world[9]; /* from q: cf_place */
} CfBody;

typedef struct CfContact {
    int    a, b;                       /* a < b */
    double point[3], gap;              /* gap < 0: they overlap by that much */
    double frame[9];                   /* normal from a to b, then two tangents: n, t, n x t */
    double mu[3];                      /* along t, along n x t, about n */
} CfContact;

typedef struct Cf {
    CfBody    *body;
    int        bodies;
    CfContact *contact;
    int        contacts;
    double     gravity, dt;
    double     margin;                 /* contacts are made when the gap is below this */
    double     erp, slop;              /* share of an overlap taken out in a step; overlap that is left alone */
    int        project;                /* sweeps that move the bodies apart after the step (section 3.4.6); 0: the share is
                                          added to the velocity that the impulses must reach (section 1.9, eq. 59) */
    /* the solver */
    int        set, method, iterations;
    double     omega;                  /* r = omega / A_ii (or omega / largest eigenvalue for Jacobi) */
    double     tolerance;              /* stop when an iteration changes no impulse by more than this times the largest */
    bool       warm;                   /* cf_problem starts lambda from the last step's impulses; else from 0 */
    bool       adapt;                  /* an iteration that changes more than the last is undone and r made smaller
                                          (section 3.4.4); not for NNCG */
    int       *order;                  /* the order a sweep takes the contacts in; cf_problem sets 0, 1, 2 ... */
    /* what the last cf_solve did */
    int        used, rollbacks;
    double     change;                 /* largest change of an impulse in its last iteration */
    double    *lambda;                 /* 4 a contact: normal, t, n x t, and the torque about n divided by mu[2] */
    /* the method's own */
    int        room, old_room, olds;
    CfContact *old;
    double    *old_lambda;
    double    *row, *back, *diag, *bias, *r, *r_local, *free, *push, *scratch;
} Cf;

Cf  *cf_open(void);
void cf_close(Cf *c);

/* A new body at p, not turned, at rest; density 0: it does not move. Returns its index. */
int cf_add(Cf *c, int shape, const double half[3], double density, const double p[3]);

/* After q was changed: the rotation matrix and the inverse inertia in the world. */
void cf_place(CfBody *b);

/* Contacts between two bodies (section 2.1): at most 8. Fills point, gap and the normal. */
int cf_collide(const CfBody *a, const CfBody *b, double margin, CfContact out[8]);

/* The closest point to z in the ellipsoid with half axes s (algorithm 13); an axis of 0 is allowed. In place. */
void cf_ellipsoid(const double s[3], double z[3]);

/* One step is cf_step. Its parts, for looking at one contact problem with several solvers:
 * cf_detect   all contacts at the bodies' places
 * cf_problem  the rows of J, b = J (u + h f / M) and the stabilisation, for the bodies' velocities
 *             and the first guess: with `warm` the impulses of the contacts that the last cf_detect replaced,
 *             each given away once (so only the first cf_problem after a cf_detect starts warm), else 0
 * cf_solve    improves lambda (cf_problem set the first guess); `history`, if not NULL, gets cf_merit after every iteration
 * cf_merit    largest |lambda - prox(lambda - (A lambda + b) / A_ii)|: 0 at a solution of the chosen set
 * cf_apply    the velocities after the impulses */
void   cf_detect(Cf *c);
void   cf_problem(Cf *c);
void   cf_solve(Cf *c, double *history);
double cf_merit(const Cf *c);
void   cf_apply(Cf *c);
void   cf_step(Cf *c);

#endif /* CONTACT_METHOD_H */
