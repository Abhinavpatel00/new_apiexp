/* flight.c — the two integrators and the traffic law.
 *
 * One physics tick, written out, with the frame conventions from flight.h:
 *
 *   surfaces    slew toward the command, which is what gives the stick weight
 *   air data    rho from the exponential atmosphere, alpha and beta from the
 *               velocity relative to the wind, both in body axes
 *   forces      thrust along the nose, drag against it, lift along body -Z,
 *               side force along body +Y, gravity rotated into the body
 *   moments     the coefficient polynomials, then Euler's equations on a
 *               diagonal inertia tensor
 *   integrate   rates into the attitude quaternion, body velocity with the
 *               transport term, then position from the world velocity
 *
 * The order matters. Velocity is updated before position, which makes the step
 * semi-implicit; explicit Euler on an aircraft is a divergent phugoid at any
 * timestep a game can afford.
 */
#include "flight.h"

#include "mu_easing.h"
#include "mu_math.h"

#include <math.h>
#include <string.h>

const AeroModel fs_model_default = {
    .mass = 1450.0f, .wing_area = 20.0f, .span = 12.5f, .chord = 1.75f,
    .inertia = {2100.0f, 3400.0f, 4900.0f}, .thrust_max = 21000.0f,

    /* Longitudinal. cl_alpha is per radian and cm_alpha is the static margin:
     * negative, so a nose-up perturbation pitches the nose back down. */
    .cl0 = 0.24f, .cl_alpha = 5.4f, .cl_de = 0.42f, .cl_q = 4.2f,
    .cd0 = 0.028f, .cd_k = 0.043f, .cd_de = 0.012f,
    .cm0 = 0.03f, .cm_alpha = -1.15f, .cm_de = -0.62f, .cm_q = -6.4f,

    /* Lateral. cl_beta negative is dihedral; cn_beta positive is the
     * weathercock, and the two together are why a slipped turn rolls. */
    .cy_beta = -0.62f, .cy_dr = 0.18f,
    .cl_beta = -0.075f, .cl_da = 0.20f, .cl_dr = 0.045f, .cl_p = -0.42f, .cl_r = 0.16f,
    .cn_beta = 0.085f, .cn_da = 0.028f, .cn_dr = -0.16f, .cn_p = -0.035f, .cn_r = -0.28f,

    .alpha_stall = 0.27f,          /* about 15.5 degrees                        */
    .surface_rate = 1.9f,          /* rad/s: a heavy feel, not an instant one    */
    .deflection = {0.42f, 0.38f, 0.35f},

    .arcade_roll_rate = 2.1f, .arcade_pitch_rate = 1.15f, .arcade_yaw_rate = 0.55f,
    .arcade_speed_min = 62.0f, .arcade_speed_max = 165.0f, .arcade_speed_lag = 0.55f,
};

/* Body axes are the rotating frame the velocity is stored in, so a world vector
 * has to be turned into the body before it can be added to one. The columns of
 * body_to_world are the body axes in world coordinates, so the transpose is a
 * dot product per row. */
static void world_to_body(float *restrict out, const float *restrict R, const float *restrict v)
{
    out[0] = R[0] * v[0] + R[1] * v[1] + R[2] * v[2];
    out[1] = R[4] * v[0] + R[5] * v[1] + R[6] * v[2];
    out[2] = R[8] * v[0] + R[9] * v[1] + R[10] * v[2];
}

/* A signed smoothstep: mu_ease_smoothstep clamps its input to [0, 1], which is
 * right for a fade and wrong for a control surface that has to deflect both
 * ways. The sign is restored outside the curve so the response is odd, and an
 * odd response is what makes centring the stick mean anything. */
static float signed_smoothstep(float e)
{
    const float s = e < 0.0f ? -1.0f : 1.0f;
    return s * mu_ease_smoothstep(fabsf(e));
}

