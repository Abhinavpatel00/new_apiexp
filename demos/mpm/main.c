/* mpm — water and jelly: the material point method on the GPU.
 *
 *   (Clear -> P2G -> G2P) x K -> Draw -> Present
 *
 * 16 384 particles and a 128 x 128 grid; see shaders/mpm.slang for the
 * method. A block of water is let go next to two bricks of jelly.
 *
 * --check redoes one time step on the CPU, adds up mass and momentum on the
 * grid, drops a brick through empty space against the free-fall formula, and
 * runs the same scene twice.
 */
#include "demo.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SHADERS    "demos/mpm/shaders/"
#define N          128u
#define MAX_COUNT  16384u
#define MAX_STEPS  18u          /* three passes each; a frame program holds 64 */
#define WALLS      3u
#define TIME_STEP  2.0e-4f
#define GRAVITY    (9.8f * N)   /* the box is one metre across */
#define BULK       (400.0f * N * N)
#define MASS_SCALE     16777216.0f
#define MOMENTUM_SCALE 16384.0f

typedef struct Particle {
    float    pos[2], vel[2], c[4], f[4];
    uint32_t material, pad[3];   /* 0 water, 1 and 2 jelly */
} Particle;
_Static_assert(sizeof(Particle) == 64, "Particle must match mpm.slang");

typedef struct Push {
    uint32_t particles, grid, count, n;
    float    dt, gravity, bulk, mu;
    float    lambda;
    uint32_t walls, pad[2];
} Push;
_Static_assert(sizeof(Push) == 48, "Push must match mpm.slang");

typedef struct DrawPush {
    uint32_t particles, count, n, view;
    float    aspect, size, walls, pad;
} DrawPush;

static const char *const k_views[]  = {"material", "speed", "squeezed or stretched"};
static const char *const k_scenes[] = {"water and two bricks", "one brick falling"};

typedef struct App {
    Demo      demo;
    BufSlice  particles, grid;
    BufUse    use_particles_read, use_particles_write, use_grid_read, use_grid_write, use_draw;
    Particle *seed;
    uint32_t  count;
    /* options */
    uint32_t  scene, steps, view;
    float     stiffness;         /* of the jelly: Young's modulus / density, m^2/s^2 */
    float     size;
    bool      restart;
    uint32_t  run;               /* time steps this frame runs, at most `steps`; --check changes it */
} App;

/* The starting state: blocks of particles, four to a cell. */
static void seed(App *a) {
    /* first particle, particles across, origin (cells), material, jitter */
    static const struct { uint32_t first, across; float x, y; uint32_t material; float jitter; } k_blocks[2][3] = {
        {{0u, 96u, 3.0f, 3.0f, 0u, 0.3f}, {12288u, 32u, 76.0f, 30.0f, 1u, 0.0f}, {14336u, 64u, 30.0f, 92.0f, 2u, 0.0f}},
        {{0u, 32u, 56.0f, 64.0f, 1u, 0.0f}, {2048u, 0u, 0.0f, 0.0f, 0u, 0.0f}, {2048u, 0u, 0.0f, 0.0f, 0u, 0.0f}}};
    uint32_t i, b;
    a->count = a->scene == 0u ? MAX_COUNT : 2048u;
    memset(a->seed, 0, (size_t)MAX_COUNT * sizeof(Particle));
    for (i = 0u; i < a->count; ++i) {
        Particle *p = &a->seed[i];
        uint32_t  h = i * 747796405u + 2891336453u, local;
        for (b = 0u; b < 2u && i >= k_blocks[a->scene][b + 1u].first; ++b) {}
        local = i - k_blocks[a->scene][b].first;
        h = ((h >> ((h >> 28u) + 4u)) ^ h) * 277803737u;
        p->pos[0] = k_blocks[a->scene][b].x + 0.5f * ((float)(local % k_blocks[a->scene][b].across) + 0.5f +
                                                      k_blocks[a->scene][b].jitter * ((float)(h & 0xffffu) / 65535.0f - 0.5f));
        p->pos[1] = k_blocks[a->scene][b].y + 0.5f * ((float)(local / k_blocks[a->scene][b].across) + 0.5f +
                                                      k_blocks[a->scene][b].jitter * ((float)(h >> 16u) / 65535.0f - 0.5f));
        p->f[0] = p->f[3] = 1.0f;
        p->material = k_blocks[a->scene][b].material;
    }
    demo_upload(&a->demo, a->particles, a->seed, (uint64_t)MAX_COUNT * sizeof(Particle));
}

