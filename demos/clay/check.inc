/* check.inc — clay's acceptance test; included by main.c. */

static void run_frames(App *a, uint32_t n) {
    uint32_t i;
    for (i = 0u; i < n; ++i) app_frame(a);
}

typedef struct State {
    Particle *particles;
    uint32_t *grid;
    float    *volume;        /* r of every voxel */
} State;

static void state_free(State *s) { free(s->particles); free(s->grid); free(s->volume); memset(s, 0, sizeof *s); }

static void state_read(App *a, State *s, bool with_volume) {
    Demo        *d      = &a->demo;
    const size_t voxels = (size_t)VOXELS * VOXELS * VOXELS, cells = (size_t)CELLS * CELLS * CELLS;
    size_t       i;
    memset(s, 0, sizeof *s);
    s->particles = (Particle *)malloc(COUNT * sizeof(Particle));
    s->grid      = (uint32_t *)malloc(cells * CELL_STRIDE);
    demo_readback_buffer(d, a->particles, s->particles, COUNT * sizeof(Particle));
    demo_readback_buffer(d, a->grid, s->grid, cells * CELL_STRIDE);
    if (!with_volume) return;
    /* the distance is the first of the volume's four channels */
    s->volume = demo_read_floats(d, a->volume, voxels * 4u);
    for (i = 0u; i < voxels; ++i) s->volume[i] = s->volume[i * 4u];
}

static uint32_t cell_of(const float p[3]) {
    int c[3], i;
    static const float origin[3] = {-1.6f, 0.0f, -1.6f};
    for (i = 0; i < 3; ++i) {
        c[i] = (int)floorf((p[i] - origin[i]) / CELL);
        if (c[i] < 0) c[i] = 0;
        if (c[i] > (int)CELLS - 1) c[i] = (int)CELLS - 1;
    }
    return (uint32_t)c[0] + CELLS * ((uint32_t)c[1] + CELLS * (uint32_t)c[2]);
}

/* Grid: every particle is in its own cell's list, exactly once in the whole grid; counts add up. */
static void check_grid(const State *s, uint32_t *missing, uint32_t *overflowed, uint32_t *fullest, uint32_t *listed) {
    const size_t cells = (size_t)CELLS * CELLS * CELLS;
    uint8_t     *seen  = (uint8_t *)calloc(COUNT, 1u);
    size_t       c;
    uint32_t     i;
    *missing = *overflowed = *fullest = *listed = 0u;
    if (!seen) { *missing = COUNT; return; }
    for (c = 0u; c < cells; ++c) {
        const uint32_t *cell  = s->grid + c * (CELL_STRIDE / 4u);
        const uint32_t  count = cell[0];
        if (count > *fullest) *fullest = count;
        if (count > CELL_CAP) *overflowed += count - CELL_CAP;
        for (i = 0u; i < count && i < CELL_CAP; ++i) {
            const uint32_t j = cell[1u + i];
            if (j < COUNT && cell_of(s->particles[j].pos) == c && !seen[j]) { seen[j] = 1u; ++*listed; }
        }
    }
    for (i = 0u; i < COUNT; ++i) *missing += !seen[i];
    free(seen);
}

/* Pairs: the nearest two centres, and how many pairs overlap by more than a tenth of a diameter. */
static void check_pairs(const State *s, double *nearest, uint32_t *deep) {
    uint32_t i, j;
    double   best = 1e30;
    *deep = 0u;
    for (i = 0u; i < COUNT; ++i)
        for (j = i + 1u; j < COUNT; ++j) {
            const double dx = (double)s->particles[i].pos[0] - s->particles[j].pos[0];
            const double dy = (double)s->particles[i].pos[1] - s->particles[j].pos[1];
            const double dz = (double)s->particles[i].pos[2] - s->particles[j].pos[2];
            const double l  = sqrt(dx * dx + dy * dy + dz * dz);
            if (l < best) best = l;
            if (l < 0.9 * 2.0 * RADIUS) ++*deep;
        }
    *nearest = best;
}