/* The lift curve past the stall. Below alpha_stall it is the linear one; above
 * it the wing is a flat plate, whose normal force is 2 sin^2(alpha) and whose
 * lift is that times cos(alpha). The blend is what produces the buffet and the
 * nose drop, instead of a lift value that simply stops increasing. */
static float lift_coefficient(const AeroModel *m, float alpha)
{
    const float linear = m->cl0 + m->cl_alpha * alpha;
    const float over   = alpha - m->alpha_stall;
    if (over <= 0.0f) return linear;

    const float t        = mu_clamp(over / 0.30f, 0.0f, 1.0f);
    const float s        = sinf(alpha);
    const float plate    = 2.0f * s * s * cosf(alpha);
    const float at_stall = m->cl0 + m->cl_alpha * m->alpha_stall;
    return mu_lerp(at_stall, plate, t);
}

void fs_aircraft_derive(Aircraft *a, const float *restrict body_to_world, float load_factor)
{
    const float fx = body_to_world[0];      /* the nose, in world coordinates    */
    const float fy = body_to_world[1];
    const float fz = body_to_world[2];

    a->altitude    = a->pos[1];
    a->vsi         = a->vel_world[1];
    /* Heading from north, clockwise seen from above. North is -Z and east is
     * +X in this world, so the two arguments are the east component and the
     * north one, in that order. */
    a->heading     = atan2f(fx, -fz);
    a->pitch       = asinf(mu_clamp(fy, -1.0f, 1.0f));
    /* Roll from the right wing's height, normalised by the horizontal part of
     * the span so a steep climb does not read as a bank. */
    a->roll        = atan2f(-body_to_world[5],
                            sqrtf(body_to_world[4] * body_to_world[4] +
                                  body_to_world[6] * body_to_world[6]));
    a->groundspeed = sqrtf(a->vel_world[0] * a->vel_world[0] + a->vel_world[2] * a->vel_world[2]);
    a->mach        = a->airspeed / (340.3f - 0.0042f * mu_clamp(a->altitude, 0.0f, 20000.0f));
    a->load_factor = load_factor;
}

