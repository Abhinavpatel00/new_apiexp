/* flight.h — the flight-dynamics transform.
 *
 * Two tables and two loops, because AGENTS.md rule 3 is about not asking "what
 * am I?" inside a loop:
 *
 *   Sim.player    one row. Its controls come from the input snapshot.
 *   Sim.traffic   N rows. Their controls come from a pursuit law.
 *
 * Nothing in either loop tests a per-row kind, and nothing downstream cares
 * which table a row came from — fs_sim_draw_records() writes both into one
 * array the renderer instances over.
 *
 * A row that hits the ground is swap-removed from traffic and counted in
 * traffic_lost, so "is this aircraft still flying" is membership in
 * [0, traffic_count) rather than a flag on the row. The player is not removed;
 * crashing sets Sim.crashed and the caller decides whether to reset.
 *
 * The model is a dispatch, not a branch. fs_sim_step() picks one of two
 * specialised integrators once per tick, so "realistic or arcade" never appears
 * inside a loop over rows.
 *
 * Layout. Aircraft is a structure and not a structure of arrays, deliberately.
 * The physics step reads every field of a row and writes every field of it,
 * which is the one access pattern SoA has nothing to offer: splitting 24 rows
 * across twenty arrays would touch twenty cache lines to do work one line
 * already holds. Where SoA does pay — the batched view*model multiply that
 * feeds the renderer — the data is packed into one contiguous float array and
 * mu_mat4_mul_batch() does four at a time. Rule 14, not rule 1, decides.
 *
 * Frames. World: metres, right-handed, +X east, +Y up, +Z south, so north is
 * -Z and a compass heading increases clockwise seen from above. Body: +X
 * forward, +Y right, +Z down, the aerospace convention, so u is the speed
 * along the nose, and the attitude quaternion rotates body into world.
 */
#ifndef FS_FLIGHT_H
#define FS_FLIGHT_H

#include "mu_math.h"
#include "mu_pcg.h"
#include "mu_shader_noise.h"

#include <stdbool.h>
#include <stdint.h>

#define FS_MAX_TRAFFIC 24u

#define FS_G       9.80665f      /* m/s^2                                    */
#define FS_RHO_0   1.225f        /* kg/m^3 at sea level, ISA                 */
#define FS_SCALE_H 8500.0f       /* m; the exponential atmosphere's scale    */
#define FS_KT_TO_MS 0.514444f
#define FS_MS_TO_KT (1.0f / FS_KT_TO_MS)
#define FS_M_TO_FT  3.2808399f
#define FS_FT_TO_M  (1.0f / FS_M_TO_FT)

/* The aerodynamic model, as data. Coefficients rather than a ladder of cases is
 * what lets the two flight models share one airframe, and what lets --check
 * move one number and assert the expected consequence. */
typedef struct AeroModel {
    float mass;              /* kg                                        */
    float wing_area;         /* m^2                                       */
    float span;              /* m                                         */
    float chord;             /* m, mean aerodynamic                       */
    /* Diagonal inertia only. The product of inertia Ixz is real on a swept
     * airframe and dropping it is an approximation, not an oversight; it costs
     * a little of the pitch-roll coupling in a hard turn and it removes a
     * denominator that goes to zero for a symmetric tensor. */
    float inertia[3];        /* kg m^2 about body X, Y, Z                 */
    float thrust_max;        /* N at full throttle, sea level             */

    float cl0, cl_alpha, cl_de, cl_q;
    float cd0, cd_k, cd_de;
    float cm0, cm_alpha, cm_de, cm_q;
    float cy_beta, cy_dr;
    float cl_beta, cl_da, cl_dr, cl_p, cl_r;
    float cn_beta, cn_da, cn_dr, cn_p, cn_r;

    float alpha_stall;       /* rad; past this the lift curve flattens    */
    float surface_rate;      /* rad/s the control surfaces slew at        */
    float deflection[3];     /* rad of elevator, aileron, rudder travel   */

    /* The arcade model's own numbers. It is a rate controller, not an
     * aerodynamic one, so it needs rates rather than coefficients. */
    float arcade_roll_rate;   /* rad/s at full aileron                    */
    float arcade_pitch_rate;  /* rad/s at full elevator                   */
    float arcade_yaw_rate;    /* rad/s at full rudder                     */
    float arcade_speed_min;   /* m/s at zero throttle                     */
    float arcade_speed_max;   /* m/s at full throttle                     */
    float arcade_speed_lag;   /* 1/s; how fast the speed chases the target */
} AeroModel;

/* The default airframe: a two-tonne low-wing single with more power than it
 * needs, so a player can loop it. Coefficients are in the range a light
 * aircraft actually has; they are not a specific type's and README.md says so. */
extern const AeroModel fs_model_default;

