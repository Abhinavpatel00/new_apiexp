/* method.h — weighted sample elimination (Yuksel 2015) in the plane. See method.c. */
#ifndef SAMPLEELIM_METHOD_H
#define SAMPLEELIM_METHOD_H
#include <stdbool.h>
#include <stdint.h>

typedef struct Se {
    double size[2];              /* the domain is [0, size[0]) x [0, size[1])            */
    double alpha, beta, gamma;   /* eq. 1 and eq. 5: 8, 0.65, 1.5                         */
    bool   tiled;                /* distances wrap round the domain                       */
} Se;

/* A number in [0, 1); `state` must not be 0. */
double se_random(uint64_t *state);

/* r_max of eq. 2 for n samples. */
double se_rmax(const Se *s, uint32_t n);

/* Eliminates all but `want` of the samples p[2 index[0 .. have)] (figure 2 of the paper).
 * Afterwards index[0 .. want) are the samples kept, in the order they had, and
 * index[want .. have) the eliminated ones, the last one eliminated first.
 * intensity: 0 for none, or one value in [0, 1] for each entry of p (indexed like p): then
 * d^ij = d_ij (3 - 2 I_i) and r_max is three times eq. 2, as for the paper's figure 12.
 * weight: 0, or receives the final weight of every sample kept (indexed like index). */
void se_eliminate(const Se *s, const double *p, uint32_t *index, uint32_t have, uint32_t want, const double *intensity, double *weight);

/* The order of section 5.1: index[0 .. want) as above and then again for want / 2, want / 4 ...
 * so that every prefix of index[0 .. want) is a sample set of its own. */
void se_order(const Se *s, const double *p, uint32_t *index, uint32_t have, uint32_t want);

/* Half the smallest distance between two of the n samples; *mean: the mean over the samples of
 * half the distance to the nearest other one. */
double se_radius(const Se *s, const double *p, const uint32_t *index, uint32_t n, double *mean);

/* Dart throwing: random points, each kept if no kept one is within 2 radius. Stops at `want`
 * points or after `tries` darts; returns how many points it wrote to p. */
uint32_t se_darts(const Se *s, double radius, uint32_t want, uint64_t tries, uint64_t *state, double *p);

/* |sum_j exp(-2 pi i k . x_j)|^2 / n for all whole k with |k_x|, |k_y| <= reach, for a domain of
 * 1 x 1; power has (2 reach + 1)^2 entries, k_x fastest. */
void se_spectrum(const double *p, const uint32_t *index, uint32_t n, int reach, double *power);
#endif /* SAMPLEELIM_METHOD_H */