void fs_step_sixdof(Sim *s, Aircraft *a, float dt, float ground)
{
    const AeroModel *m = &s->model;
    float            R[16];

    /* ---- surfaces --------------------------------------------------------- */
    for (int k = 0; k < 3; ++k) {
        const float target = mu_clamp(a->cmd[k], -1.0f, 1.0f) * m->deflection[k];
        const float step   = m->surface_rate * dt;
        a->surf[k] += mu_clamp(target - a->surf[k], -step, step);
    }
    a->throttle += mu_clamp(a->cmd_throttle - a->throttle, -0.6f * dt, 0.6f * dt);
    a->throttle  = mu_clamp(a->throttle, 0.0f, 1.0f);

    /* ---- air data --------------------------------------------------------- */
    mu_mat4_from_quat(R, a->att);
    const float alt = a->pos[1];
    const float rho = FS_RHO_0 * expf(-mu_clamp(alt, 0.0f, 40000.0f) / FS_SCALE_H);
    a->rho = rho;

    float wind_body[3];
    world_to_body(wind_body, R, s->wind);
    const float vrel[3] = {a->vel[0] - wind_body[0], a->vel[1] - wind_body[1],
                           a->vel[2] - wind_body[2]};
    const float V = mu_vec3_length(vrel);
    a->airspeed   = V;

    /* Guard the divide. At zero airspeed the angles are undefined, and the
     * honest answer is the one a stalled aircraft gives, not a NaN that would
     * propagate into the attitude quaternion and never leave it. */
    const float Vsafe = V > 0.5f ? V : 0.5f;
    const float alpha = atan2f(vrel[2], vrel[0]);
    const float beta  = asinf(mu_clamp(vrel[1] / Vsafe, -1.0f, 1.0f));
    a->alpha        = alpha;
    a->beta         = beta;
    a->stall_margin = 1.0f - alpha / m->alpha_stall;

    const float qbar = 0.5f * rho * Vsafe * Vsafe;
    const float pb   = m->span  * a->rate[0] / (2.0f * Vsafe);
    const float qb   = m->chord * a->rate[1] / (2.0f * Vsafe);
    const float rb   = m->span  * a->rate[2] / (2.0f * Vsafe);

    /* ---- forces ----------------------------------------------------------- */
    const float cl = lift_coefficient(m, alpha) + m->cl_de * a->surf[0] + m->cl_q * qb;
    const float cd = m->cd0 + m->cd_k * cl * cl + m->cd_de * fabsf(a->surf[0]) +
                     0.020f * fabsf(a->surf[1]) + 0.030f * fabsf(a->surf[2]);
    const float cy = m->cy_beta * beta + m->cy_dr * a->surf[2];

    /* Thrust lapses with the air the engine is breathing. */
    const float thrust = a->throttle * m->thrust_max * powf(rho / FS_RHO_0, 0.70f);
    const float lift   = qbar * m->wing_area * cl;
    const float drag   = qbar * m->wing_area * cd;
    const float side   = qbar * m->wing_area * cy;

    const float g_world[3] = {0.0f, -FS_G, 0.0f};
    float       g_body[3];
    world_to_body(g_body, R, g_world);

    float force[3];
    force[0] = thrust - drag + m->mass * g_body[0];
    force[1] = side          + m->mass * g_body[1];
    force[2] = -lift         + m->mass * g_body[2];      /* body +Z is down      */

    /* ---- moments ---------------------------------------------------------- */
    /* L, M and N here are the roll, pitch and yaw moments, not the lift above.
     * The collision is the aerospace convention's, not ours.
     *
     * Signs, checked against the frame: a positive moment about body +X drops
     * the right wing, about +Y raises the nose, about +Z yaws the nose right.
     * So cm_alpha negative is a stable aircraft, cl_beta negative is dihedral
     * and cn_beta positive is the weathercock. */
    const float Lm = qbar * m->wing_area * m->span *
                     (m->cl_beta * beta + m->cl_p * pb + m->cl_r * rb +
                      m->cl_da * a->surf[1] + m->cl_dr * a->surf[2]);
    const float Mm = qbar * m->wing_area * m->chord *
                     (m->cm0 + m->cm_alpha * alpha + m->cm_q * qb + m->cm_de * a->surf[0]);
    const float Nm = qbar * m->wing_area * m->span *
                     (m->cn_beta * beta + m->cn_p * pb + m->cn_r * rb +
                      m->cn_da * a->surf[1] + m->cn_dr * a->surf[2]);

    const float Ix = m->inertia[0], Iy = m->inertia[1], Iz = m->inertia[2];
    const float p = a->rate[0], q = a->rate[1], r = a->rate[2];
    const float p_dot = (Lm - (Iz - Iy) * q * r) / Ix;
    const float q_dot = (Mm - (Ix - Iz) * p * r) / Iy;
    const float r_dot = (Nm - (Iy - Ix) * p * q) / Iz;

    /* ---- integrate -------------------------------------------------------- */
    /* Attitude first: dq/dt = 1/2 q (x) omega with omega a pure quaternion in
     * the body frame, which is the form that holds when q maps body to world. */
    const float omega_q[4] = {a->rate[0], a->rate[1], a->rate[2], 0.0f};
    float       qdot[4];
    mu_quat_mul(qdot, a->att, omega_q);
    for (int k = 0; k < 4; ++k) a->att[k] += 0.5f * dt * qdot[k];
    mu_quat_normalize(a->att, a->att);
    a->rate[0] += p_dot * dt;
    a->rate[1] += q_dot * dt;
    a->rate[2] += r_dot * dt;

    /* Body velocity, with the transport term: the body frame is rotating, so a
     * vector held constant in the world still changes in the body. */
    float cross[3];
    mu_vec3_cross(cross, a->rate, a->vel);
    for (int k = 0; k < 3; ++k) a->vel[k] += (force[k] / m->mass - cross[k]) * dt;

    /* Position from the world velocity, using the attitude just integrated. */
    mu_quat_rotate_vec3(a->vel_world, a->att, a->vel);
    for (int k = 0; k < 3; ++k) a->pos[k] += a->vel_world[k] * dt;

    /* ---- ground contact --------------------------------------------------- */
    const float clearance = 2.2f;                            /* the undercarriage */
    if (a->pos[1] < ground + clearance) {
        const float sink = a->vel_world[1];                  /* negative descending */
        const float hard = -sink > 4.5f || fabsf(a->pitch) > 0.30f || fabsf(a->roll) > 0.45f;
        if (hard && !s->crashed) {
            s->crashed     = true;
            s->crash_speed = mu_vec3_length(a->vel_world);
        }
        a->pos[1]      = ground + clearance;
        if (a->vel[2] > 0.0f) a->vel[2] = 0.0f;              /* stop descending    */
        a->vel_world[1] = 0.0f;
        /* On the ground the aircraft is a trolley: rates decay, the attitude is
         * left to the pilot, and rolling friction bleeds speed. */
        a->rate[0] *= 0.86f;
        a->rate[1] *= 0.86f;
        a->rate[2] *= 0.90f;
        a->vel[0]  *= 1.0f - mu_clamp(0.22f * dt, 0.0f, 1.0f);
    }

    /* ---- derived ---------------------------------------------------------- */
    mu_mat4_from_quat(R, a->att);
    fs_aircraft_derive(a, R, lift / (m->mass * FS_G));
}

