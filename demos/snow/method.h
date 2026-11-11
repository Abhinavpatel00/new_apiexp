/* method.h — the snow of Stomakhin et al. 2013 on the CPU, in double.
 *
 * The same time step as shaders/snow.slang, for --check to compare with, and
 * the parts the shaders leave out: the particle volumes of the first step,
 * and the implicit velocity solve with the force derivatives of the paper's
 * technical document.
 *
 * Lengths are grid cells, time is seconds, mass is particles (all weigh the
 * same). Stress is divided by the density of fresh snow, so it is in
 * cells^2 / s^2.
 */
#ifndef SNOW_METHOD_H
#define SNOW_METHOD_H

#include <stdint.h>

#define SNOW_N              64u                       /* nodes along a side */
#define SNOW_NODES          (SNOW_N * SNOW_N * SNOW_N)
#define SNOW_PER_CELL       8.0                       /* particles in a cell of fresh snow */
#define SNOW_MASS_SCALE     16777216.0                /* the grid holds integers: see snow.slang */
#define SNOW_MOMENTUM_SCALE 16384.0
#define SNOW_IMPULSE_SCALE  262144.0

typedef struct SnowParticle {
    float pos[3], volume;   /* volume at the start / volume of a particle of fresh snow */
    float vel[3], jp;       /* jp = det F_P: below 1 packed, above 1 torn */
    float f[3][4];          /* F_E by rows; the fourth column is not used */
} SnowParticle;
_Static_assert(sizeof(SnowParticle) == 80, "SnowParticle must match snow.slang");

typedef struct SnowParams {
    double dt, gravity;
    double mu, lambda;          /* of fresh snow, / density */
    double hardening;           /* xi */
    double compress, stretch;   /* theta_c, theta_s */
    double flip;                /* alpha: 0 all PIC, 1 all FLIP */
    double friction;
    double walls;               /* nodes 0 .. walls - 1 and n - walls .. n - 1 are wall */
    double ball[4];             /* a ball that snow cannot enter: centre and radius, 0 for none */
    double ball_velocity[3];    /* it may move */
    int    sticky;              /* side walls and ceiling hold whatever touches them (the paper's sticky collision) */
} SnowParams;

/* The 4 x 4 x 4 nodes round a particle: the lowest, and the cubic B-spline and its slope along each axis. */
void snow_weights(const float pos[3], int base[3], double w[3][4], double dw[3][4]);

/* c = v diag(lam) v^T for a symmetric c (Jacobi rotations); c is destroyed. */
void snow_eigen(double c[3][3], double v[3][3], double lam[3]);
/* f = r s, r a rotation, s symmetric. */
void snow_polar(const double f[3][3], double r[3][3], double s[3][3]);

/* The energy density psi(F_E, J_P) / density, its derivative by F_E, and the change of that
 * derivative for a change df of F_E (technical document, section 2). */
double snow_energy(const SnowParams *p, const double f[3][3], double jp);
void   snow_stress(const SnowParams *p, const double f[3][3], double jp, double out[3][3]);
void   snow_stress_change(const SnowParams *p, const double f[3][3], double jp, const double df[3][3], double out[3][3]);

/* The paper's collision rule against the walls and the ball, for a node or a particle at x. */
void snow_collide(const SnowParams *p, const double x[3], double v[3]);

/* Step 2 of the paper, done once: every particle's volume from the density the grid sees at it. */
void snow_volumes(SnowParticle *particles, uint32_t count);

/* Steps 1 and 3: mass, momentum and dt x force of every node, as the integers the GPU keeps (8 a node). */
void snow_p2g(const SnowParams *p, const SnowParticle *particles, uint32_t count, int32_t *grid);
/* Steps 4 and 5: node velocities before and after forces, gravity and collisions (3 a node), and masses.
 * held (may be NULL) is 1 for a node whose velocity a collision changed. */
void snow_velocities(const SnowParams *p, const int32_t *grid, double *mass, double *before, double *after, uint8_t *held);

/* Total energy and node forces with the nodes moved by `offset` (3 a node, NULL for none):
 * equations 1 and 2 of the technical document. Either output may be NULL. */
void snow_forces(const SnowParams *p, const SnowParticle *particles, uint32_t count, const double *offset, double *force, double *energy);
/* Equations 3 and 4: the change of the node forces when the nodes move by du. */
void snow_force_change(const SnowParams *p, const SnowParticle *particles, uint32_t count, const double *du, double *df);
/* Step 6: solves (1 + beta dt^2 m^-1 d2Phi/dx2) v = v* in place by conjugate residuals, v* = `after`.
 * Held nodes keep their velocity and are left out of the solve (the paper, section 8).
 * Returns the iterations used; *residual is |r| / |b| at the end. */
uint32_t snow_implicit(const SnowParams *p, const SnowParticle *particles, uint32_t count, double beta, const double *mass,
                       const uint8_t *held, double *after, double *residual);

/* Steps 7 to 10 for one particle. */
void snow_g2p(const SnowParams *p, SnowParticle *particle, const double *before, const double *after);

#endif