static void app_frame(App *a) {
    Demo    *d = &a->demo;
    PassCtx *pass;
    uint32_t i;
    Push     push = {.particles = (uint32_t)a->particles.offset, .grid = (uint32_t)a->grid.offset, .count = a->count, .n = N,
                     .dt = TIME_STEP, .gravity = GRAVITY, .bulk = BULK,
                     /* Poisson's ratio 0.2 */
                     .mu = a->stiffness * N * N / 2.4f, .lambda = a->stiffness * N * N * 0.2f / (1.2f * 0.6f), .walls = WALLS};
    DrawPush draw = {.particles = (uint32_t)a->particles.offset, .count = a->count, .n = N, .view = a->view,
                     .aspect = (float)d->width / (float)d->height, .size = a->size, .walls = (float)WALLS - 0.5f};
    d->present.tonemap = DEMO_TONEMAP_NONE;

    if (a->restart) { seed(a); a->restart = false; }
    if (!demo_frame_begin(d)) return;
    for (i = 0u; i < 3u * a->steps; ++i) {
        pass = demo_pass(d, NULL);   /* "Clear" / "P2G" / "G2P" */
        if (i / 3u >= a->run) continue;
        pass_push(pass, &push, sizeof push);
        pass_dispatch(pass, (i % 3u == 0u ? N * N : a->count) / 64u, 1u, 1u);
    }
    pass = demo_pass(d, "Draw");
    pass_push(pass, &draw, sizeof draw);
    pass_draw(pass, (4u + a->count) * 6u, 1u, 0u, 0u);
    demo_present(d);
    demo_frame_end(d);
}

static void app_init(App *a) {
    static const char *const k_names[3] = {"Clear", "P2G", "G2P"};
    Demo    *d = &a->demo;
    PassDesc passes[3u * MAX_STEPS + 3u];
    uint32_t count = 0u, i;
    Target   hdr = demo_target(d, "mpm_hdr", FMT_RGBA16F, d->width, d->height, false, NULL);
    Pipe     pipes[3];

    a->seed      = (Particle *)malloc((size_t)MAX_COUNT * sizeof(Particle));
    a->particles = demo_alloc(d, NULL, (uint64_t)MAX_COUNT * sizeof(Particle));
    a->grid      = demo_alloc(d, NULL, (uint64_t)N * N * 16u);
    seed(a);

    a->use_particles_read  = use_read(a->particles.buffer, a->particles.offset, a->particles.size);
    a->use_particles_write = use_write(a->particles.buffer, a->particles.offset, a->particles.size);
    a->use_grid_read       = use_read(a->grid.buffer, a->grid.offset, a->grid.size);
    a->use_grid_write      = use_write(a->grid.buffer, a->grid.offset, a->grid.size);
    a->use_draw            = a->use_particles_read;
    a->use_draw.stages     = SH_VERT;

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
                                     demo_shader(d, SHADERS "draw.frag.spv", STAGE_FRAGMENT), FMT_RGBA16F, FMT_NONE, BLEND_OPAQUE),
        .colors = (AttachDesc[]){attach_clear(hdr, 0.012f, 0.014f, 0.02f, 1.0f)}, .color_count = 1u,
        .buf_reads = &a->use_draw, .buf_read_count = 1u};
    passes[count++] = demo_present_pass(d, hdr);
    d->program = demo_program(d, passes, count);

    a->run = a->steps;
    if (d->headless && !d->frames) d->frames = 200u;
}