void fs_step_arcade(Sim *s, Aircraft *a, float dt, float ground)
{
    const AeroModel *m = &s->model;

    /* A rate controller with a speed that chases a target. No stall, no spin,
     * no coupling: the stick asks for a rate and the aircraft produces it. That
     * is the point of the mode. It shares the attitude and position integration
     * with the real model so the camera, the HUD and the renderer cannot tell
     * the two apart, and so neither one has a second set of conventions. */
    a->throttle += mu_clamp(a->cmd_throttle - a->throttle, -1.2f * dt, 1.2f * dt);
    a->throttle  = mu_clamp(a->throttle, 0.0f, 1.0f);
    for (int k = 0; k < 3; ++k) a->surf[k] = mu_clamp(a->cmd[k], -1.0f, 1.0f);

    a->rate[0] = a->surf[1] * m->arcade_roll_rate;
    /* Negated so the stick means what it means in the six-DOF model: positive
     * elevator is nose DOWN in aerospace convention, so back stick (cmd -1)
     * has to produce a positive (nose-up) pitch rate here too. Switching modes
     * mid-flight must not invert the pitch axis. */
    a->rate[1] = -a->surf[0] * m->arcade_pitch_rate;
    a->rate[2] = a->surf[2] * m->arcade_yaw_rate;

    /* Speed lives entirely along the nose, so the body velocity is (V, 0, 0). */
    const float target = mu_lerp(m->arcade_speed_min, m->arcade_speed_max, a->throttle);
    a->vel[0] += (target - a->vel[0]) * mu_clamp(m->arcade_speed_lag * dt * 3.0f, 0.0f, 1.0f);
    a->vel[1]  = 0.0f;
    a->vel[2]  = 0.0f;
    a->airspeed     = a->vel[0];
    a->alpha        = 0.0f;
    a->beta         = 0.0f;
    a->stall_margin = 1.0f;
    a->rho          = FS_RHO_0 * expf(-mu_clamp(a->pos[1], 0.0f, 40000.0f) / FS_SCALE_H);

    const float omega_q[4] = {a->rate[0], a->rate[1], a->rate[2], 0.0f};
    float       qdot[4];
    mu_quat_mul(qdot, a->att, omega_q);
    for (int k = 0; k < 4; ++k) a->att[k] += 0.5f * dt * qdot[k];
    mu_quat_normalize(a->att, a->att);

    mu_quat_rotate_vec3(a->vel_world, a->att, a->vel);
    for (int k = 0; k < 3; ++k) a->pos[k] += a->vel_world[k] * dt;

    if (a->pos[1] < ground + 2.2f) {
        if (-a->vel_world[1] > 6.0f && !s->crashed) {
            s->crashed     = true;
            s->crash_speed = mu_vec3_length(a->vel_world);
        }
        a->pos[1]       = ground + 2.2f;
        a->vel_world[1] = 0.0f;
        a->vel[0]      *= 1.0f - mu_clamp(0.5f * dt, 0.0f, 1.0f);
    }

    float R[16];
    mu_mat4_from_quat(R, a->att);
    /* The load factor an arcade aircraft is asking for is geometric: it is what
     * the bank angle implies, and it is the number a player compares the turn
     * against. */
    fs_aircraft_derive(a, R, 1.0f / mu_clamp(cosf(a->roll), 0.20f, 1.0f));
}

