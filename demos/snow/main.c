/* snow — the material point method for snow, on the GPU.
 *
 *   (Clear -> P2G -> G2P) x K -> Draw -> Present
 *
 * Stomakhin, Schroeder, Chai, Teran, Selle: "A material point method for snow
 * simulation" (SIGGRAPH 2013) in a 64^3 grid, one metre across; see
 * shaders/snow.slang for the time step. The shaders do the explicit step.
 * method.c has the same step on the CPU and the implicit velocity solve of
 * the paper with the force derivatives of its technical document;
 * --implicit runs that instead, five times the time step, on the CPU.
 *
 * --check tests the material's derivatives and the node forces against
 * finite differences, one GPU step against the CPU, conservation, free
 * fall, and the implicit step where the explicit one blows up.
 */
#include "demo.h"
#include "method.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SHADERS   "demos/snow/shaders/"
#define N         SNOW_N
#define MAX_COUNT 32768u
#define MAX_STEPS 20u            /* three passes each; a frame program holds 64 */
#define WALLS     3.0f
#define TIME_STEP 1.0e-4f
#define IMPLICIT_STEP 5.0e-4   /* the paper's */

typedef struct Push {
    uint32_t particles, grid, count, n;
    float    dt, gravity, mu, lambda;
    float    hardening, compress, stretch, flip;
    float    friction, walls;
    uint32_t sticky;
    float    pad;
    float    ball[4], ball_velocity[4];
} Push;
_Static_assert(sizeof(Push) == 96, "Push must match snow.slang");

typedef struct DrawPush {
    float    eye[4], right[4], up[4], forward[4], ball[4];
    uint32_t particles, count, view;
    float    walls;
} DrawPush;
_Static_assert(sizeof(DrawPush) == 96, "DrawPush must match draw.slang");

static const char *const k_views[]  = {"snow", "speed", "packed (blue) or torn (red)", "squeezed or stretched"};
static const char *const k_scenes[] = {"a snowball thrown at a wall", "two snowballs meet", "a slab falls on a ball", "a ball ploughs through snow"};

typedef struct App {
    Demo          demo;
    DemoOrbit     orbit;
    BufSlice      particles, grid;
    BufUse        use_particles_read, use_particles_write, use_grid_read, use_grid_write, use_draw;
    AttachDesc    depth;
    SnowParticle *seed;
    uint32_t      count;
    float         ball[4], ball_velocity[3], ball_stop;   /* the ball moves until its x reaches ball_stop */
    /* the CPU's grid, for --implicit and --check */
    int32_t      *sums;
    double       *mass, *before, *after;
    uint8_t      *held;
    /* options */
    uint32_t      scene, steps, view;
    float         stiffness;     /* Young's modulus / density of fresh snow, m^2/s^2 */
    float         hardening, compress, stretch, flip, friction, size;
    bool          implicit, sticky, restart;
    uint32_t      run;           /* time steps this frame runs on the GPU, at most `steps`; --check changes it */
} App;

static SnowParams params(const App *a, double dt) {
    /* Poisson's ratio 0.2 */
    return (SnowParams){.dt = dt, .gravity = 9.8 * N, .mu = (double)a->stiffness * N * N / 2.4,
                        .lambda = (double)a->stiffness * N * N * 0.2 / (1.2 * 0.6), .hardening = a->hardening,
                        .compress = a->compress, .stretch = a->stretch, .flip = a->flip, .friction = a->friction, .walls = WALLS,
                        .ball = {a->ball[0], a->ball[1], a->ball[2], a->ball[3]},
                        .ball_velocity = {a->ball_velocity[0], a->ball_velocity[1], a->ball_velocity[2]}, .sticky = a->sticky};
}

/* The ball after one more time step; it stops where the scene says. */
static void move_ball(App *a, double dt) {
    uint32_t k;
    for (k = 0u; k < 3u; ++k) a->ball[k] += (float)dt * a->ball_velocity[k];
    if (a->ball[0] >= a->ball_stop) a->ball_velocity[0] = a->ball_velocity[1] = a->ball_velocity[2] = 0.0f;
}

/* The starting state: balls and blocks of fresh snow, eight particles to a cell, at rest inside. */
static void seed(App *a) {
    /* centre, half size (a ball when all three are equal), velocity in cells / s */
    static const struct { float centre[3], half[3], vel[3]; } k_bodies[4][2] = {
        {{{44.0f, 26.0f, 32.0f}, {7.0f, 7.0f, 7.0f}, {650.0f, 60.0f, 0.0f}}, {{0}, {0}, {0}}},
        {{{22.0f, 30.0f, 32.0f}, {6.0f, 6.0f, 6.0f}, {450.0f, 80.0f, 0.0f}}, {{42.0f, 31.0f, 33.0f}, {6.0f, 6.0f, 6.0f}, {-450.0f, 80.0f, 0.0f}}},
        {{{32.0f, 27.5f, 32.0f}, {11.0f, 3.5f, 11.0f}, {0.0f, -250.0f, 0.0f}}, {{0}, {0}, {0}}},
        {{{36.0f, 6.2f, 32.0f}, {16.0f, 3.0f, 9.0f}, {0.0f, 0.0f, 0.0f}}, {{0}, {0}, {0}}}};
    /* centre, radius, velocity, where it stops */
    static const float k_balls[4][8] = {{0}, {0}, {32.0f, 13.0f, 32.0f, 8.0f, 0.0f, 0.0f, 0.0f, 0.0f},
                                        {11.0f, 8.5f, 32.0f, 5.0f, 300.0f, 0.0f, 0.0f, 50.0f}};
    uint32_t b, c;
    int      x, y, z;
    a->count = 0u;
    memset(a->seed, 0, (size_t)MAX_COUNT * sizeof(SnowParticle));
    for (b = 0u; b < 2u; ++b) {
        const float *centre = k_bodies[a->scene][b].centre, *half = k_bodies[a->scene][b].half;
        const bool   round = half[0] == half[1] && half[1] == half[2];
        for (z = (int)(-2.0f * half[2]); z < (int)(2.0f * half[2]); ++z)
            for (y = (int)(-2.0f * half[1]); y < (int)(2.0f * half[1]); ++y)
                for (x = (int)(-2.0f * half[0]); x < (int)(2.0f * half[0]); ++x) {
                    const float at[3] = {0.5f * ((float)x + 0.5f), 0.5f * ((float)y + 0.5f), 0.5f * ((float)z + 0.5f)};
                    SnowParticle *p = &a->seed[a->count];
                    uint32_t h = (uint32_t)(x + 64) * 73856093u ^ (uint32_t)(y + 64) * 19349663u ^ (uint32_t)(z + 64) * 83492791u ^ b;
                    if (round && at[0] * at[0] + at[1] * at[1] + at[2] * at[2] > half[0] * half[0]) continue;
                    if (a->count == MAX_COUNT) continue;
                    for (c = 0u; c < 3u; ++c) {
                        h = h * 747796405u + 2891336453u;
                        h = ((h >> ((h >> 28u) + 4u)) ^ h) * 277803737u;
                        p->pos[c]  = centre[c] + at[c] + 0.3f * ((float)(h >> 16u) / 65535.0f - 0.5f);
                        p->vel[c]  = k_bodies[a->scene][b].vel[c];
                        p->f[c][c] = 1.0f;
                    }
                    p->jp = 1.0f;
                    ++a->count;
                }
    }
    memcpy(a->ball, k_balls[a->scene], sizeof a->ball);
    memcpy(a->ball_velocity, &k_balls[a->scene][4], sizeof a->ball_velocity);
    a->ball_stop = k_balls[a->scene][7];
    snow_volumes(a->seed, a->count);
    demo_upload(&a->demo, a->particles, a->seed, (uint64_t)MAX_COUNT * sizeof(SnowParticle));
}