static void app_ui(App *a, struct nk_context *ctx) {
    Demo *d = &a->demo;
    if (nk_begin(ctx, "Material point method", nk_rect(10, 10, 300, 280),
                 NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_TITLE)) {
        demo_ui_text(ctx, "%.1f ms   %u particles", (double)d->dt * 1000.0, a->count);
        demo_ui_text(ctx, "%.1f ms of the scene a frame", (double)(a->steps * TIME_STEP) * 1000.0);
        if (demo_ui_choice(ctx, "Scene", k_scenes, 2u, &a->scene)) a->restart = true;
        demo_ui_bool(ctx, "Start again", &a->restart);
        demo_ui_float(ctx, "Jelly stiffness", &a->stiffness, 50.0f, 600.0f);
        demo_ui_choice(ctx, "View", k_views, 3u, &a->view);
        demo_ui_float(ctx, "Disc radius (cells)", &a->size, 0.2f, 1.0f);
    }
    nk_end(ctx);
}

/* ---- acceptance ------------------------------------------------------------ */

static void app_check(App *a) {
    Demo        *d = &a->demo;
    const size_t bytes = (size_t)MAX_COUNT * sizeof(Particle), nodes = (size_t)N * N;
    Particle    *before = (Particle *)malloc(bytes), *after = (Particle *)malloc(bytes);
    int32_t     *grid = (int32_t *)malloc(nodes * 16u), *sums = (int32_t *)calloc(nodes * 4u, sizeof(int32_t));
    const float  mu = a->stiffness * N * N / 2.4f, lambda = a->stiffness * N * N * 0.2f / (1.2f * 0.6f);
    uint32_t     i, f, gx, gy;
    size_t       n;

    /* 1. The scene the run left behind. */
    {
        double low[2] = {1e9, 1e9}, high[2] = {-1e9, -1e9}, bad = 0.0, deepest = -1e9, fastest = 0.0;
        uint32_t axis;
        demo_readback_buffer(d, a->particles, before, bytes);
        for (i = 0u; i < a->count; ++i) {
            const Particle *p = &before[i];
            const double j = (double)p->f[0] * p->f[3] - (double)p->f[1] * p->f[2], speed = hypot(p->vel[0], p->vel[1]);
            const uint32_t kind = p->material ? 1u : 0u;
            if (!(p->pos[0] == p->pos[0] && p->pos[1] == p->pos[1] && speed == speed && j == j)) { bad += 1.0; continue; }
            /* The wall nodes are 0 .. WALLS - 1 and N - WALLS .. N - 1. */
            for (axis = 0u; axis < 2u; ++axis) {
                if (WALLS - 1.0 - p->pos[axis] > deepest) deepest = WALLS - 1.0 - p->pos[axis];
                if (p->pos[axis] - (N - WALLS) > deepest) deepest = p->pos[axis] - (N - WALLS);
            }
            if (j < low[kind]) low[kind] = j;
            if (j > high[kind]) high[kind] = j;
            if (speed > fastest) fastest = speed;
        }
        demo_check(d, "after the run: particles that are not numbers", bad, "== 0", bad == 0.0);
        demo_check(d, "  deepest particle, cells past the first wall node", deepest, "< 1", deepest < 1.0);
        demo_check(d, "  fastest particle, cells per time step", fastest * TIME_STEP, "< 0.5", fastest * TIME_STEP < 0.5);
        demo_check(d, "  jelly, area now / area at the start: lowest", low[1], "> 0.5", low[1] > 0.5);
        demo_check(d, "  jelly, highest", high[1], "< 1.5", high[1] < 1.5);
        demo_check(d, "  water, lowest (reported)", low[0], "", true);
        demo_check(d, "  water, highest (reported)", high[0], "", true);
    }

    /* 2. One more time step, on the GPU and here. The grid is integers, so the
     * two can only differ where a float rounded the other way. */
    {
        double mass = 0.0, momentum[2] = {0.0, 0.0}, carried[2] = {0.0, 0.0};
        double worst_pos = 0.0, worst_vel = 0.0, worst_node[3] = {0.0, 0.0, 0.0}, differ = 0.0;
        a->run = 1u;
        app_frame(a);
        demo_readback_buffer(d, a->particles, after, bytes);
        demo_readback_buffer(d, a->grid, grid, nodes * 16u);
        for (i = 0u; i < a->count; ++i) {
            const Particle *p = &before[i];
            const int   bx = (int)floorf(p->pos[0] - 0.5f), by = (int)floorf(p->pos[1] - 0.5f);
            const float fx[2] = {p->pos[0] - (float)bx, p->pos[1] - (float)by};
            const float j = p->f[0] * p->f[3] - p->f[1] * p->f[2];
            float w[3][2], stress[4], affine[4];
            uint32_t axis;
            for (axis = 0u; axis < 2u; ++axis) {
                w[0][axis] = 0.5f * (1.5f - fx[axis]) * (1.5f - fx[axis]);
                w[1][axis] = 0.75f - (fx[axis] - 1.0f) * (fx[axis] - 1.0f);
                w[2][axis] = 0.5f * (fx[axis] - 0.5f) * (fx[axis] - 0.5f);
            }
            if (p->material == 0u) {
                stress[0] = stress[3] = BULK * (j - 1.0f);
                stress[1] = stress[2] = 0.0f;
            } else {
                const float spring = lambda * logf(j > 0.1f ? j : 0.1f);
                stress[0] = mu * (p->f[0] * p->f[0] + p->f[1] * p->f[1] - 1.0f) + spring;
                stress[1] = stress[2] = mu * (p->f[0] * p->f[2] + p->f[1] * p->f[3]);
                stress[3] = mu * (p->f[2] * p->f[2] + p->f[3] * p->f[3] - 1.0f) + spring;
            }
            for (axis = 0u; axis < 4u; ++axis) affine[axis] = p->c[axis] - 4.0f * TIME_STEP * stress[axis];
            for (gy = 0u; gy < 3u; ++gy)
                for (gx = 0u; gx < 3u; ++gx) {
                    const float away[2] = {(float)gx - fx[0], (float)gy - fx[1]}, weight = w[gx][0] * w[gy][1];
                    int32_t *node = &sums[((size_t)(by + (int)gy) * N + (size_t)(bx + (int)gx)) * 4u];
                    node[0] += (int32_t)floorf(weight * MASS_SCALE + 0.5f);
                    node[1] += (int32_t)floorf(weight * (p->vel[0] + affine[0] * away[0] + affine[1] * away[1]) * MOMENTUM_SCALE + 0.5f);
                    node[2] += (int32_t)floorf(weight * (p->vel[1] + affine[2] * away[0] + affine[3] * away[1]) * MOMENTUM_SCALE + 0.5f);
                }
            carried[0] += p->vel[0];
            carried[1] += p->vel[1];
        }
        for (n = 0u; n < nodes; ++n) {
            uint32_t c;
            for (c = 0u; c < 3u; ++c) {
                const double off = fabs((double)grid[n * 4u + c] - (double)sums[n * 4u + c]);
                if (off > 0.0) differ += 1.0;
                if (off > worst_node[c]) worst_node[c] = off;
            }
            mass += grid[n * 4u] / (double)MASS_SCALE;
            momentum[0] += grid[n * 4u + 1u] / (double)MOMENTUM_SCALE;
            momentum[1] += grid[n * 4u + 2u] / (double)MOMENTUM_SCALE;
        }
        /* Grid to particles, from the grid the GPU made. */
        for (i = 0u; i < a->count; ++i) {
            const Particle *p = &before[i], *q = &after[i];
            const int   bx = (int)floorf(p->pos[0] - 0.5f), by = (int)floorf(p->pos[1] - 0.5f);
            const float fx[2] = {p->pos[0] - (float)bx, p->pos[1] - (float)by};
            float w[3][2], vel[2] = {0.0f, 0.0f}, pos[2];
            uint32_t axis;
            for (axis = 0u; axis < 2u; ++axis) {
                w[0][axis] = 0.5f * (1.5f - fx[axis]) * (1.5f - fx[axis]);
                w[1][axis] = 0.75f - (fx[axis] - 1.0f) * (fx[axis] - 1.0f);
                w[2][axis] = 0.5f * (fx[axis] - 0.5f) * (fx[axis] - 0.5f);
            }
            for (gy = 0u; gy < 3u; ++gy)
                for (gx = 0u; gx < 3u; ++gx) {
                    const int      x = bx + (int)gx, y = by + (int)gy;
                    const int32_t *node = &grid[((size_t)y * N + (size_t)x) * 4u];
                    float v[2] = {0.0f, 0.0f};
                    if (node[0] > 0) {
                        v[0] = (float)node[1] / MOMENTUM_SCALE / ((float)node[0] / MASS_SCALE);
                        v[1] = (float)node[2] / MOMENTUM_SCALE / ((float)node[0] / MASS_SCALE) - TIME_STEP * GRAVITY;
                        if (x < (int)WALLS && v[0] < 0.0f) v[0] = 0.0f;
                        if (x > (int)(N - 1u - WALLS) && v[0] > 0.0f) v[0] = 0.0f;
                        if (y < (int)WALLS && v[1] < 0.0f) v[1] = 0.0f;
                        if (y > (int)(N - 1u - WALLS) && v[1] > 0.0f) v[1] = 0.0f;
                    }
                    vel[0] += w[gx][0] * w[gy][1] * v[0];
                    vel[1] += w[gx][0] * w[gy][1] * v[1];
                }
            for (axis = 0u; axis < 2u; ++axis) {
                pos[axis] = p->pos[axis] + TIME_STEP * vel[axis];
                pos[axis] = pos[axis] < 1.0f ? 1.0f : pos[axis] > N - 2.0f ? N - 2.0f : pos[axis];
                if (fabs((double)pos[axis] - q->pos[axis]) > worst_pos) worst_pos = fabs((double)pos[axis] - q->pos[axis]);
                if (fabs((double)vel[axis] - q->vel[axis]) > worst_vel) worst_vel = fabs((double)vel[axis] - q->vel[axis]);
            }
        }
        if (worst_node[2] > worst_node[1]) worst_node[1] = worst_node[2];
        demo_check(d, "one step, grid against the CPU: sums that differ (of 49152, reported)", differ, "", true);
        demo_check(d, "  largest difference in a node's mass (particles)", worst_node[0] / MASS_SCALE, "< 1e-5", worst_node[0] / MASS_SCALE < 1e-5);
        demo_check(d, "  in a node's momentum (particle cells/s)", worst_node[1] / MOMENTUM_SCALE, "< 1e-3", worst_node[1] / MOMENTUM_SCALE < 1e-3);
        demo_check(d, "  mass on the grid / particles - 1", mass / a->count - 1.0, "|x| < 1e-5", fabs(mass / a->count - 1.0) < 1e-5);
        demo_check(d, "  momentum on the grid - momentum of the particles, per particle (cells/s): x",
                   (momentum[0] - carried[0]) / a->count, "|x| < 1e-3", fabs(momentum[0] - carried[0]) / a->count < 1e-3);
        demo_check(d, "  y", (momentum[1] - carried[1]) / a->count, "|x| < 1e-3", fabs(momentum[1] - carried[1]) / a->count < 1e-3);
        demo_check(d, "one step, particles against the CPU: largest difference in velocity (cells/s)", worst_vel, "< 1e-3", worst_vel < 1e-3);
        demo_check(d, "  in position (cells)", worst_pos, "< 1e-5", worst_pos < 1e-5);
    }

    /* 3. A brick in empty space falls as a point would: with this scheme
     * v = -g n dt and y = y0 - g dt^2 n (n + 1) / 2 after n steps, and
     * nothing squeezes it. */
    {
        const uint32_t frames = 20u, steps = frames * a->steps;
        double start[2] = {0.0, 0.0}, end[2] = {0.0, 0.0}, speed[2] = {0.0, 0.0}, strain = 0.0;
        const double fall = (double)GRAVITY * TIME_STEP * TIME_STEP * steps * (steps + 1.0) / 2.0;
        a->scene = 1u;
        seed(a);
        a->run = a->steps;
        for (f = 0u; f < frames; ++f) app_frame(a);
        demo_readback_buffer(d, a->particles, after, bytes);
        for (i = 0u; i < a->count; ++i) {
            const Particle *p = &after[i];
            start[0] += a->seed[i].pos[0] / (double)a->count;
            start[1] += a->seed[i].pos[1] / (double)a->count;
            end[0] += p->pos[0] / (double)a->count;
            end[1] += p->pos[1] / (double)a->count;
            speed[0] += p->vel[0] / (double)a->count;
            speed[1] += p->vel[1] / (double)a->count;
            if (fabs(p->f[0] - 1.0) > strain) strain = fabs(p->f[0] - 1.0);
            if (fabs(p->f[3] - 1.0) > strain) strain = fabs(p->f[3] - 1.0);
            if (fabs(p->f[1]) > strain) strain = fabs(p->f[1]);
            if (fabs(p->f[2]) > strain) strain = fabs(p->f[2]);
        }
        demo_check(d, "a brick falling for 360 steps: distance fallen (cells)", start[1] - end[1], "", true);
        demo_check(d, "  / g dt^2 n (n + 1) / 2 - 1", (start[1] - end[1]) / fall - 1.0, "|x| < 1e-4",
                   fabs((start[1] - end[1]) / fall - 1.0) < 1e-4);
        demo_check(d, "  speed / g n dt - 1", -speed[1] / ((double)GRAVITY * TIME_STEP * steps) - 1.0, "|x| < 1e-4",
                   fabs(-speed[1] / ((double)GRAVITY * TIME_STEP * steps) - 1.0) < 1e-4);
        demo_check(d, "  sideways drift (cells)", end[0] - start[0], "|x| < 1e-4", fabs(end[0] - start[0]) < 1e-4);
        demo_check(d, "  largest change of shape, |F - 1|", strain, "< 1e-3", strain < 1e-3);
    }

    /* 4. The same five frames twice. */
    {
        double differ = 0.0;
        uint32_t round;
        a->scene = 0u;
        for (round = 0u; round < 2u; ++round) {
            seed(a);
            for (f = 0u; f < 5u; ++f) app_frame(a);
            demo_readback_buffer(d, a->particles, round == 0u ? before : after, bytes);
        }
        for (n = 0u; n < bytes; ++n)
            if (((const unsigned char *)before)[n] != ((const unsigned char *)after)[n]) differ += 1.0;
        demo_check(d, "90 steps run twice: bytes of the particles that differ", differ, "== 0", differ == 0.0);
    }
    free(before); free(after); free(grid); free(sums);
}