/* ---- the traffic law ------------------------------------------------------ */

void fs_traffic_control(Sim *s, float dt)
{
    /* One specialised loop over one table. Every row does the same three things:
     * steer for its waypoint, hold its altitude, throttle to hold its speed. The
     * player is not in this table and never asks these questions, which is why
     * there is no "am I the player" test anywhere in here. */
    for (uint32_t i = 0u; i < s->traffic_count; ++i) {
        Aircraft    *a  = &s->traffic[i];
        const float *wp = s->waypoint[i];

        /* A fresh waypoint when the old one is reached, so the traffic flies a
         * circuit instead of orbiting a point it has already overflown. */
        const float dx = wp[0] - a->pos[0];
        const float dz = wp[2] - a->pos[2];
        if (dx * dx + dz * dz < 900.0f * 900.0f) {
            const float rad   = a->heading + mu_radians(55.0f);
            const float reach = 7000.0f + 5000.0f * (float)(i % 3u);
            s->waypoint[i][0] = a->pos[0] + sinf(rad) * reach;
            s->waypoint[i][2] = a->pos[2] - cosf(rad) * reach;
        }

        float err = atan2f(dx, -dz) - a->heading;
        while (err > 3.14159265f)  err -= 6.28318531f;
        while (err < -3.14159265f) err += 6.28318531f;

        /* Bank into the turn with an odd, saturating response, and a little
         * rudder so the turn is not visibly slipped. */
        a->cmd[1] = signed_smoothstep(mu_clamp(err * 1.9f, -1.0f, 1.0f));
        a->cmd[2] = a->cmd[1] * 0.35f;

        /* Altitude hold as a proportional term on the error minus a rate term,
         * which is what stops the overshoot that a pure P controller has. */
        a->cmd[0] = mu_clamp((wp[1] - a->pos[1]) * 0.0012f - a->vsi * 0.06f, -1.0f, 1.0f);
        a->cmd_throttle = mu_clamp(0.55f + (78.0f - a->airspeed) * 0.05f, 0.0f, 1.0f);
        (void)dt;
    }
}

/* ---- the frame ------------------------------------------------------------ */