/* One time step on the CPU: beta 0 is the shaders' step, beta 1 the paper's implicit one. */
static uint32_t cpu_step(App *a, SnowParticle *particles, uint32_t count, const SnowParams *p, double beta, double *residual) {
    uint32_t i, iterations = 0u;
    snow_p2g(p, particles, count, a->sums);
    snow_velocities(p, a->sums, a->mass, a->before, a->after, a->held);
    if (beta > 0.0) iterations = snow_implicit(p, particles, count, beta, a->mass, a->held, a->after, residual);
    for (i = 0u; i < count; ++i) snow_g2p(p, &particles[i], a->before, a->after);
    return iterations;
}

static void app_frame(App *a) {
    Demo            *d = &a->demo;
    const SnowParams p = params(a, TIME_STEP);
    const float      aspect = (float)d->width / (float)d->height;
    PassCtx         *pass;
    uint32_t         i;
    float            view[16], eye[3];
    Push     push = {.particles = (uint32_t)a->particles.offset, .grid = (uint32_t)a->grid.offset, .count = a->count, .n = N,
                     .dt = TIME_STEP, .gravity = (float)p.gravity, .mu = (float)p.mu, .lambda = (float)p.lambda,
                     .hardening = a->hardening, .compress = a->compress, .stretch = a->stretch, .flip = a->flip,
                     .friction = a->friction, .walls = WALLS, .sticky = a->sticky};
    DrawPush draw = {.particles = (uint32_t)a->particles.offset, .count = a->count, .view = a->view, .walls = WALLS};

    if (a->restart) { seed(a); a->restart = false; }
    if (a->implicit) {
        /* The paper's implicit step on the CPU, 0.5 ms as in the paper, twice, so that a frame is 1 ms of
         * the scene as it is with the shaders. Then the particles go to the GPU to be drawn. */
        for (i = 0u; i < 2u; ++i) {
            const SnowParams slow = params(a, IMPLICIT_STEP);
            cpu_step(a, a->seed, a->count, &slow, 1.0, NULL);
            move_ball(a, IMPLICIT_STEP);
        }
        demo_upload(d, a->particles, a->seed, (uint64_t)MAX_COUNT * sizeof(SnowParticle));
    }

    demo_orbit_input(d, &a->orbit);
    demo_orbit_camera(&a->orbit, aspect, view, NULL, NULL, eye);
    /* Rows of the rotation part of a column-major view matrix are the camera axes; the camera looks down -Z. */
    for (i = 0u; i < 3u; ++i) {
        draw.eye[i]     = eye[i];
        draw.right[i]   = view[4u * i];
        draw.up[i]      = view[4u * i + 1u];
        draw.forward[i] = -view[4u * i + 2u];
    }
    draw.eye[3]     = tanf(a->orbit.fov_deg * 0.017453292f * 0.5f);
    draw.right[3]   = aspect;
    draw.up[3]      = a->size;
    draw.forward[3] = (float)N;
    d->present.tonemap = DEMO_TONEMAP_NONE;

    if (!demo_frame_begin(d)) return;
    for (i = 0u; i < 3u * a->steps; ++i) {
        pass = demo_pass(d, NULL);   /* "Clear" / "P2G" / "G2P" */
        if (a->implicit || i / 3u >= a->run) continue;
        memcpy(push.ball, a->ball, sizeof a->ball);
        memcpy(push.ball_velocity, a->ball_velocity, sizeof a->ball_velocity);
        pass_push(pass, &push, sizeof push);
        pass_dispatch(pass, i % 3u == 0u ? N * N * N / 64u : (a->count + 63u) / 64u, 1u, 1u);
        if (i % 3u == 2u) move_ball(a, TIME_STEP);
    }
    memcpy(draw.ball, a->ball, sizeof a->ball);
    pass = demo_pass(d, "Draw");
    pass_push(pass, &draw, sizeof draw);
    pass_draw(pass, (3u + a->count) * 6u, 1u, 0u, 0u);
    demo_present(d);
    demo_frame_end(d);
}

