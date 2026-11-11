/* method.h — a soft body that moves itself by muscles (Tan, Turk, Liu 2012). See method.c. */
#ifndef SOFTLOCO_METHOD_H
#define SOFTLOCO_METHOD_H
#include <stdbool.h>
#include <stdint.h>

#define SL_CONTACTS 24   /* nodes on the ground that a step looks at (the lowest, if there are more) */
#define SL_FREE     0    /* what a contact does in a step: it lets go, */
#define SL_STATIC   1    /* stays where it is, */
#define SL_SLIDE    2    /* or slides: SL_SLIDE + e, e = 0 .. 7, friction on edge e of the eight-sided cone */

typedef struct Sl {
    /* the body: tetrahedra, node positions and velocities */
    int     nodes, tets, *tet;
    double *rest, *p, *v, *mass;
    double *push;                        /* a force on every node (3 a node) that the caller sets and clears */
    double  young, poisson, damping;     /* damping: C = damping * K (the paper's 0.2) */
    double  density, gravity, dt;
    double  friction[8];                 /* edge e of the cone: friction along the direction (e + 0.5) * 45 degrees from x towards z is at most friction[e] * cos(22.5 degrees) * normal */
    /* muscles: each a segment between two nodes that wants to be `wanted` long */
    int     muscles, *end, *group;
    double *length0, *length, *wanted, *least;   /* least: how short it may want to be (0.5 * length0) */
    double  stiffness, reach;            /* stress = stiffness * (wanted - length) / rest length; reach: width of the Gaussian weights */
    /* goals for the coming step: row . (velocity after the step) = target, weighted */
    int     goals, goal_room;
    double *goal_row, *goal_target, *goal_weight;
    double  relax;                       /* added to the goals: relax * sum of ((wanted - rest) / rest)^2 */
    /* how the contacts' behaviour is chosen */
    int     search;                      /* 0 one QP: what touches the ground stays where it is; 1 the paper's search; 2 every combination */
    int     visits;                      /* most QPs the search solves in a step */
    bool    warm;                        /* start the search from the last step's choice */
    /* what the last step did */
    int     contacts, contact_node[SL_CONTACTS], contact_plan[SL_CONTACTS], contact_did[SL_CONTACTS];   /* plan: the cases chosen; did: the cases of the step taken */
    double  impulse[3 * SL_CONTACTS];    /* normal, x, z of each contact */
    int     solved, sweeps;              /* programmes solved; active-set steps in all of them */
    bool    planned;                     /* false: no QP was feasible, the muscles kept their lengths */
    double  objective, first_objective;  /* of the plan, and of the first QP (1e300: that one was infeasible) */
    /* the method's own */
    double *tet_inverse, *tet_volume, *tet_k, *weight, *direction;
    double *k, *factor, *a, *x, *v0;
    struct SlPlan *plan;
    int     last_contacts, last_node[SL_CONTACTS], last_plan[SL_CONTACTS];
} Sl;

/* Takes copies. A muscle is end[2 j], end[2 j + 1] and its group (weights are normalised within a group). */
Sl  *sl_open(int nodes, const double *rest, int tets, const uint32_t *tet, int muscles, const int *end, const int *group, double reach);
void sl_close(Sl *s);

/* Centre of mass into c; returns the mass. */
double sl_centre(const Sl *s, double c[3]);

/* Goals (section 5.2); each is forgotten after the next sl_step.
 * sl_goal_point:    the point sum of share[i] * p[i] is at `target` after the step (eq. 17), a weight for each coordinate
 * sl_goal_momentum: the linear momentum changes at `rate` (eq. 12)
 * sl_goal_spin:     the angular momentum about the centre of mass changes at `rate` (eq. 14)
 * sl_goal_base:     the area of the triangles (3 nodes each) seen from above changes at `rate` (eq. 15, 16) */
void sl_goal_point(Sl *s, const double weight[3], const double *share, const double target[3]);
void sl_goal_momentum(Sl *s, double weight, const double rate[3]);
void sl_goal_spin(Sl *s, double weight, const double rate[3]);
void sl_goal_base(Sl *s, double weight, int triangles, const int *triangle, double rate);

/* One time step: chooses the muscles' wanted lengths for the goals (if there are any), then
 * moves the body. advance false: only chooses, so that searches can be compared on one state. */
void sl_step(Sl *s, bool advance);

#endif /* SOFTLOCO_METHOD_H */