void fs_sim_step(Sim *s, float dt, const GroundHeights *ground)
{
    /* Fixed ticks, capped. A frame that takes half a second must not ask for a
     * hundred and twenty integrations, or the simulation falls further behind
     * every time and never catches up. */
    s->accumulator += mu_clamp(dt, 0.0f, 0.25f);
    s->ticks_this_frame = 0u;

    /* The model dispatch is here, once per frame, and not inside a loop over
     * rows. */
    void (*const integrate)(Sim *, Aircraft *, float, float) =
        s->arcade ? fs_step_arcade : fs_step_sixdof;

    while (s->accumulator >= s->tick && s->ticks_this_frame < 24u) {
        s->accumulator -= s->tick;
        s->ticks_this_frame++;
        s->time += s->tick;

        /* Turbulence is a gust field sampled at the aircraft, not a random
         * number per tick. The same patch of air has to bump every aircraft
         * that flies through it, or it does not read as air; mu_fbm4 on the
         * position is the same field the shader could evaluate, so a gust is a
         * place in the world rather than an event in time. */
        if (s->turbulence > 0.0f) {
            const float t  = s->time * 0.35f;
            const float g  = s->turbulence * 3.4f;
            const float px = s->player.pos[0] * 0.0006f;
            const float pz = s->player.pos[2] * 0.0006f;
            s->wind[0] = s->wind_base[0] + mu_fbm4(px + t, pz, 2.0f, 0.5f) * g;
            s->wind[1] = s->wind_base[1] + mu_fbm4(px - t * 0.7f, pz, 2.0f, 0.5f) * g;
            s->wind[2] = s->wind_base[2] + mu_fbm4(px, pz + t * 1.3f, 2.0f, 0.5f) * g;
        }

        fs_traffic_control(s, s->tick);
        integrate(s, &s->player, s->tick, ground->player);
        for (uint32_t i = 0u; i < s->traffic_count; ++i)
            integrate(s, &s->traffic[i], s->tick, ground->traffic[i]);

        /* Compaction is its own pass, not a test inside the loop above. A row
         * that has hit the ground leaves the table by swap-remove, so the table
         * stays dense and being in [0, traffic_count) is the whole meaning of
         * "still flying". The parallel ground array moves with the row, which
         * is the reason this is a second loop: doing it inside the first would
         * mean integrating a row that has just moved into a slot already passed. */
        for (uint32_t i = 0u; i < s->traffic_count;) {
            if (s->traffic[i].pos[1] > ground->traffic[i] + 2.3f) { ++i; continue; }
            const uint32_t last = --s->traffic_count;
            s->traffic_lost++;
            if (i != last) {
                s->traffic[i]          = s->traffic[last];
                s->waypoint[i][0]      = s->waypoint[last][0];
                s->waypoint[i][1]      = s->waypoint[last][1];
                s->waypoint[i][2]      = s->waypoint[last][2];
                ((GroundHeights *)ground)->traffic[i] = ground->traffic[last];
            }
            /* Do not advance: the row now at i has not been tested. */
        }
    }
}

/* Level flight at heading zero, as a body-to-world quaternion. The body axes
 * are (forward, right, down) while the world axes are (east, up, south), so
 * the identity quaternion is NOT level flight: it maps the right wing into
 * the sky and the belly to the south, and an aircraft "trimmed" there lifts
 * sideways and tumbles. Every attitude therefore starts from this base,
 * which maps (forward, right, down) onto (north, east, down), and yaws are
 * composed onto it about the world's up axis. */
static const float k_att_level[4] = {0.5f, 0.5f, -0.5f, 0.5f};