static void app_init(App *a) {
    static const char *const k_names[3] = {"Clear", "P2G", "G2P"};
    Demo    *d = &a->demo;
    PassDesc passes[3u * MAX_STEPS + 3u];
    uint32_t count = 0u, i;
    Target   hdr = demo_target(d, "snow_hdr", FMT_RGBA16F, d->width, d->height, false, NULL);
    Target   depth = demo_target(d, "snow_depth", FMT_D32F, d->width, d->height, false, NULL);
    Pipe     pipes[3];

    a->seed      = (SnowParticle *)malloc((size_t)MAX_COUNT * sizeof(SnowParticle));
    a->sums      = (int32_t *)malloc((size_t)SNOW_NODES * 8u * sizeof(int32_t));
    a->mass      = (double *)malloc((size_t)SNOW_NODES * sizeof(double));
    a->before    = (double *)malloc((size_t)SNOW_NODES * 3u * sizeof(double));
    a->after     = (double *)malloc((size_t)SNOW_NODES * 3u * sizeof(double));
    a->held      = (uint8_t *)malloc((size_t)SNOW_NODES);
    a->particles = demo_alloc(d, NULL, (uint64_t)MAX_COUNT * sizeof(SnowParticle));
    a->grid      = demo_alloc(d, NULL, (uint64_t)SNOW_NODES * 32u);
    seed(a);

    a->use_particles_read  = use_read(a->particles.buffer, a->particles.offset, a->particles.size);
    a->use_particles_write = use_write(a->particles.buffer, a->particles.offset, a->particles.size);
    a->use_grid_read       = use_read(a->grid.buffer, a->grid.offset, a->grid.size);
    a->use_grid_write      = use_write(a->grid.buffer, a->grid.offset, a->grid.size);
    a->use_draw            = a->use_particles_read;
    a->use_draw.stages     = SH_VERT;
    a->depth               = attach_depth_clear(depth, 1.0f);

    pipes[0] = pipe_intern_compute(d->gpu, demo_shader(d, SHADERS "clear.comp.spv", STAGE_COMPUTE), "main");
    pipes[1] = pipe_intern_compute(d->gpu, demo_shader(d, SHADERS "p2g.comp.spv", STAGE_COMPUTE), "main");
    pipes[2] = pipe_intern_compute(d->gpu, demo_shader(d, SHADERS "g2p.comp.spv", STAGE_COMPUTE), "main");
    for (i = 0u; i < 3u * a->steps; ++i) {
        PassDesc *p = &passes[count++];
        *p = (PassDesc){.kind = PASS_COMPUTE, .state = draw_state_opaque(SC_1, CULL_NONE), .pipe = pipes[i % 3u]};
        strcpy(p->name, k_names[i % 3u]);
        if (i % 3u == 1u) { p->buf_reads = &a->use_particles_read; p->buf_read_count = 1u; }
        if (i % 3u == 2u) { p->buf_reads = &a->use_grid_read; p->buf_read_count = 1u; }
        p->buf_writes      = i % 3u == 2u ? &a->use_particles_write : &a->use_grid_write;
        p->buf_write_count = 1u;
    }
    passes[count++] = (PassDesc){
        .name = "Draw", .kind = PASS_GRAPHICS, .state = draw_state_opaque(SC_1, CULL_NONE),
        .pipe = pipe_intern_graphics(d->gpu, demo_shader(d, SHADERS "draw.vert.spv", STAGE_VERTEX),
                                     demo_shader(d, SHADERS "draw.frag.spv", STAGE_FRAGMENT), FMT_RGBA16F, FMT_D32F, BLEND_OPAQUE),
        .colors = (AttachDesc[]){attach_clear(hdr, 0.02f, 0.024f, 0.034f, 1.0f)}, .color_count = 1u, .depth = &a->depth,
        .buf_reads = &a->use_draw, .buf_read_count = 1u};
    passes[count++] = demo_present_pass(d, hdr);
    d->program = demo_program(d, passes, count);

    a->run = a->steps;
    if (d->headless && !d->frames) d->frames = 40u;
}

static void app_ui(App *a, struct nk_context *ctx) {
    Demo *d = &a->demo;
    if (nk_begin(ctx, "Snow", nk_rect(10, 10, 310, 380), NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_TITLE)) {
        demo_ui_text(ctx, "%.1f ms   %u particles", (double)d->dt * 1000.0, a->count);
        demo_ui_text(ctx, "%.1f ms of the scene a frame", a->implicit ? 2.0 * IMPLICIT_STEP * 1000.0 : (double)(a->steps * TIME_STEP) * 1000.0);
        if (demo_ui_choice(ctx, "Scene", k_scenes, 4u, &a->scene)) a->restart = true;
        demo_ui_bool(ctx, "Start again", &a->restart);
        demo_ui_bool(ctx, "Implicit step (CPU)", &a->implicit);
        demo_ui_bool(ctx, "Sticky walls", &a->sticky);
        demo_ui_float(ctx, "Stiffness E / density", &a->stiffness, 50.0f, 700.0f);
        demo_ui_float(ctx, "Hardening", &a->hardening, 0.0f, 15.0f);
        demo_ui_float(ctx, "Critical compression", &a->compress, 0.005f, 0.05f);
        demo_ui_float(ctx, "Critical stretch", &a->stretch, 0.001f, 0.02f);
        demo_ui_float(ctx, "FLIP share", &a->flip, 0.0f, 1.0f);
        demo_ui_float(ctx, "Friction", &a->friction, 0.0f, 2.0f);
        demo_ui_choice(ctx, "View", k_views, 4u, &a->view);
        demo_ui_float(ctx, "Disc radius (cells)", &a->size, 0.2f, 1.0f);
    }
    nk_end(ctx);
}

/* ---- acceptance ------------------------------------------------------------ */

/* A number in -1 .. 1; the same sequence every run. */
static double noise(uint32_t *state) {
    *state = *state * 1664525u + 1013904223u;
    return (double)(*state >> 8) / 8388608.0 - 1.0;
}

