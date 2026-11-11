/* method.h — particle merging-and-splitting (Truong et al. 2021) for discs in the plane. See method.c. */
#ifndef MERGESPLIT_METHOD_H
#define MERGESPLIT_METHOD_H
#include <stdbool.h>
#include <stdint.h>

typedef struct MsParticle {
    double x[2], v[2], mass, radius;
    bool   pinned;   /* does not move and collides with nothing */
} MsParticle;

typedef struct MsSpring {
    uint32_t a, b;
    double   rest, stiffness;
} MsSpring;

typedef enum MsMethod {
    MS_MERGE,     /* the paper */
    MS_PENALTY,   /* a spring pushes intersecting particles apart */
    MS_IMPULSE    /* each approaching pair gets an instant collision, one after another */
} MsMethod;

typedef struct Ms {
    MsParticle *particle;
    MsSpring   *spring;
    uint32_t    particles, springs;
    double      dt, gravity;
    double      box[2];          /* walls at 0 and box[k] */
    double      alpha;           /* share of the bond's energy given back at the split (eq. 9) */
    double      spring_damping;  /* N s / m along each spring */
    double      penalty;         /* N / m, for MS_PENALTY */
    uint32_t    limit;           /* most particles in one meta-particle (section 5.2) */
    MsMethod    method;
    /* counted by ms_step, added up until the caller clears them */
    uint64_t    merges, second_stages, without_root;
    uint32_t    largest;         /* most particles seen in one meta-particle */
} Ms;

/* Eq. 3 and 4: the velocity of the meta-particle; returns the kinetic energy the merge takes. */
double ms_merge(double m1, const double v1[2], double m2, const double v2[2], double v12[2]);

/* Eq. 8 to 16: split a meta-particle that now moves at `v12` into particles 1 and 2. `v1` is
 * what particle 1 had before the merge, `n` the unit vector from 1 to 2 then, `energy` what
 * the bond gives back (alpha x eq. 4). Returns false when eq. 13 had no real root and the
 * orthogonal term of eq. 16 was needed. */
bool ms_split(double m1, const double v1[2], double m2, const double n[2], double energy, const double v12[2], double out1[2], double out2[2]);

uint32_t ms_add(Ms *s, double x, double y, double mass, double radius);
void     ms_link(Ms *s, uint32_t a, uint32_t b, double stiffness);   /* rest length: as they are now */

/* Kinetic + height + spring energy. */
double ms_energy(const Ms *s);

/* One time step of symplectic Euler with the collisions handled by s->method. */
void ms_step(Ms *s);
#endif /* MERGESPLIT_METHOD_H */