void fs_sim_reset(Sim *s, const AeroModel *model, uint32_t traffic_count, uint64_t seed)
{
    /* Copied out before the memset, because the obvious way to reset an aircraft
     * in place is fs_sim_reset(&sim, &sim.model, ...), and that argument aliases
     * the struct being zeroed. Without this the airframe comes back with no mass
     * and no coefficients, and the next tick divides by zero. */
    const AeroModel m = *model;
    memset(s, 0, sizeof *s);
    s->model         = m;
    s->tick          = 1.0f / 240.0f;
    s->traffic_count = traffic_count > FS_MAX_TRAFFIC ? FS_MAX_TRAFFIC : traffic_count;
    s->turbulence    = 0.15f;
    s->wind_base[0]  = 4.0f;
    s->wind_base[2]  = -2.0f;
    s->wind[0]       = s->wind_base[0];
    s->wind[2]       = s->wind_base[2];

    /* The player starts in the air, trimmed, at a speed the wing can hold. */
    Aircraft   *p = &s->player;
    const float up[3] = {0.0f, 1.0f, 0.0f};
    p->pos[0] = 0.0f; p->pos[1] = 2200.0f; p->pos[2] = 0.0f;
    p->vel[0] = 78.0f; p->vel[1] = 0.0f; p->vel[2] = 0.0f;
    memcpy(p->att, k_att_level, sizeof p->att);
    p->throttle = 0.62f; p->cmd_throttle = 0.62f;
    p->rho = FS_RHO_0; p->airspeed = 78.0f;
    mu_quat_rotate_vec3(p->vel_world, p->att, p->vel);

    /* Traffic on a spread of headings and altitudes, from a seed so a run is
     * reproducible and --check can assert on what it produced. */
    mu_pcg32 rng;
    mu_pcg32_init(&rng, seed ? seed : 1u, 0x9e3779b97f4a7c15ULL);
    for (uint32_t i = 0u; i < s->traffic_count; ++i) {
        Aircraft    *a    = &s->traffic[i];
        const float u     = (float)mu_pcg32_next_u32(&rng) * (1.0f / 4294967296.0f);
        const float u2    = (float)mu_pcg32_next_u32(&rng) * (1.0f / 4294967296.0f);
        const float u3    = (float)mu_pcg32_next_u32(&rng) * (1.0f / 4294967296.0f);
        const float u4    = (float)mu_pcg32_next_u32(&rng) * (1.0f / 4294967296.0f);
        const float ang   = u * 6.28318531f;
        const float rad   = 3500.0f + u2 * 9000.0f;
        const float alt   = 1500.0f + u3 * 2600.0f;

        a->pos[0] = sinf(ang) * rad;
        a->pos[1] = alt;
        a->pos[2] = -cosf(ang) * rad;
        a->vel[0] = 72.0f + u4 * 18.0f;
        float qyaw[4];
        mu_quat_axis_angle(qyaw, up, -ang);
        mu_quat_mul(a->att, qyaw, k_att_level);
        a->throttle = 0.6f; a->cmd_throttle = 0.6f;
        a->rho = FS_RHO_0; a->airspeed = a->vel[0];
        mu_quat_rotate_vec3(a->vel_world, a->att, a->vel);

        const float wa = ang + 1.0f;
        s->waypoint[i][0] = a->pos[0] + sinf(wa) * 9000.0f;
        s->waypoint[i][1] = alt;
        s->waypoint[i][2] = a->pos[2] - cosf(wa) * 9000.0f;
    }
}

uint32_t fs_sim_draw_records(const Sim *s, float *records, uint32_t stride,
                             const float *player_colour, const float *traffic_colour)
{
    /* One record per row, player first, so the instance index is the row index
     * and the renderer never learns that two tables exist. The stride is the
     * caller's: the shader loads a record at `records + instance * stride`, so
     * the layout is 16 floats of model matrix then 4 of colour, and anything
     * else the caller wants to hang off the end is its business. */
    const uint32_t floats = stride / 4u;
    const float    scale[3] = {1.0f, 1.0f, 1.0f};
    const uint32_t count  = 1u + s->traffic_count;

    /* The beacon phase is per aircraft rather than global, so a formation does
     * not flash as one light. */
    for (uint32_t i = 0u; i < count; ++i) {
        const Aircraft *a   = i == 0u ? &s->player : &s->traffic[i - 1u];
        const float    *col = i == 0u ? player_colour : traffic_colour;
        float          *rec = records + (size_t)i * floats;
        const float     blink = 0.5f + 0.5f * sinf(s->time * 5.0f + (float)i * 1.7f);

        mu_mat4_trs(rec, a->pos, a->att, scale);
        rec[16] = col[0];
        rec[17] = col[1];
        rec[18] = col[2];
        rec[19] = blink * (i == 0u ? 0.9f : 0.5f);
    }
    return count;
}
