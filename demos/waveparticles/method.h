/* method.h — wave particles (Yuksel, House, Keyser 2007) on the CPU. See method.c. */
#ifndef WAVEPARTICLES_METHOD_H
#define WAVEPARTICLES_METHOD_H
#include <stdbool.h>
#include <stdint.h>

typedef struct WpParticle {
    double origin[2];   /* where its wavefront began, in a plane without walls   */
    double angle;       /* the direction it travels from there, radians          */
    double born;        /* when the wavefront began                              */
    double amplitude;   /* a_i, before damping                                   */
    double spread;      /* dispersion angle: the share of the front it stands for; 0 = a straight front */
    bool   alive;
} WpParticle;

typedef struct WpEvent {
    double   time;
    uint32_t particle;
} WpEvent;

typedef struct Wp {
    double speed, radius;   /* of every wave and every particle                              */
    double damping;         /* amplitudes fall as exp(-damping x age); 0 = none              */
    double threshold;       /* a particle due to subdivide with less amplitude is removed    */
    double pool;            /* walls at 0 and `pool` in x and y; 0 = open water              */
    double time;
    /* counted by wp_advance */
    uint64_t visited, removed;
    uint32_t alive;
    /* owned by wp_emit / wp_advance */
    WpParticle *particle;
    WpEvent    *event;      /* a heap: the soonest subdivision first */
    uint32_t   *spare;      /* slots of removed particles            */
    uint32_t    slots, events, spares, capacity;
} Wp;

/* A particle at (x, y) now. `spread` 2 pi is a ripple from a point (section 4.1). */
void wp_emit(Wp *s, double x, double y, double angle, double amplitude, double spread);

/* Move time on by dt and do the subdivisions that fall in it (section 3.2). Particles are
 * not visited otherwise: where one is follows from its origin and age. */
void wp_advance(Wp *s, double dt);

/* Where a particle is now and which way it travels, walls taken into account (section 3.3). */
void wp_place(const Wp *s, const WpParticle *p, double at[2], double direction[2]);

/* The height field on n x n samples, sample (i, j) at ((i + 0.5) cell, (j + 0.5) cell): the
 * sum of eq. 7 over the particles. If `shift` is not NULL it receives 2 a sample: the
 * horizontal deviation of eq. 9 to 11. */
void wp_field(const Wp *s, uint32_t n, double cell, double *height, double *shift);

/* The same height the way section 5 makes it on a GPU: each particle a point in the image,
 * then a filter with eq. 7 as the kernel, or (`separable`) a horizontal and a vertical pass
 * with eq. 5. With walls the image must cover the pool exactly. */
void wp_field_filtered(const Wp *s, uint32_t n, double cell, bool separable, double *height);
#endif /* WAVEPARTICLES_METHOD_H */
