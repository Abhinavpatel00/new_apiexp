/* method.h — Augmented Vertex Block Descent for rigid boxes in the plane. See method.c. */
#ifndef AVBD_METHOD_H
#define AVBD_METHOD_H
#include <stdbool.h>
#include <stdint.h>

enum { AVBD_SPRING = 0, AVBD_JOINT = 1, AVBD_CONTACT = 2 };
enum { AVBD_ROWS = 4 };
#define AVBD_PENALTY_MIN 1.0      /* k_start of the paper                         */
#define AVBD_PENALTY_MAX 1e9
#define AVBD_MARGIN 0.0005        /* contacts aim at this much overlap, not at 0  */
#define AVBD_STICK 0.01           /* a contact that slid less than this is stuck  */

typedef struct AvbdBody {
    double size[2], mass, moment, friction, radius;   /* mass 0: the body does not move */
    double x[3], v[3];                                /* x, y, angle                    */
    double v_before[3], inertial[3], initial[3];      /* owned by avbd_step             */
} AvbdBody;

typedef struct AvbdContact {
    double   ra[2], rb[2], normal[2];   /* the two contact points in each body's frame; normal from b to a */
    double   jan[3], jbn[3], jat[3], jbt[3], c0[2];
    uint32_t feature;                   /* which corner against which side: the same contact next step */
    bool     stick;
} AvbdContact;

typedef struct AvbdForce {
    uint32_t    kind;
    int         a, b;                     /* bodies; a = -1 pins a joint to the point `ra` of the world */
    int         rows;                     /* spring 1; joint 3: x, y, angle; contact 2 a point: normal, tangent */
    double      ra[2], rb[2];             /* where it holds each body, in the body's frame */
    double      rest, arm, c0[3];         /* spring: rest length. joint: rest angle, and angle rows are scaled by `arm` */
    double      stiffness[AVBD_ROWS];     /* INFINITY: a hard constraint                    */
    double      fmin[AVBD_ROWS], fmax[AVBD_ROWS];
    double      penalty[AVBD_ROWS], lambda[AVBD_ROWS], c[AVBD_ROWS];
    AvbdContact contact[2];
    int         contacts;
} AvbdForce;

typedef struct Avbd {
    AvbdBody  *body;
    AvbdForce *force;
    uint32_t   bodies, forces, capacity;
    double     dt, gravity;
    uint32_t   iterations;
    double     alpha;            /* share of the error a hard constraint starts the step with that is left alone (eq. 18) */
    double     beta;             /* how fast the stiffness grows with the error (eq. 12, 16)   */
    double     gamma;            /* how much stiffness and force are kept from the last step (eq. 19) */
    bool       post_stabilize;   /* the reference demo's way: alpha = 1, then one more iteration with alpha = 0 after the velocities are taken */
    bool       vbd;              /* plain vertex block descent: no multipliers, the real stiffness from the first iteration */
    double     vbd_hard;         /* the stiffness plain VBD gives a hard constraint            */
} Avbd;

/* The reference demo's values: dt 1/60, gravity 10, 10 iterations, alpha 0.99, beta 1e5, gamma 0.99.
 * (The paper gives alpha 0.95, beta 10, gamma 0.99.) */
void avbd_defaults(Avbd *s);

/* Add a box, a spring, a joint. `stiffness` of a joint is for x, y and angle; 0 leaves that row free. */
int  avbd_box(Avbd *s, double width, double height, double density, double friction, double x, double y, double angle);
void avbd_spring(Avbd *s, int a, int b, const double ra[2], const double rb[2], double stiffness, double rest);
void avbd_joint(Avbd *s, int a, int b, const double ra[2], const double rb[2], const double stiffness[3]);

/* Contact points of two boxes: 0, 1 or 2. */
int avbd_collide(const AvbdBody *a, const AvbdBody *b, AvbdContact out[2]);

/* C of every row of a force at the bodies' positions, and for one of its bodies the rows'
 * gradients J and the second derivatives H (3 x 3 a row). */
void avbd_constraint(const Avbd *s, AvbdForce *f, double alpha);
void avbd_derivatives(const Avbd *s, const AvbdForce *f, int body, double j[AVBD_ROWS][3], double h[AVBD_ROWS][3][3]);

/* One time step: Algorithm 1 of the paper. */
void avbd_step(Avbd *s);
#endif /* AVBD_METHOD_H */