static void app_check(App *a) {
    Demo           *d = &a->demo;
    const size_t    bytes = (size_t)MAX_COUNT * sizeof(SnowParticle), vector = (size_t)SNOW_NODES * 3u * sizeof(double);
    const SnowParams p = params(a, TIME_STEP);
    SnowParticle   *before = (SnowParticle *)malloc(bytes), *after = (SnowParticle *)malloc(bytes), *smashed = (SnowParticle *)malloc(bytes);
    int32_t        *grid = (int32_t *)malloc((size_t)SNOW_NODES * 32u);
    double         *move = (double *)calloc(1u, vector), *move2 = (double *)calloc(1u, vector), *plus = (double *)malloc(vector),
                   *minus = (double *)malloc(vector), *df = (double *)malloc(vector), *df2 = (double *)malloc(vector);
    uint32_t        rng = 12345u, i, f, r, c, k;
    size_t          n;

    /* 1. The scene the run left behind. */
    {
        double bad = 0.0, outside = 0.0, fastest = 0.0, low = 1e9, high = -1e9, packed = 1e9, torn = -1e9;
        demo_readback_buffer(d, a->particles, smashed, bytes);
        for (i = 0u; i < a->count; ++i) {
            const SnowParticle *q = &smashed[i];
            double cc[3][3], v[3][3], lam[3];
            const double speed = sqrt((double)q->vel[0] * q->vel[0] + (double)q->vel[1] * q->vel[1] + (double)q->vel[2] * q->vel[2]);
            if (!(q->pos[0] == q->pos[0] && q->pos[1] == q->pos[1] && q->pos[2] == q->pos[2] && speed == speed && q->jp == q->jp)) { bad += 1.0; continue; }
            for (k = 0u; k < 3u; ++k)
                if (q->pos[k] < WALLS - 1.0f || q->pos[k] > (float)N - WALLS) outside += 1.0;
            for (r = 0u; r < 3u; ++r)
                for (c = 0u; c < 3u; ++c) cc[r][c] = (double)q->f[0][r] * q->f[0][c] + (double)q->f[1][r] * q->f[1][c] + (double)q->f[2][r] * q->f[2][c];
            snow_eigen(cc, v, lam);
            for (k = 0u; k < 3u; ++k) { low = fmin(low, sqrt(lam[k])); high = fmax(high, sqrt(lam[k])); }
            packed  = fmin(packed, q->jp);
            torn    = fmax(torn, q->jp);
            fastest = fmax(fastest, speed);
        }
        demo_check(d, "after the run: particles that are not numbers", bad, "== 0", bad == 0.0);
        demo_check(d, "  coordinates more than a cell inside a wall", outside, "== 0", outside == 0.0);
        demo_check(d, "  fastest particle, cells per time step", fastest * TIME_STEP, "< 0.5", fastest * TIME_STEP < 0.5);
        demo_check(d, "  smallest singular value of F_E - (1 - theta_c)", low - (1.0 - a->compress), "> -1e-5", low - (1.0 - a->compress) > -1e-5);
        demo_check(d, "  largest - (1 + theta_s)", high - (1.0 + a->stretch), "< 1e-5", high - (1.0 + a->stretch) < 1e-5);
        demo_check(d, "  J_P, most packed (reported)", packed, "", true);
        demo_check(d, "  J_P, most torn (reported)", torn, "", true);
    }

    /* 2. The material, in double: the rotation, and the two derivatives of the technical document against differences. */
    {
        double turn = 0.0, product = 0.0, first = 0.0, second = 0.0, weights = 0.0;
        for (i = 0u; i < 200u; ++i) {
            const double jp = 1.0 + 0.1 * noise(&rng), step = 1e-5;
            double m[3][3], dm[3][3], rot[3][3], sym[3][3], stress[3][3], change[3][3], hi[3][3], lo[3][3], scale = 0.0, scale2 = 0.0;
            for (r = 0u; r < 3u; ++r)
                for (c = 0u; c < 3u; ++c) {
                    m[r][c]  = (r == c ? 1.0 : 0.0) + 0.3 * noise(&rng);
                    dm[r][c] = noise(&rng);
                }
            snow_polar(m, rot, sym);
            for (r = 0u; r < 3u; ++r)
                for (c = 0u; c < 3u; ++c) {
                    turn    = fmax(turn, fabs(rot[0][r] * rot[0][c] + rot[1][r] * rot[1][c] + rot[2][r] * rot[2][c] - (r == c ? 1.0 : 0.0)));
                    product = fmax(product, fabs(rot[r][0] * sym[0][c] + rot[r][1] * sym[1][c] + rot[r][2] * sym[2][c] - m[r][c]));
                    product = fmax(product, fabs(sym[r][c] - sym[c][r]));
                }
            /* dPsi/dF against (Psi(F + e) - Psi(F - e)) / 2e, entry by entry. */
            snow_stress(&p, m, jp, stress);
            snow_stress_change(&p, m, jp, dm, change);
            for (r = 0u; r < 3u; ++r)
                for (c = 0u; c < 3u; ++c) { scale = fmax(scale, fabs(stress[r][c])); scale2 = fmax(scale2, fabs(change[r][c])); }
            for (r = 0u; r < 3u; ++r)
                for (c = 0u; c < 3u; ++c) {
                    double up, down;
                    memcpy(hi, m, sizeof hi);
                    hi[r][c] += step;
                    up = snow_energy(&p, hi, jp);
                    hi[r][c] -= 2.0 * step;
                    down = snow_energy(&p, hi, jp);
                    first = fmax(first, fabs((up - down) / (2.0 * step) - stress[r][c]) / scale);
                }
            /* Its change for a change dF against (dPsi/dF(F + e dF) - dPsi/dF(F - e dF)) / 2e. */
            for (r = 0u; r < 3u; ++r)
                for (c = 0u; c < 3u; ++c) { hi[r][c] = m[r][c] + step * dm[r][c]; lo[r][c] = m[r][c] - step * dm[r][c]; }
            snow_stress(&p, hi, jp, hi);
            snow_stress(&p, lo, jp, lo);
            for (r = 0u; r < 3u; ++r)
                for (c = 0u; c < 3u; ++c) second = fmax(second, fabs((hi[r][c] - lo[r][c]) / (2.0 * step) - change[r][c]) / scale2);
        }
        for (i = 0u; i < 200u; ++i) {
            const float pos[3] = {20.0f + 10.0f * (float)noise(&rng), 20.0f + 10.0f * (float)noise(&rng), 20.0f + 10.0f * (float)noise(&rng)};
            int    base[3];
            double w[3][4], dw[3][4];
            snow_weights(pos, base, w, dw);
            for (k = 0u; k < 3u; ++k) {
                double sum = 0.0, slope = 0.0, centre = 0.0, lean = 0.0;
                for (c = 0u; c < 4u; ++c) {
                    sum    += w[k][c];
                    slope  += dw[k][c];
                    centre += w[k][c] * (base[k] + (int)c);
                    lean   += dw[k][c] * (base[k] + (int)c);
                }
                weights = fmax(weights, fmax(fmax(fabs(sum - 1.0), fabs(slope)), fmax(fabs(centre - pos[k]), fabs(lean - 1.0))));
            }
        }
        demo_check(d, "rotation of 200 random F: largest entry of R^T R - 1", turn, "< 1e-12", turn < 1e-12);
        demo_check(d, "  of R S - F, and of S - S^T", product, "< 1e-12", product < 1e-12);
        demo_check(d, "dPsi/dF - differences of Psi, / largest entry", first, "< 1e-6", first < 1e-6);
        demo_check(d, "change of dPsi/dF (with dR, technical document) - differences of dPsi/dF, / largest entry", second, "< 1e-6", second < 1e-6);
        demo_check(d, "cubic weights: sum - 1, sum of slopes, sum w x - x, sum slope x - 1, largest", weights, "< 1e-5", weights < 1e-5);
    }

    /* A ball in the air with every particle deformed a little, some packed and some torn. */
    a->scene = 0u;
    seed(a);
    for (i = 0u; i < a->count; ++i) {
        for (r = 0u; r < 3u; ++r)
            for (c = 0u; c < 3u; ++c) a->seed[i].f[r][c] += 0.015f * (float)noise(&rng);
        a->seed[i].jp = 1.0f + 0.1f * (float)noise(&rng);
    }
    demo_upload(d, a->particles, a->seed, bytes);

    /* 3. The node forces of the technical document, on the CPU. */
    {
        double worst = 0.0, largest = 0.0, total[3] = {0, 0, 0}, off = 0.0, big = 0.0, cross[2] = {0.0, 0.0};
        snow_forces(&p, a->seed, a->count, NULL, df, NULL);
        for (n = 0u; n < (size_t)SNOW_NODES * 3u; ++n) { largest = fmax(largest, fabs(df[n])); total[n % 3u] += df[n]; }
        /* (2) against (Phi(x - e) - Phi(x + e)) / 2e for one node moved along one axis, 24 times. */
        for (i = 0u; i < 24u; ++i) {
            const SnowParticle *q = &a->seed[(i * 467u) % a->count];
            const size_t at = (((size_t)q->pos[2] * N + (size_t)q->pos[1]) * N + (size_t)q->pos[0]) * 3u + i % 3u;
            double up, down;
            move[at] = 1e-4;
            snow_forces(&p, a->seed, a->count, move, NULL, &up);
            move[at] = -1e-4;
            snow_forces(&p, a->seed, a->count, move, NULL, &down);
            move[at] = 0.0;
            worst = fmax(worst, fabs((down - up) / 2e-4 - df[at]));
        }
        demo_check(d, "node force (eq. 2) - differences of the energy (eq. 1), 24 nodes, / largest force", worst / largest, "< 1e-5", worst / largest < 1e-5);
        demo_check(d, "  sum of all node forces / largest force", fmax(fabs(total[0]), fmax(fabs(total[1]), fabs(total[2]))) / largest, "< 1e-6",
                   fmax(fabs(total[0]), fmax(fabs(total[1]), fabs(total[2]))) / largest < 1e-6);
        /* (3, 4) against (f(x + e du) - f(x - e du)) / 2e with every node moved at random. */
        for (n = 0u; n < (size_t)SNOW_NODES * 3u; ++n) { move[n] = noise(&rng); move2[n] = noise(&rng); }
        snow_force_change(&p, a->seed, a->count, move, df);
        snow_force_change(&p, a->seed, a->count, move2, df2);
        for (n = 0u; n < (size_t)SNOW_NODES * 3u; ++n) { cross[0] += move2[n] * df[n]; cross[1] += move[n] * df2[n]; move2[n] = 1e-5 * move[n]; }
        snow_forces(&p, a->seed, a->count, move2, plus, NULL);
        for (n = 0u; n < (size_t)SNOW_NODES * 3u; ++n) move2[n] = -move2[n];
        snow_forces(&p, a->seed, a->count, move2, minus, NULL);
        for (n = 0u; n < (size_t)SNOW_NODES * 3u; ++n) {
            off = fmax(off, fabs((plus[n] - minus[n]) / 2e-5 - df[n]));
            big = fmax(big, fabs(df[n]));
        }
        demo_check(d, "change of the node forces (eq. 3, 4) - differences of the forces, / largest change", off / big, "< 1e-5", off / big < 1e-5);
        demo_check(d, "  it is symmetric: (u . df(w) - w . df(u)) / u . df(w)", (cross[0] - cross[1]) / cross[0], "< 1e-9",
                   fabs((cross[0] - cross[1]) / cross[0]) < 1e-9);
    }

    /* 4. One step of the shaders against method.c, from what the run left behind: the snowball packed
     * against the wall. A ball moving into it and sticky walls are added so that every collision rule is in it. */
    {
        double worst[3] = {0, 0, 0}, sum[7] = {0, 0, 0, 0, 0, 0, 0}, carried[3] = {0, 0, 0}, apart[4] = {0, 0, 0, 0}, momentum = 0.0, force = 0.0;
        double touched = 0.0;
        SnowParams hit;
        a->ball[0] = 55.0f; a->ball[1] = 27.0f; a->ball[2] = 32.0f; a->ball[3] = 4.0f;
        a->ball_velocity[0] = -200.0f; a->ball_velocity[1] = 50.0f; a->ball_velocity[2] = 0.0f;
        a->ball_stop = 1e9f;
        a->sticky    = true;
        hit = params(a, TIME_STEP);
        memcpy(before, smashed, bytes);
        demo_upload(d, a->particles, smashed, bytes);
        a->run = 1u;
        app_frame(a);
        a->run    = a->steps;
        a->sticky = false;
        demo_readback_buffer(d, a->grid, grid, (size_t)SNOW_NODES * 32u);
        demo_readback_buffer(d, a->particles, after, bytes);
        snow_p2g(&hit, before, a->count, a->sums);
        for (n = 0u; n < (size_t)SNOW_NODES * 8u; ++n) {
            const uint32_t slot = (uint32_t)(n % 8u), kind = slot == 0u ? 0u : slot < 4u ? 1u : 2u;
            if (slot == 7u) continue;
            worst[kind] = fmax(worst[kind], fabs((double)grid[n] - (double)a->sums[n]));
            sum[slot]  += grid[n];
        }
        for (i = 0u; i < a->count; ++i)
            for (k = 0u; k < 3u; ++k) carried[k] += before[i].vel[k];
        demo_check(d, "one step, grid on the GPU - on the CPU: mass, particles", worst[0] / SNOW_MASS_SCALE, "< 1e-5", worst[0] / SNOW_MASS_SCALE < 1e-5);
        demo_check(d, "  momentum, particle-cells / s", worst[1] / SNOW_MOMENTUM_SCALE, "< 0.01", worst[1] / SNOW_MOMENTUM_SCALE < 0.01);
        demo_check(d, "  dt x force, particle-cells / s", worst[2] / SNOW_IMPULSE_SCALE, "< 0.01", worst[2] / SNOW_IMPULSE_SCALE < 0.01);
        demo_check(d, "mass on the grid / number of particles - 1", sum[0] / SNOW_MASS_SCALE / a->count - 1.0, "< 1e-5",
                   fabs(sum[0] / SNOW_MASS_SCALE / a->count - 1.0) < 1e-5);
        for (k = 0u; k < 3u; ++k) {
            momentum = fmax(momentum, fabs(sum[1u + k] / SNOW_MOMENTUM_SCALE - carried[k]) / a->count);
            force    = fmax(force, fabs(sum[4u + k] / SNOW_IMPULSE_SCALE) / a->count);
        }
        demo_check(d, "momentum on the grid - momentum of the particles, per particle, cells / s", momentum, "< 1e-3", momentum < 1e-3);
        demo_check(d, "sum of dt x force over the grid, per particle, cells / s (forces inside the snow cancel)", force, "< 1e-3", force < 1e-3);
        /* The particles, from the GPU's own grid. */
        snow_velocities(&hit, grid, a->mass, a->before, a->after, a->held);
        for (n = 0u; n < SNOW_NODES; ++n) touched += a->held[n];
        demo_check(d, "  nodes whose velocity a wall or the ball changed in that step", touched, "> 100", touched > 100.0);
        for (i = 0u; i < a->count; ++i) {
            snow_g2p(&hit, &before[i], a->before, a->after);
            for (k = 0u; k < 3u; ++k) {
                apart[0] = fmax(apart[0], fabs((double)before[i].pos[k] - after[i].pos[k]));
                apart[1] = fmax(apart[1], fabs((double)before[i].vel[k] - after[i].vel[k]));
                for (c = 0u; c < 3u; ++c) apart[2] = fmax(apart[2], fabs((double)before[i].f[k][c] - after[i].f[k][c]));
            }
            apart[3] = fmax(apart[3], fabs((double)before[i].jp - after[i].jp));
        }
        demo_check(d, "one step, particles on the GPU - on the CPU (from the GPU's grid): position, cells", apart[0], "< 1e-4", apart[0] < 1e-4);
        demo_check(d, "  velocity, cells / s", apart[1], "< 0.05", apart[1] < 0.05);
        demo_check(d, "  F_E, largest entry", apart[2], "< 1e-5", apart[2] < 1e-5);
        demo_check(d, "  J_P", apart[3], "< 1e-5", apart[3] < 1e-5);
    }

    /* 5. A ball let go in the air falls as one piece: g dt^2 n (n + 1) / 2 after n steps. */
    {
        const uint32_t steps = 40u * a->steps;
        const double   expect = p.gravity * TIME_STEP * TIME_STEP * steps * (steps + 1.0) / 2.0;
        double fall = 0.0, drift = 0.0, shape = 0.0;
        seed(a);
        for (i = 0u; i < a->count; ++i) a->seed[i].vel[0] = a->seed[i].vel[1] = a->seed[i].vel[2] = 0.0f;
        demo_upload(d, a->particles, a->seed, bytes);
        for (f = 0u; f < 40u; ++f) app_frame(a);
        demo_readback_buffer(d, a->particles, after, bytes);
        for (i = 0u; i < a->count; ++i) {
            fall += ((double)a->seed[i].pos[1] - after[i].pos[1]) / a->count;
            drift = fmax(drift, fmax(fabs((double)after[i].pos[0] - a->seed[i].pos[0]), fabs((double)after[i].pos[2] - a->seed[i].pos[2])));
            for (r = 0u; r < 3u; ++r)
                for (c = 0u; c < 3u; ++c) shape = fmax(shape, fabs((double)after[i].f[r][c] - (r == c ? 1.0 : 0.0)));
            shape = fmax(shape, fabs((double)after[i].jp - 1.0));
        }
        demo_check(d, "a ball falling for 400 steps: distance / (g dt^2 n (n + 1) / 2) - 1", fall / expect - 1.0, "< 1e-3", fabs(fall / expect - 1.0) < 1e-3);
        demo_check(d, "  sideways drift, cells", drift, "< 1e-3", drift < 1e-3);
        demo_check(d, "  largest change of F_E or J_P", shape, "< 1e-4", shape < 1e-4);
    }

    /* 6. The implicit step of 0.5 ms on the snowball packed against a sticky wall, with the ball of
     * check 4 moving into it. Nodes the wall or the ball holds are left out of the solve; for the others (m v - dt^2 df(v) - m v*) is worked out again from eq. 3, 4. */
    {
        SnowParams wall;
        double     residual = 0.0, check = 0.0, size = 0.0, moved = 0.0, holds = 0.0;
        uint32_t   iterations;
        seed(a);
        a->ball[0] = 55.0f; a->ball[1] = 27.0f; a->ball[2] = 32.0f; a->ball[3] = 4.0f;
        a->ball_velocity[0] = -200.0f; a->ball_velocity[1] = 50.0f; a->ball_velocity[2] = 0.0f;
        a->sticky = true;
        wall      = params(a, IMPLICIT_STEP);
        a->sticky = false;
        snow_p2g(&wall, smashed, a->count, a->sums);
        snow_velocities(&wall, a->sums, a->mass, a->before, a->after, a->held);
        memcpy(plus, a->after, vector);
        iterations = snow_implicit(&wall, smashed, a->count, 1.0, a->mass, a->held, a->after, &residual);
        snow_force_change(&wall, smashed, a->count, a->after, df);
        for (n = 0u; n < (size_t)SNOW_NODES * 3u; ++n) {
            const double m = a->mass[n / 3u], left = m * a->after[n] - wall.dt * wall.dt * df[n] - m * plus[n];
            if (m <= 0.0) continue;
            if (a->held[n / 3u]) { moved = fmax(moved, fabs(a->after[n] - plus[n])); holds += 1.0 / 3.0; continue; }
            check += left * left / m;
            size  += m * plus[n] * plus[n];
        }
        demo_check(d, "implicit step, 0.5 ms, snow packed on the wall: iterations of conjugate residuals", iterations, "<= 60", iterations <= 60u);
        demo_check(d, "  |r| / |b| it reports", residual, "< 1e-5", residual < 1e-5);
        demo_check(d, "  |(m v - dt^2 df(v) - m v*) / sqrt(m)| / |sqrt(m) v*| over the free nodes, done again", sqrt(check / size), "< 1e-5", sqrt(check / size) < 1e-5);
        demo_check(d, "  nodes held by the wall or the ball", holds, "> 100", holds > 100.0);
        demo_check(d, "  largest change of a held node's velocity in the solve", moved, "== 0", moved == 0.0);
    }

    /* A ball squeezed by 1.5 % all round and let go, with a time step 15 times the shaders'. No gravity,
     * nothing near a wall, and no yielding, so that it is elasticity alone: the squeeze can only turn
     * into motion and back. */
    {
        SnowParams big;
        double     speed[2] = {0.0, 0.0};
        uint32_t   mode, step;
        seed(a);
        big          = params(a, 15.0 * TIME_STEP);
        big.gravity  = 0.0;
        big.compress = big.stretch = 0.9;
        for (mode = 0u; mode < 2u; ++mode) {
            seed(a);
            for (i = 0u; i < a->count; ++i) {
                a->seed[i].vel[0] = a->seed[i].vel[1] = a->seed[i].vel[2] = 0.0f;
                a->seed[i].f[0][0] = a->seed[i].f[1][1] = a->seed[i].f[2][2] = 0.985f;
            }
            for (step = 0u; step < (mode == 0u ? 60u : 12u) && speed[mode] < 1e5; ++step) {
                cpu_step(a, a->seed, a->count, &big, mode == 0u ? 0.0 : 1.0, NULL);
                speed[mode] = 0.0;
                for (i = 0u; i < a->count; ++i) {
                    const SnowParticle *q = &a->seed[i];
                    const double v = sqrt((double)q->vel[0] * q->vel[0] + (double)q->vel[1] * q->vel[1] + (double)q->vel[2] * q->vel[2]);
                    speed[mode] = v == v ? fmax(speed[mode], v) : 1e30;
                }
            }
        }
        demo_check(d, "a squeezed elastic ball, 12 steps of 1.5 ms: fastest particle, cells / s, implicit", speed[1], "< 40", speed[1] < 40.0);
        demo_check(d, "  explicit, the same step, up to 60 steps (it blows up)", speed[0], "> 1e5", speed[0] > 1e5);
    }

    /* 7. The same scene twice. */
    {
        double differ = 0.0;
        uint32_t round;
        a->scene = 1u;
        for (round = 0u; round < 2u; ++round) {
            seed(a);
            for (f = 0u; f < 12u; ++f) app_frame(a);
            demo_readback_buffer(d, a->particles, round == 0u ? before : after, bytes);
        }
        for (n = 0u; n < bytes; ++n)
            if (((const unsigned char *)before)[n] != ((const unsigned char *)after)[n]) differ += 1.0;
        demo_check(d, "120 steps of two snowballs meeting, run twice: bytes of the particles that differ", differ, "== 0", differ == 0.0);
    }
    /* 8. The collision rule by itself (section 8 of the paper), at 20 000 random places inside a moving
     * ball: nothing still moves into the ball as the ball sees it, and what slides along it has lost
     * friction x (the speed into it), or all of it. Then the sticky walls. */
    {
        SnowParams rule;
        double     into = 0.0, slide = 0.0, left = 0.0;
        seed(a);
        a->ball[0] = 30.0f; a->ball[1] = 30.0f; a->ball[2] = 30.0f; a->ball[3] = 6.0f;
        a->ball_velocity[0] = 120.0f; a->ball_velocity[1] = -40.0f; a->ball_velocity[2] = 70.0f;
        a->sticky = true;
        rule      = params(a, TIME_STEP);
        a->sticky = false;
        for (i = 0u; i < 20000u; ++i) {
            double x[3], v[3], was[3], normal[3], along[2] = {0.0, 0.0}, vn[2] = {0.0, 0.0}, len = 0.0;
            for (k = 0u; k < 3u; ++k) {
                x[k]   = rule.ball[k] + 3.4 * noise(&rng);
                was[k] = v[k] = 400.0 * noise(&rng);
                len   += (x[k] - rule.ball[k]) * (x[k] - rule.ball[k]);
            }
            snow_collide(&rule, x, v);
            for (k = 0u; k < 3u; ++k) {
                normal[k] = (x[k] - rule.ball[k]) / sqrt(len);
                vn[0]    += (was[k] - rule.ball_velocity[k]) * normal[k];
                vn[1]    += (v[k] - rule.ball_velocity[k]) * normal[k];
            }
            for (k = 0u; k < 3u; ++k) {
                const double t0 = was[k] - rule.ball_velocity[k] - vn[0] * normal[k], t1 = v[k] - rule.ball_velocity[k] - vn[1] * normal[k];
                along[0] += t0 * t0;
                along[1] += t1 * t1;
            }
            into = fmax(into, -vn[1]);
            if (vn[0] < 0.0) slide = fmax(slide, fabs(sqrt(along[1]) - fmax(sqrt(along[0]) + rule.friction * vn[0], 0.0)));
            /* the same velocity at a place inside a side wall or the ceiling */
            x[i % 3u] = i % 2u ? 1.0 + 1.9 * fabs(noise(&rng)) : (double)N - 2.0 - 1.9 * fabs(noise(&rng));
            if (i % 3u == 1u && i % 2u) continue;   /* the floor is not sticky */
            memcpy(v, was, sizeof v);
            snow_collide(&rule, x, v);
            left = fmax(left, fabs(v[0]) + fabs(v[1]) + fabs(v[2]));
        }
        demo_check(d, "collision with a moving ball, 20 000 random velocities: fastest still going into it, cells / s", into, "< 1e-9", into < 1e-9);
        demo_check(d, "  speed along it - max(before - friction x speed into it, 0), cells / s", slide, "< 1e-9", slide < 1e-9);
        demo_check(d, "sticky walls and ceiling: largest speed left", left, "== 0", left == 0.0);
    }

    /* 9. The ball of scene 3 pushed 4.5 cells into the snow: 150 steps of the shaders. The rule acts on
     * velocities only, so a particle can end a little inside the ball; how far is measured. */
    {
        double deepest = 0.0, pushed = 0.0;
        a->scene = 3u;
        seed(a);
        a->ball[0] = 15.0f;   /* it starts touching the snow */
        for (f = 0u; f < 15u; ++f) app_frame(a);
        demo_readback_buffer(d, a->particles, after, bytes);
        for (i = 0u; i < a->count; ++i) {
            const SnowParticle *q = &after[i];
            const double dx = q->pos[0] - a->ball[0], dy = q->pos[1] - a->ball[1], dz = q->pos[2] - a->ball[2];
            const double away = sqrt(dx * dx + dy * dy + dz * dz);
            deepest = fmax(deepest, a->ball[3] - away);
            pushed += q->vel[0] / (double)a->count;
        }
        demo_check(d, "a ball pushed 4.5 cells into resting snow at 300 cells / s: ball centre, cells along x", a->ball[0], "== 19.5", fabs(a->ball[0] - 19.5f) < 1e-3f);
        demo_check(d, "  deepest particle inside the ball, cells", deepest, "< 0.25", deepest < 0.25);
        demo_check(d, "  mean velocity of the snow along the push, cells / s", pushed, "> 1", pushed > 1.0);
        a->scene = 0u;
    }
    free(before); free(after); free(smashed); free(grid); free(move); free(move2); free(plus); free(minus); free(df); free(df2);
}