static void app_check(App *a) {
    Demo        *d      = &a->demo;
    const size_t voxels = (size_t)VOXELS * VOXELS * VOXELS;
    State        s;
    uint32_t     missing, overflowed, fullest, listed, deep, i;
    double       nearest;

    /* 1. The block falls and comes to rest: 6 s, no paddle, plain minimum in the bake. */
    a->paddle = false; a->smooth = 0.0f; a->view = 0u;
    run_frames(a, 360u);
    state_read(a, &s, true);

    check_grid(&s, &missing, &overflowed, &fullest, &listed);
    demo_check(d, "after 6 s: particles in their cell's list, once", (double)listed, "== 4096", listed == COUNT);
    demo_check(d, "after 6 s: particles lost to full cells", (double)overflowed, "== 0", overflowed == 0u);
    demo_check(d, "after 6 s: most particles in a cell", (double)fullest, "<= 8 (reported)", fullest <= CELL_CAP);
    check_pairs(&s, &nearest, &deep);
    demo_check(d, "after 6 s: nearest pair, in diameters", nearest / (2.0 * RADIUS), "> 0.85", nearest > 0.85 * 2.0 * RADIUS);
    demo_check(d, "after 6 s: pairs overlapping by more than a tenth", (double)deep, "== 0", deep == 0u);
    {
        double   speed = 0.0, fastest = 0.0, top = 0.0, outside = 0.0;
        uint32_t out = 0u;
        for (i = 0u; i < COUNT; ++i) {
            const float *p = s.particles[i].pos, *v = s.particles[i].vel;
            const double l = sqrt((double)v[0] * v[0] + (double)v[1] * v[1] + (double)v[2] * v[2]);
            const double e = fmax(fmax(fabs(p[0]), fabs(p[2])) - (HALF - RADIUS), (double)RADIUS - p[1]);
            speed += l;
            if (l > fastest) fastest = l;
            if (p[1] > top) top = p[1];
            if (e > 1e-5) { ++out; if (e > outside) outside = e; }
        }
        demo_check(d, "after 6 s: particles through a wall or the floor", (double)out, "== 0", out == 0u);
        demo_check(d, "after 6 s: mean speed, m/s", speed / COUNT, "< 0.1", speed / COUNT < 0.1);
        demo_check(d, "after 6 s: fastest particle, m/s (reported)", fastest, "", true);
        demo_check(d, "after 6 s: highest centre, m (reported)", top, "", true);
    }

    /* 2. The volume against every particle, the slow way. */
    {
        double   worst = 0.0;
        size_t   v, inside = 0u, band = 0u;
        for (v = 0u; v < voxels; ++v) {
            const float x = -1.6f + ((float)(v % VOXELS) + 0.5f) * VOXEL;
            const float y = ((float)(v / VOXELS % VOXELS) + 0.5f) * VOXEL;
            const float z = -1.6f + ((float)(v / (VOXELS * VOXELS)) + 0.5f) * VOXEL;
            float       best = REACH * REACH;
            double      e;
            for (i = 0u; i < COUNT; ++i) {
                const float dx = x - s.particles[i].pos[0], dy = y - s.particles[i].pos[1], dz = z - s.particles[i].pos[2];
                const float l2 = dx * dx + dy * dy + dz * dz;
                if (l2 < best) best = l2;
            }
            e = fabs((double)(sqrtf(best) - RADIUS) - (double)s.volume[v]);
            if (e > worst) worst = e;
            if (s.volume[v] < 0.0f) ++inside;
            if (s.volume[v] < REACH - RADIUS - 1e-4f) ++band;
        }
        demo_check(d, "volume: voxels within reach of a particle", (double)band, "> 10000", band > 10000u);
        demo_check(d, "volume vs every particle on the CPU, all 262 144 voxels, largest difference (m)", worst, "< 0.0005",
                         worst < 0.0005);
        demo_check(d, "volume: particles' volume / volume of the voxels inside them (reported)",
                         (double)COUNT * 4.18879 * RADIUS * RADIUS * RADIUS / ((double)inside * VOXEL * VOXEL * VOXEL), "", true);
    }
    state_free(&s);

    /* 3. The paddle ploughs through for 4 s. */
    a->paddle = true;
    {
        uint32_t frame, inside_paddle = 0u, out = 0u, lost = 0u, most = 0u, unlisted = 0u;
        double   deepest = 0.0, nearest_ever = 1e30;
        for (frame = 0u; frame < 240u; frame += 20u) {
            float c[3];
            run_frames(a, 20u);
            state_read(a, &s, false);
            paddle_at(a->steps, c);
            for (i = 0u; i < COUNT; ++i) {
                const float *p = s.particles[i].pos;
                const double l = sqrt(((double)p[0] - c[0]) * (p[0] - c[0]) + ((double)p[1] - c[1]) * (p[1] - c[1]) +
                                      ((double)p[2] - c[2]) * (p[2] - c[2]));
                const double e = fmax(fmax(fabs(p[0]), fabs(p[2])) - (HALF - RADIUS), (double)RADIUS - p[1]);
                if (PADDLE_RADIUS + RADIUS - l > 1e-4) { ++inside_paddle; if (PADDLE_RADIUS + RADIUS - l > deepest) deepest = PADDLE_RADIUS + RADIUS - l; }
                if (e > 1e-5) ++out;
            }
            check_grid(&s, &missing, &overflowed, &fullest, &listed);
            lost += overflowed; unlisted += COUNT - listed;
            if (fullest > most) most = fullest;
            check_pairs(&s, &nearest, &deep);
            if (nearest < nearest_ever) nearest_ever = nearest;
            state_free(&s);
        }
        demo_check(d, "paddle, 12 samples: particles inside the paddle", (double)inside_paddle, "== 0", inside_paddle == 0u);
        demo_check(d, "paddle: deepest (m, reported)", deepest, "", true);
        demo_check(d, "paddle: particles through a wall or the floor", (double)out, "== 0", out == 0u);
        demo_check(d, "paddle: particles not in their cell's list", (double)unlisted, "== 0", unlisted == 0u);
        demo_check(d, "paddle: particles lost to full cells", (double)lost, "== 0", lost == 0u);
        demo_check(d, "paddle: most particles in a cell (reported)", (double)most, "", true);
        demo_check(d, "paddle: nearest pair ever, in diameters (reported)", nearest_ever / (2.0 * RADIUS), "", true);
    }

    /* 4. The same two seconds twice. */
    {
        State    first, second;
        uint32_t differ = 0u;
        reset(a);
        run_frames(a, 120u);
        state_read(a, &first, false);
        reset(a);
        run_frames(a, 120u);
        state_read(a, &second, false);
        for (i = 0u; i < COUNT; ++i) differ += memcmp(&first.particles[i], &second.particles[i], sizeof(Particle)) != 0;
        demo_check(d, "same 2 s twice: particles whose state differs in any bit", (double)differ, "== 0", differ == 0u);
        state_free(&first); state_free(&second);
    }

    /* 5. The picture: what each ray hit. */
    {
        const size_t count = (size_t)d->width * d->height * 4u;
        uint16_t    *half  = (uint16_t *)malloc(count * 2u);
        size_t       px, clay = 0u, paddle = 0u, missed = 0u;
        a->view = 3u;
        run_frames(a, 1u);
        demo_readback(d, a->hdr, half, count * 2u);
        a->view = 0u;
        for (px = 0u; px < count / 4u; ++px) {
            const float what = demo_half_to_float(half[px * 4u]);
            clay += what == 3.0f; paddle += what == 2.0f; missed += what < 0.0f;
        }
        free(half);
        demo_check(d, "picture: rays ending on clay (share)", (double)clay / (double)(count / 4u), "> 0.1", clay * 10u > count / 4u);
        demo_check(d, "picture: rays ending on the paddle", (double)paddle, "> 0", paddle > 0u);
        demo_check(d, "picture: rays that hit nothing (sky or out of steps; share, reported)", (double)missed / (double)(count / 4u), "", true);
    }
}