int main(int argc, char **argv) {
    static App app;
    static const DemoOption options[] = {
        {"--scene", .u32 = &app.scene, .lo = 0, .hi = 1, .help = "0 water and two bricks of jelly, 1 one brick falling"},
        {"--stiffness", .f32 = &app.stiffness, .lo = 50, .hi = 600, .help = "of the jelly: Young's modulus / density, m^2/s^2"},
        {"--steps", .u32 = &app.steps, .lo = 1, .hi = MAX_STEPS, .help = "time steps (0.2 ms each) per frame"},
        {"--view", .u32 = &app.view, .lo = 0, .hi = 2, .help = "0 material, 1 speed, 2 squeezed or stretched"},
        {"--radius", .f32 = &app.size, .lo = 0.2, .hi = 1.0, .help = "disc radius in cells"},
        {0}};
    demo_defaults(&app.demo, "mpm", "Material point method - water and jelly");
    app.steps     = MAX_STEPS;
    app.stiffness = 300.0f;
    app.size      = 0.45f;
    app.demo.note = "Headless: 200 frames (0.72 s of the scene) unless --frames says otherwise.";
    demo_open(&app.demo, options, argc, argv);
    app_init(&app);
    while (demo_running(&app.demo)) {
        if (app.demo.ui) app_ui(&app, app.demo.ui);
        app_frame(&app);
    }
    if (app.demo.check) app_check(&app);
    free(app.seed);
    return demo_close(&app.demo);
}
