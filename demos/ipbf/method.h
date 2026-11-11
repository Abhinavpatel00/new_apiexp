/* method.h — Implicit Position-Based Fluids (Diaz et al. 2025) in the plane, and Position Based
 * Fluids (Macklin and Mueller 2013) to compare with. See method.c. */
#ifndef IPBF_METHOD_H
#define IPBF_METHOD_H
#include <stdbool.h>
#include <stdint.h>

#define FL_NEIGHBOURS 192   /* most neighbours kept for one particle */

typedef enum FlMethod { FL_IPBF, FL_PBF } FlMethod;

typedef struct Fl {
    /* particles: `fluid` moving ones first, then `walls` fixed ones outside the box */
    double  *x, *v;            /* 2 each */
    uint32_t fluid, walls;
    double   spacing;          /* of the lattice that has the rest density       */
    double   radius;           /* of the kernel: 2 spacings                      */
    double   mass, rest;       /* rest: the rest density, 1                      */
    double   scale;            /* (mass / rest)^2 x sum of |grad W|^2 over that lattice: the size of the
                                * pressure term of the Hessian, used as the unit of 1 / compliance */
    double   box[2];           /* the fluid stays in [0, box]                    */

    double   h, gravity;
    uint32_t iterations;
    FlMethod method;
    /* IPBF. A compliance c here stands for the paper's alpha = c x scale x (1/240 s)^2 / mass,
     * so that c = 1 makes inertia weigh as much as pressure at a step of 1/240 s. */
    double   compliance;       /* 0: incompressible (the paper's default)        */
    double   soft;             /* the larger compliance of the damping's second solution (alpha*) */
    double   beta;             /* damping acts where the two solutions are within beta kernel radii */
    bool     exact;            /* the full second-order term instead of its diagonal approximation */
    bool     damping;
    bool     active;           /* leave constraints that the clamp set to zero out of the Hessian too */
    /* PBF */
    double   relaxation;       /* epsilon of its eq. 11, in units of `scale`     */

    /* results of the last step */
    double   error;            /* mean over the fluid of max(density / rest - 1, 0) at the end */
    double   densest;          /* largest density / rest                         */
    uint32_t overflow;         /* particles that had more than FL_NEIGHBOURS neighbours */

    /* work */
    double   *y, *guess, *next, *other, *c, *lambda;
    uint32_t *list, *count, *cell, *order;
    uint32_t  cells[2], capacity;
} Fl;

/* An empty box with walls of three layers of fixed particles round it. */
void fl_open(Fl *s, double width, double height, double spacing);
void fl_close(Fl *s);

/* Fluid on a square lattice of `step` between the corners; step = spacing is the rest density.
 * Call before the first step only after fl_open (it keeps the walls last). */
void fl_block(Fl *s, double x0, double y0, double x1, double y1, double step);

/* W, dW/dr and d2W/dr2 of the cubic spline kernel at distance r. */
void fl_kernel(const Fl *s, double r, double w[3]);

/* Neighbour lists and s->c[i] = density / rest - 1 for the positions x (all particles, 2 each). */
void fl_constraints(Fl *s, const double *x);

/* The normalised energy of eq. 7 for positions x and inertial positions y; fl_constraints(s, x) first. */
double fl_energy(const Fl *s, const double *x, const double *y, double compliance);

/* For fluid particle i: f = -dPsi/dx_i (eq. 10) and its Hessian H = {xx, xy, yy} (eq. 11, 15),
 * approximated as in section 3.5 unless `exact`. fl_constraints(s, x) first. */
void fl_local(const Fl *s, const double *x, const double *y, double compliance, bool exact, uint32_t i, double f[2], double H[3]);

/* One time step by s->method. */
void fl_step(Fl *s);
#endif /* IPBF_METHOD_H */