/* One aircraft. Every field here is read and written by the step. */
typedef struct Aircraft {
    float pos[3];        /* world position, m                                   */
    float vel[3];        /* body velocity, m/s: u forward, v right, w down       */
    float rate[3];       /* body rates, rad/s: p roll, q pitch, r yaw            */
    float att[4];        /* body -> world quaternion, mu_math order x, y, z, w   */
    float surf[3];       /* actual elevator, aileron, rudder deflection, rad     */
    float throttle;      /* 0 .. 1                                               */

    /* Commanded controls, normalised -1 .. 1 except the throttle. The input
     * layer and the pursuit law write these; the step slews `surf` toward them,
     * which is what makes the aircraft feel like it has mass on the sticks. */
    float cmd[3];
    float cmd_throttle;

    /* Derived every step. The HUD and the checks read these and nothing writes
     * them, so they are recomputed rather than carried: a stale instrument is
     * worse than a cheap one. */
    float airspeed;      /* m/s through the airmass                              */
    float groundspeed;   /* m/s over the ground                                  */
    float alpha;         /* angle of attack, rad                                 */
    float beta;          /* sideslip, rad                                        */
    float altitude;      /* m above sea level                                    */
    float vsi;           /* m/s, positive climbing                               */
    float load_factor;   /* g                                                    */
    float heading;       /* rad, 0 = north, clockwise                            */
    float pitch;         /* rad, positive nose up                                */
    float roll;          /* rad, positive right wing down                        */
    float stall_margin;  /* 1 = plenty of margin, 0 = at the stall, <0 stalled   */
    float mach;
    float rho;           /* kg/m^3 at the current altitude                       */

    float vel_world[3];  /* world velocity, m/s; the flight path marker needs it */
} Aircraft;

typedef struct Sim {
    Aircraft  player;
    Aircraft  traffic[FS_MAX_TRAFFIC];
    uint32_t  traffic_count;      /* rows in [0, traffic_count) are flying       */
    uint32_t  traffic_lost;       /* rows removed because they hit the ground    */
    AeroModel model;

    /* Traffic-only data lives beside the rows rather than inside them: the
     * player has no waypoint and would carry three dead floats forever. */
    float     waypoint[FS_MAX_TRAFFIC][3];

    bool      arcade;             /* chosen once per frame, outside every loop   */
    bool      crashed;            /* the player hit the ground                   */
    float     crash_speed;        /* m/s at impact, for the readout              */

    float     wind_base[3];       /* the airmass's own motion, world, m/s        */
    float     wind[3];            /* base plus this tick's gust, world, m/s      */
    float     turbulence;         /* 0 = still, 1 = rough                        */
    float     time;               /* s since the sim started                     */

    /* Fixed-step accumulator. A flight model integrated at the frame rate
     * changes its behaviour with the frame rate, which is the one thing a
     * simulator must not do; the render frame is decoupled from the tick. */
    float     accumulator;
    float     tick;               /* s per physics tick                          */
    uint32_t  ticks_this_frame;
} Sim;

/* The terrain height under each row, filled by the world before the step so
 * flight.c never has to know what the ground is made of. One float per row,
 * which is the shape the ground-contact test actually wants. */
typedef struct GroundHeights {
    float player;
    float traffic[FS_MAX_TRAFFIC];
} GroundHeights;

void fs_sim_reset(Sim *s, const AeroModel *model, uint32_t traffic_count, uint64_t seed);

/* One render frame's worth of physics: as many fixed ticks as the elapsed time
 * calls for, capped so a stall in the frame rate cannot turn into a spiral of
 * them. `ground` is consumed, not retained. */
void fs_sim_step(Sim *s, float dt, const GroundHeights *ground);

/* The two integrators, exposed so --check can drive one tick of each directly. */
void fs_step_sixdof(Sim *s, Aircraft *a, float dt, float ground);
void fs_step_arcade(Sim *s, Aircraft *a, float dt, float ground);

/* One pursuit-law tick over the traffic table: each row steers for its
 * waypoint, holds its altitude and keeps out of the player's way. */
void fs_traffic_control(Sim *s, float dt);

/* The instruments' inputs, recomputed from the attitude and the velocity. Both
 * integrators call it, because an instrument computed twice in two places is an
 * instrument that eventually disagrees with the physics beside it. `load_factor`
 * is passed in: the 6-DOF model knows it aerodynamically and the arcade model
 * only geometrically, and neither should have to reimplement the other. */
void fs_aircraft_derive(Aircraft *a, const float *restrict body_to_world, float load_factor);

/* Pack every row, player first, into the 80-byte records the aircraft shader
 * instances over: a model matrix in mu_math's column-major order and a colour
 * whose alpha is emission. Returns the record count.
 *
 * The view-projection is deliberately not folded in here. The vertex shader
 * needs the model matrix on its own anyway, because world position and world
 * normal both come from it, so precomputing model*view_proj would leave three
 * mat4-by-vec4 multiplies per vertex where there are already three, and would
 * widen the record every instance has to load. view_proj travels as a push
 * constant instead, which costs nothing per instance. */
uint32_t fs_sim_draw_records(const Sim *s, float *records, uint32_t stride,
                             const float *player_colour, const float *traffic_colour);

#endif /* FS_FLIGHT_H */
