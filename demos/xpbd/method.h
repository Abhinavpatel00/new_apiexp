/* method.h — XPBD (Macklin, Mueller, Chentanez 2016) on the CPU. See method.c. */
#ifndef XPBD_METHOD_H
#define XPBD_METHOD_H
#include <stdbool.h>
#include <stdint.h>

enum { XPBD_DISTANCE = 0, XPBD_VOLUME = 1 };

typedef struct XpbdConstraint {
    uint32_t kind, node[4];
    double   rest;         /* length, or volume                                             */
    double   compliance;   /* alpha = 1 / stiffness: m / N for a length, m^6 / J for a volume */
    double   lambda;       /* total multiplier of this time step; lambda / dt^2 is the force */
} XpbdConstraint;

typedef struct Xpbd {
    uint32_t        nodes, constraints;
    double         *x, *v, *before;   /* 3 a node; `before` is x at the start of the step */
    double         *w;                /* 1 / mass; 0 holds a node where it is             */
    XpbdConstraint *c;
    double          dt, gravity;
    double          damping;          /* beta of the paper's section 5: N s / m for a length */
    uint32_t        iterations;
    bool            pbd;              /* the 2007 method instead, with `stiffness` in 0..1 */
    double          stiffness;
    double          floor;            /* nothing goes below this height                    */
    double          ball[4];          /* centre and radius of a ball nothing enters        */
    double          friction;         /* share of the sideways motion lost at a contact    */
} Xpbd;

/* C and its gradient with respect to each node of the constraint. */
double xpbd_value(const Xpbd *s, const XpbdConstraint *c, double grad[4][3]);

/* One time step: Algorithm 1 of the paper. */
void xpbd_step(Xpbd *s);

/* The same time step by implicit Euler on the energy sum C^2 / (2 alpha), minimised by Newton's
 * method to rounding. Distance constraints only; for the comparison of the paper's section 6. */
void xpbd_reference_step(Xpbd *s);
#endif /* XPBD_METHOD_H */
