/* method.c — Yuksel, House, Keyser: "Wave Particles" (SIGGRAPH 2007), section 3 and the
 * height field part of section 5.
 *
 * The water surface is a sum of small bumps (eq. 3), each riding on a particle that moves in
 * a straight line at the wave speed (eq. 4). Bumps side by side make a wavefront. A front
 * that widens would open gaps, so each particle knows what angle of the front it stands
 * for, and when its neighbours are more than half a radius away it becomes three (3.2).
 *
 * Nothing about a particle changes between its birth and its subdivision, so the time of
 * that is known at birth and kept in a heap ("a time table of events", section 5). A time
 * step looks only at the particles whose time has come.
 *
 * Not here: objects in the water (section 4), and curved walls.
 */
#include "method.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define WP_PI 3.14159265358979323846

static void schedule(Wp *s, uint32_t index) {
    const WpParticle *p = &s->particle[index];
    /* Neighbours are spread x distance apart; they are too far when that passes radius / 2.
     * A particle of a straight front never subdivides. Its event is the time at which damping
     * has taken it below the threshold (never, without damping): not in the paper, but a
     * straight front in a pool would otherwise stay for ever. The time uses the damping of the
     * moment the particle is emitted. */
    const WpEvent e = {p->born + (p->spread > 0.0 ? s->radius / (2.0 * p->spread * s->speed) : log(fabs(p->amplitude) / s->threshold) / s->damping), index};
    uint32_t      at = s->events++;
    while (at > 0u && s->event[(at - 1u) / 2u].time > e.time) { s->event[at] = s->event[(at - 1u) / 2u]; at = (at - 1u) / 2u; }
    s->event[at] = e;
}

/* A free slot; the arrays grow so that every slot can also hold an event and a spare. */
static uint32_t slot(Wp *s) {
    if (s->spares) return s->spare[--s->spares];
    if (s->slots == s->capacity) {
        s->capacity = s->capacity ? 2u * s->capacity : 1024u;
        s->particle = (WpParticle *)realloc(s->particle, s->capacity * sizeof(WpParticle));
        s->event    = (WpEvent *)realloc(s->event, s->capacity * sizeof(WpEvent));
        s->spare    = (uint32_t *)realloc(s->spare, s->capacity * sizeof(uint32_t));
    }
    return s->slots++;
}

void wp_emit(Wp *s, double x, double y, double angle, double amplitude, double spread) {
    const uint32_t index = slot(s);
    s->particle[index] = (WpParticle){{x, y}, angle, s->time, amplitude, spread, true};
    s->alive++;
    schedule(s, index);
}

void wp_advance(Wp *s, double dt) {
    s->time += dt;
    while (s->events && s->event[0].time <= s->time) {
        const WpEvent    due = s->event[0], last = s->event[--s->events];
        const WpParticle parent = s->particle[due.particle];
        uint32_t         at = 0u, child;
        int              k;
        /* take the soonest event out of the heap */
        for (;;) {
            child = 2u * at + 1u;
            if (child >= s->events) break;
            if (child + 1u < s->events && s->event[child + 1u].time < s->event[child].time) child++;
            if (s->event[child].time >= last.time) break;
            s->event[at] = s->event[child];
            at = child;
        }
        s->event[at] = last;
        s->visited++;
        /* "if the amplitude is below a user defined threshold, kill the wave particle instead of subdividing" */
        if (parent.spread == 0.0 || fabs(parent.amplitude) * exp(-s->damping * (due.time - parent.born)) < s->threshold) {
            s->particle[due.particle].alive = false;
            s->spare[s->spares++] = due.particle;
            s->alive--;
            s->removed++;
            continue;
        }
        /* three in its place, each a third of the amplitude and of the angle; the radius stays */
        for (k = -1; k <= 1; ++k) {
            const uint32_t index = k ? slot(s) : due.particle;
            s->particle[index] = (WpParticle){{parent.origin[0], parent.origin[1]}, parent.angle + k * parent.spread / 3.0, parent.born,
                                              parent.amplitude / 3.0, parent.spread / 3.0, true};
            schedule(s, index);
        }
        s->alive += 2u;
    }
}

void wp_place(const Wp *s, const WpParticle *p, double at[2], double direction[2]) {
    const double far = s->speed * (s->time - p->born);
    int          k;
    direction[0] = cos(p->angle); direction[1] = sin(p->angle);
    for (k = 0; k < 2; ++k) {
        at[k] = p->origin[k] + far * direction[k];
        /* A straight wall sends a particle back as a mirror would: fold the plane at every wall. */
        if (s->pool > 0.0) {
            at[k] = fmod(at[k], 2.0 * s->pool);
            if (at[k] < 0.0) at[k] += 2.0 * s->pool;
            if (at[k] > s->pool) { at[k] = 2.0 * s->pool - at[k]; direction[k] = -direction[k]; }
        }
    }
}