int main(int argc, char **argv) {
    static App app;
    static const DemoOption options[] = {
        {"--scene", .u32 = &app.scene, .lo = 0, .hi = 3, .help = "0 a snowball thrown at a wall, 1 two snowballs meet, 2 a slab falls on a ball, 3 a ball ploughs through snow"},
        {"--steps", .u32 = &app.steps, .lo = 1, .hi = MAX_STEPS, .help = "time steps (0.1 ms each) per frame"},
        {"--view", .u32 = &app.view, .lo = 0, .hi = 3, .help = "0 snow, 1 speed, 2 packed or torn, 3 squeezed or stretched"},
        {"--implicit", .flag = &app.implicit, .help = "the paper's implicit step, 0.5 ms, on the CPU; two a frame"},
        {"--sticky", .flag = &app.sticky, .help = "side walls and ceiling hold whatever touches them (the paper's sticky collision)"},
        {"--stiffness", .f32 = &app.stiffness, .lo = 50, .hi = 700, .help = "Young's modulus / density of fresh snow, m^2/s^2"},
        {"--hardening", .f32 = &app.hardening, .lo = 0, .hi = 15, .help = "xi: how fast packed snow stiffens"},
        {"--compression", .f32 = &app.compress, .lo = 0.005, .hi = 0.05, .help = "theta_c: squeeze beyond this is permanent"},
        {"--stretch", .f32 = &app.stretch, .lo = 0.001, .hi = 0.02, .help = "theta_s: stretch beyond this is permanent"},
        {"--flip", .f32 = &app.flip, .lo = 0, .hi = 1, .help = "share of FLIP in the particle velocity"},
        {"--friction", .f32 = &app.friction, .lo = 0, .hi = 2, .help = "of the walls and the ball"},
        {"--radius", .f32 = &app.size, .lo = 0.2, .hi = 1.0, .help = "disc radius in cells"},
        {0}};
    demo_defaults(&app.demo, "snow", "Snow - the material point method of Stomakhin et al. 2013");
    app.steps     = 10u;
    /* The paper's reference snow: E = 1.4e5 Pa, density 400 kg/m^3, xi 10, theta_c 0.025, theta_s 0.0075. */
    app.stiffness = 350.0f;
    app.hardening = 10.0f;
    app.compress  = 0.025f;
    app.stretch   = 0.0075f;
    app.flip      = 0.95f;
    app.friction  = 0.6f;
    app.size      = 0.55f;
    app.orbit = (DemoOrbit){.target = {36.0f, 21.0f, 32.0f}, .distance = 68.0f, .yaw = -0.4f, .pitch = 0.3f,
                            .fov_deg = 40.0f, .znear = 1.0f, .zfar = 400.0f, .min_distance = 20.0f, .max_distance = 200.0f};
    app.demo.note = "Headless: 40 frames (0.04 s of the scene) unless --frames says otherwise.";
    demo_open(&app.demo, options, argc, argv);
    app_init(&app);
    while (demo_running(&app.demo)) {
        if (app.demo.ui) app_ui(&app, app.demo.ui);
        app_frame(&app);
    }
    if (app.demo.check) app_check(&app);
    free(app.seed); free(app.sums); free(app.mass); free(app.before); free(app.after); free(app.held);
    return demo_close(&app.demo);
}