void wp_field(const Wp *s, uint32_t n, double cell, double *height, double *shift) {
    const double r = s->radius;
    uint32_t     index, image;
    memset(height, 0, (size_t)n * n * sizeof(double));
    if (shift) memset(shift, 0, (size_t)n * n * 2u * sizeof(double));
    for (index = 0u; index < s->slots; ++index) {
        const WpParticle *p = &s->particle[index];
        const double      a = p->amplitude * exp(-s->damping * (s->time - p->born));
        double            at[2], dir[2];
        if (!p->alive) continue;
        wp_place(s, p, at, dir);
        /* Within a radius of a wall part of the bump is outside the pool. Its mirror image puts
         * that part back inside: the reflected wave, which is on its way before the particle
         * itself turns round. Image 1 mirrors in x, 2 in y, 3 in both. */
        for (image = 0u; image < 4u; ++image) {
            double c[2] = {at[0], at[1]}, d[2] = {dir[0], dir[1]};
            int    i, j, k, lo[2], hi[2];
            bool   needed = true;
            for (k = 0; k < 2; ++k) {
                if (!(image >> k & 1u)) continue;
                needed = needed && s->pool > 0.0 && (c[k] < r || c[k] > s->pool - r);
                c[k]   = c[k] < r ? -c[k] : 2.0 * s->pool - c[k];
                d[k]   = -d[k];
            }
            if (!needed) continue;
            for (k = 0; k < 2; ++k) {
                lo[k] = (int)floor((c[k] - r) / cell - 0.5); hi[k] = (int)ceil((c[k] + r) / cell - 0.5);
                if (lo[k] < 0) lo[k] = 0;
                if (hi[k] > (int)n - 1) hi[k] = (int)n - 1;
            }
            for (j = lo[1]; j <= hi[1]; ++j)
                for (i = lo[0]; i <= hi[0]; ++i) {
                    const double x = (i + 0.5) * cell - c[0], y = (j + 0.5) * cell - c[1], rho = sqrt(x * x + y * y);
                    double       bump;
                    if (rho >= r) continue;
                    bump = 0.5 * a * (cos(WP_PI * rho / r) + 1.0);                 /* eq. 7 */
                    height[(size_t)j * n + (size_t)i] += bump;
                    if (shift) {
                        const double along = -sin(WP_PI * (d[0] * x + d[1] * y) / r) * bump;   /* eq. 10, 11 */
                        shift[2u * ((size_t)j * n + (size_t)i)]      += along * d[0];
                        shift[2u * ((size_t)j * n + (size_t)i) + 1u] += along * d[1];
                    }
                }
        }
    }
}

void wp_field_filtered(const Wp *s, uint32_t n, double cell, bool separable, double *height) {
    const double r = s->radius;
    const int    reach = (int)ceil(r / cell), size = (int)n;
    double      *points = (double *)calloc((size_t)n * n, sizeof(double)), *across = (double *)calloc((size_t)n * n, sizeof(double));
    uint32_t     index;
    int          i, j, di, dj, pass;
    /* "drawing wave particles as point primitives on the height field texture" */
    for (index = 0u; index < s->slots; ++index) {
        const WpParticle *p = &s->particle[index];
        double            at[2], dir[2];
        if (!p->alive) continue;
        wp_place(s, p, at, dir);
        i = (int)floor(at[0] / cell); j = (int)floor(at[1] / cell);
        if (i >= 0 && i < size && j >= 0 && j < size) points[(size_t)j * n + (size_t)i] += p->amplitude * exp(-s->damping * (s->time - p->born));
    }
    memset(height, 0, (size_t)n * n * sizeof(double));
    /* A sample outside the image is its mirror image inside when there are walls, nothing otherwise. */
    for (pass = 0; pass < 2; ++pass)
        for (j = 0; j < size; ++j)
            for (i = 0; i < size; ++i) {
                const double *from = pass ? across : points;
                double       *to = pass || !separable ? height : across, sum = 0.0;
                if (pass && !separable) continue;
                for (dj = pass || !separable ? -reach : 0; dj <= (pass || !separable ? reach : 0); ++dj)
                    for (di = pass ? 0 : -reach; di <= (pass ? 0 : reach); ++di) {
                        const double rho = sqrt((double)(di * di + dj * dj)) * cell;
                        int          x = i + di, y = j + dj;
                        if (rho >= r) continue;
                        if (s->pool > 0.0) { x = x < 0 ? -1 - x : x >= size ? 2 * size - 1 - x : x; y = y < 0 ? -1 - y : y >= size ? 2 * size - 1 - y : y; }
                        if (x < 0 || x >= size || y < 0 || y >= size) continue;
                        sum += from[(size_t)y * n + (size_t)x] * 0.5 * (cos(WP_PI * rho / r) + 1.0);   /* eq. 7; in one dimension eq. 5 */
                    }
                to[(size_t)j * n + (size_t)i] = sum;
            }
    free(points); free(across);
}
