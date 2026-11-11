/* check.inc — sdfworld's acceptance test (included by main.c).
 *
 * Runs after the headless frames, so the world it tests has been dug, built
 * on and hit for 240 frames. It then stops everything and asks:
 *   1. does a brush change only its box, and does it change it the way the
 *      formula says;
 *   2. is the empty-space level what a CPU computes from the same volume;
 *   3. is the field safe to step along (never steeper than 1);
 *   4. do the coarse level and the pre-pass change what is hit, and how many
 *      steps do they save;
 *   5. does the pointer's brush land where the picture shows the surface;
 *   6. does a falling stone dig where it hit, through a dispatch whose size
 *      the GPU decided.
 */





/* Where a voxel is in a read-back volume: slice after slice. */
static size_t atlas_index(uint32_t x, uint32_t y, uint32_t z) { return (((size_t)z * VY + y) * VX + x) * 4u; }
static size_t coarse_index(uint32_t x, uint32_t y, uint32_t z) { return (((size_t)z * CY + y) * CX + x) * 4u; }

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
/* mu_sd_smin / mu_sd_smax, as in mu_lib's sdf.slang */
static float smin_poly(float a, float b, float k) {
    const float h = clampf(0.5f + 0.5f * (b - a) / k, 0.0f, 1.0f);
    return b + (a - b) * h - k * h * (1.0f - h);
}

/* Compare two states of the volume around one dig of `radius`, `k` at
 * `c`: voxels changed outside the brush's box, voxels changed inside, and
 * the largest difference between the new distance and the formula applied
 * to the old one. */
typedef struct DigResult { size_t outside_changed, inside_changed, inside, color_changed; double worst; } DigResult;

static DigResult verify_dig(const float *before, const float *after, const float *c, float radius, float k) {
    const int half = (int)box_half(radius, k);
    const int bx = (int)floorf(c[0]) - half, by = (int)floorf(c[1]) - half, bz = (int)floorf(c[2]) - half;
    DigResult r = {0u, 0u, 0u, 0u, 0.0};
    uint32_t  x, y, z;
    for (z = 0u; z < VZ; ++z)
        for (y = 0u; y < VY; ++y)
            for (x = 0u; x < VX; ++x) {
                const size_t i  = atlas_index(x, y, z);
                const bool   in = (int)x >= bx && (int)x <= bx + 2 * half && (int)y >= by &&
                                  (int)y <= by + 2 * half && (int)z >= bz && (int)z <= bz + 2 * half;
                const bool   changed = memcmp(&before[i], &after[i], 4u * sizeof(float)) != 0;
                if (!in) { r.outside_changed += changed; continue; }
                {
                    const float px = (float)x + 0.5f - c[0], py = (float)y + 0.5f - c[1], pz = (float)z + 0.5f - c[2];
                    const float brush  = sqrtf(px * px + py * py + pz * pz) - radius;
                    const float expect = clampf(-smin_poly(-before[i], brush, k), -BAND, BAND);
                    const double e = fabs((double)expect - (double)after[i]);
                    if (e > r.worst) r.worst = e;
                    ++r.inside;
                    r.inside_changed += changed;
                    r.color_changed += memcmp(&before[i + 1u], &after[i + 1u], 3u * sizeof(float)) != 0;
                }
            }
    return r;
}

typedef struct TraceStats { double steps; size_t hits, rays; } TraceStats;

static void app_check(App *a) {
    Demo        *d      = &a->demo;
    const size_t texels = (size_t)VX * VY * VZ, pixels = (size_t)d->width * d->height;
    float       *before = NULL, *after = NULL, *reach = NULL;
    uint32_t     x, y, z, cam;
    char         label[112];

    a->auto_sculpt = false;
    a->ball_count  = 0u;
    a->tool        = 0u;
    app_frame(a);
    app_frame(a);
    before = demo_read_floats(d, a->fine, texels * 4u);

    /* ---- 1. one brush ---------------------------------------------------- */
    {
        const float radius = 5.0f, k = 1.5f;
        float       c[3] = {40.3f, 0.0f, 90.6f};
        DigResult   r;
        c[1] = ground_voxels(c[0], c[2]) - 0.5f;
        a->extra     = (Edit){.op = OP_DIG, .centre = {c[0], c[1], c[2]}, .radius = radius, .k = k};
        a->has_extra = true;
        app_frame(a);
        after = demo_read_floats(d, a->fine, texels * 4u);
        r = verify_dig(before, after, c, radius, k);
        demo_check(d, "one dig: voxels changed outside the brush's box", (double)r.outside_changed, "== 0",
                         r.outside_changed == 0u);
        demo_check(d, "one dig: voxels changed inside it", (double)r.inside_changed, "> 500", r.inside_changed > 500u);
        demo_check(d, "one dig: largest difference from the formula on the CPU (voxels)", r.worst, "< 0.004",
                         r.worst < 0.004);
        demo_check(d, "one dig: voxels whose colour changed", (double)r.color_changed, "== 0", r.color_changed == 0u);
        demo_check(d, "one dig: share of the volume the pass visited (reported)",
                   (double)r.inside / ((double)VX * VY * VZ), "", true);
    }

    /* ---- 2. the empty-space level, 3. the field's slope ------------------ */
    {
        static uint8_t occupied[CZ][CY][CX];
        size_t  wrong = 0u, skippable = 0u, band_voxels = 0u, band_edge = 0u;
        double  steepest = 0.0, gradient = 0.0;
        int     cx, cy, cz, ox, oy, oz;
        app_frame(a);
        reach = demo_read_floats(d, a->reach, (size_t)CX * CY * CZ * 4u);
        memset(occupied, 0, sizeof occupied);
        for (z = 0u; z < VZ; ++z)
            for (y = 0u; y < VY; ++y)
                for (x = 0u; x < VX; ++x) {
                    const float v = after[atlas_index(x, y, z)];
                    if (v < BAND * 0.98f) occupied[z / CELL][y / CELL][x / CELL] = 1u;
                    if (x + 1u < VX) { const double s = fabs((double)after[atlas_index(x + 1u, y, z)] - v); if (s > steepest) steepest = s; }
                    if (y + 1u < VY) { const double s = fabs((double)after[atlas_index(x, y + 1u, z)] - v); if (s > steepest) steepest = s; }
                    if (z + 1u < VZ) { const double s = fabs((double)after[atlas_index(x, y, z + 1u)] - v); if (s > steepest) steepest = s; }
                    if (fabsf(v) < 2.0f) {
                        if (x > 0u && x + 1u < VX && y > 0u && y + 1u < VY && z > 0u && z + 1u < VZ) {
                            const double gx = 0.5 * ((double)after[atlas_index(x + 1u, y, z)] - after[atlas_index(x - 1u, y, z)]);
                            const double gy = 0.5 * ((double)after[atlas_index(x, y + 1u, z)] - after[atlas_index(x, y - 1u, z)]);
                            const double gz = 0.5 * ((double)after[atlas_index(x, y, z + 1u)] - after[atlas_index(x, y, z - 1u)]);
                            gradient += sqrt(gx * gx + gy * gy + gz * gz);
                            ++band_voxels;
                        } else ++band_edge;
                    }
                }
        for (cz = 0; cz < (int)CZ; ++cz)
            for (cy = 0; cy < (int)CY; ++cy)
                for (cx = 0; cx < (int)CX; ++cx) {
                    int best = REACH + 1;
                    for (oz = -REACH; oz <= REACH; ++oz)
                        for (oy = -REACH; oy <= REACH; ++oy)
                            for (ox = -REACH; ox <= REACH; ++ox) {
                                const int dist = abs(ox) > abs(oy) ? (abs(ox) > abs(oz) ? abs(ox) : abs(oz))
                                                                   : (abs(oy) > abs(oz) ? abs(oy) : abs(oz));
                                const int nx = cx + ox, ny = cy + oy, nz = cz + oz;
                                if (dist >= best || nx < 0 || ny < 0 || nz < 0 || nx >= (int)CX || ny >= (int)CY || nz >= (int)CZ) continue;
                                if (occupied[nz][ny][nx]) best = dist;
                            }
                    wrong += reach[coarse_index((uint32_t)cx, (uint32_t)cy, (uint32_t)cz)] != (float)best;
                    skippable += best >= 2;
                }
        demo_check(d, "coarse level: cells that differ from a CPU rebuild of it", (double)wrong, "== 0", wrong == 0u);
        demo_check(d, "coarse level: share of cells a ray can jump from (reported)",
                   (double)skippable / ((double)CX * CY * CZ), "", true);
        /* Stepping by d is safe only if d never changes faster than 1 per voxel. */
        demo_check(d, "field: largest change between neighbouring voxels", steepest, "<= 1.005", steepest <= 1.005);
        demo_check(d, "field: mean |gradient| within 2 voxels of the surface (reported; 1 = true distance)",
                   band_voxels ? gradient / (double)band_voxels : 0.0, "", true);
        (void)band_edge;
    }

    /* ---- 4. what the accelerations cost and save ------------------------- */
    for (cam = 0u; cam < 2u; ++cam) {
        static const DemoOrbit k_cameras[2] = {
            {.target = {0.0f, 3.0f, 0.0f}, .distance = 34.0f, .yaw = 0.65f, .pitch = 0.50f, .fov_deg = 45.0f,
             .znear = 0.1f, .zfar = 200.0f},
            {.target = {3.0f, 4.0f, -2.0f}, .distance = 11.0f, .yaw = 2.6f, .pitch = 0.12f, .fov_deg = 60.0f,
             .znear = 0.1f, .zfar = 200.0f}};
        static const char *const k_name[2] = {"overview", "low camera"};
        float     *img[4] = {NULL, NULL, NULL, NULL};
        TraceStats st[4];
        uint32_t   mode;
        size_t     i, late = 0u, flips[4] = {0u}, moved[4] = {0u}, inner[4] = {0u};

        a->orbit  = k_cameras[cam];
        a->view   = 7u;
        a->budget = 2048u;      /* so that no ray gives up: the comparison is about where rays stop */
        for (mode = 0u; mode < 4u; ++mode) {
            /* 0: plain sphere tracing, 1: + coarse level, 2: + pre-pass, 3: + over-relaxed steps */
            a->use_coarse  = mode >= 1u;
            a->use_prepass = mode >= 2u;
            a->relax       = mode >= 3u ? 1.2f : 1.0f;
            app_frame(a);
            img[mode] = demo_read_floats(d, a->hdr, pixels * 4u);
        }
        {
            /* Where did the pre-pass let each ray start? Never beyond the
             * place the plain ray stops: that is the pre-pass's whole promise. */
            float *start;
            a->view = 8u;
            app_frame(a);
            start = demo_read_floats(d, a->hdr, pixels * 4u);
            for (i = 0u; i < pixels; ++i) {
                const float *r = &img[0][i * 4u], *s = &start[i * 4u];
                if (r[3] > 0.5f && s[3] > 0.5f && ((double)s[0] + s[2]) > ((double)r[0] + r[2]) + 1e-3) ++late;
            }
            free(start);
        }
        for (mode = 0u; mode < 4u; ++mode) {
            st[mode] = (TraceStats){0.0, 0u, pixels};
            for (i = 0u; i < pixels; ++i) {
                const float *p = &img[mode][i * 4u], *r = &img[0][i * 4u];
                bool changed = false;
                st[mode].steps += p[1];
                st[mode].hits += p[3] > 0.5f;
                if ((p[3] > 0.5f) != (r[3] > 0.5f)) { ++flips[mode]; changed = true; }
                else if (p[3] > 0.5f) {
                    /* A ray stops at the first sample closer to the surface
                     * than the ray is wide there (tan of the pixel's
                     * half-angle times t). Where exactly inside that shell
                     * depends on where it started, so a different start may
                     * move the hit by about that much and no test of the
                     * picture can ask for less. */
                    const double t = (double)r[0] + r[2], width = (double)a->frame.size[2] * t;
                    if (fabs(((double)p[0] + p[2]) - t) > (2.0 * width > 0.25 ? 2.0 * width : 0.25)) { ++moved[mode]; changed = true; }
                }
                if (changed) {
                    /* Is this ray on a silhouette of the plain image: do its
                     * 3 x 3 neighbours disagree about hitting, or by 2 voxels? */
                    const int px = (int)(i % d->width), py = (int)(i / d->width);
                    double    lo = 1e9, hi = -1e9;
                    int       hit = 0, miss = 0, ox, oy;
                    for (oy = -1; oy <= 1; ++oy)
                        for (ox = -1; ox <= 1; ++ox) {
                            const int nx = px + ox, ny = py + oy;
                            const float *n;
                            if (nx < 0 || ny < 0 || nx >= (int)d->width || ny >= (int)d->height) continue;
                            n = &img[0][((size_t)ny * d->width + (size_t)nx) * 4u];
                            if (n[3] > 0.5f) {
                                const double t = (double)n[0] + n[2];
                                ++hit;
                                if (t < lo) lo = t;
                                if (t > hi) hi = t;
                            } else ++miss;
                        }
                    if (!((hit && miss) || hi - lo > 2.0)) ++inner[mode];
                }
            }
        }
        snprintf(label, sizeof label, "%s: steps per ray, plain sphere tracing (reported)", k_name[cam]);
        demo_check(d, label, st[0].steps / (double)pixels, "", true);
        snprintf(label, sizeof label, "%s: steps per ray, with the coarse level (reported)", k_name[cam]);
        demo_check(d, label, st[1].steps / (double)pixels, "", true);
        snprintf(label, sizeof label, "%s: steps per ray, coarse level + pre-pass (reported)", k_name[cam]);
        demo_check(d, label, st[2].steps / (double)pixels, "", true);
        snprintf(label, sizeof label, "%s: steps per ray, the same with over-relaxation 1.2 (reported)", k_name[cam]);
        demo_check(d, label, st[3].steps / (double)pixels, "", true);
        snprintf(label, sizeof label, "%s: rays that hit (reported)", k_name[cam]);
        demo_check(d, label, (double)st[0].hits, "", true);
        snprintf(label, sizeof label, "%s: rays the pre-pass starts beyond their plain hit", k_name[cam]);
        demo_check(d, label, (double)late, "== 0", late == 0u);
        for (mode = 1u; mode < 4u; ++mode) {
            const char *what = mode == 1u ? "coarse level" : mode == 2u ? "coarse level + pre-pass" : "all three";
            snprintf(label, sizeof label, "%s, %s: rays whose hit/miss changed", k_name[cam], what);
            demo_check(d, label, (double)flips[mode], mode == 1u ? "== 0" : "< 0.05 % of rays",
                             mode == 1u ? flips[mode] == 0u : (double)flips[mode] < 0.0005 * (double)pixels);
            snprintf(label, sizeof label, "%s, %s: hits that moved more than 2 ray widths", k_name[cam], what);
            demo_check(d, label, (double)moved[mode], mode == 1u ? "== 0" : "< 0.05 % of rays",
                             mode == 1u ? moved[mode] == 0u : (double)moved[mode] < 0.0005 * (double)pixels);
            snprintf(label, sizeof label, "%s, %s: of those two, rays not on a silhouette", k_name[cam], what);
            demo_check(d, label, (double)inner[mode], "== 0", inner[mode] == 0u);
        }
        snprintf(label, sizeof label, "%s: steps saved by coarse level + pre-pass (ratio)", k_name[cam]);
        demo_check(d, label, st[0].steps / st[2].steps, "> 1.5", st[0].steps > 1.5 * st[2].steps);
        snprintf(label, sizeof label, "%s: the coarse level alone saves steps (ratio)", k_name[cam]);
        demo_check(d, label, st[0].steps / st[1].steps, "> 1", st[1].steps < st[0].steps);
        snprintf(label, sizeof label, "%s: over-relaxation takes no more steps than without", k_name[cam]);
        demo_check(d, label, st[3].steps / st[2].steps, "<= 1", st[3].steps <= st[2].steps);
        free(img[0]); free(img[1]); free(img[2]); free(img[3]);
    }
    /* ---- 5. the pointer's brush: picked and applied without the CPU ------ */
    {
        const uint32_t px = d->width / 2u, py = d->height / 2u;
        const float    radius = a->brush_radius, k = 1.5f;
        float         *image, *dug, pick[4], seen[3], rd[3], len, t;
        uint32_t       i;
        DigResult      r;

        a->orbit       = (DemoOrbit){.target = {0.0f, 3.0f, 0.0f}, .distance = 34.0f, .yaw = 0.65f, .pitch = 0.50f,
                                     .fov_deg = 45.0f, .znear = 0.1f, .zfar = 200.0f};
        a->view        = 7u;
        a->use_coarse  = true;
        a->use_prepass = false;
        a->tool        = 1u;                               /* dig */
        a->picking     = true;
        a->frame.mouse[0] = ((float)px + 0.5f) / (float)d->width * 2.0f - 1.0f;
        a->frame.mouse[1] = 1.0f - ((float)py + 0.5f) / (float)d->height * 2.0f;
        /* Frame A: nothing to apply yet; the Pick pass traces the pointer. */
        app_frame(a);
        image = demo_read_floats(d, a->hdr, pixels * 4u);
        demo_readback_buffer(d, a->pick_data, pick, sizeof pick);
        /* Frame B: the Edit pass digs at that point; nobody told the CPU where. */
        a->picking = false;
        app_frame(a);
        dug = demo_read_floats(d, a->fine, texels * 4u);
        a->tool = 0u;

        /* Where the picture of frame A shows the surface under the pointer. */
        t = image[((size_t)py * d->width + px) * 4u] + image[((size_t)py * d->width + px) * 4u + 2u];
        for (i = 0u; i < 3u; ++i)
            rd[i] = a->frame.forward[i] + a->frame.mouse[0] * a->frame.right[i] + a->frame.mouse[1] * a->frame.up[i];
        len = sqrtf(rd[0] * rd[0] + rd[1] * rd[1] + rd[2] * rd[2]);
        for (i = 0u; i < 3u; ++i) seen[i] = a->frame.eye[i] + rd[i] / len * t;
        len = sqrtf((seen[0] - pick[0]) * (seen[0] - pick[0]) + (seen[1] - pick[1]) * (seen[1] - pick[1]) +
                    (seen[2] - pick[2]) * (seen[2] - pick[2]));
        r = verify_dig(after, dug, pick, radius, k);
        demo_check(d, "pointer: the pick pass found a surface", (double)pick[3], "== 1", pick[3] == 1.0f);
        /* The picture's ray stops within its own width of the surface, the
         * pick ray within 0.02 voxels: allow for the difference. */
        demo_check(d, "pointer: picked point to the surface drawn at that pixel (voxels)", (double)len,
                         "< 1/4 + 2 ray widths", len < 0.25f + 2.0f * a->frame.size[2] * t);
        demo_check(d, "pointer: voxels the next frame's brush changed", (double)r.inside_changed, "> 500",
                         r.inside_changed > 500u);
        demo_check(d, "pointer: voxels changed outside the box around the picked point", (double)r.outside_changed,
                         "== 0", r.outside_changed == 0u);
        demo_check(d, "pointer: largest difference from the formula at the picked point (voxels)", r.worst,
                         "< 0.004", r.worst < 0.004);
        free(image); free(dug);
    }
    /* ---- 6. a stone, and the indirect dispatch it asks for ---------------- */
    {
        Ball      two[2];
        uint32_t  args[8] = {0u}, frames = 0u, want = box_groups(box_half(STONE_RADIUS, STONE_K));
        float     impact[4] = {0.0f}, zero[(MAX_BALLS + 1u) * 4u];
        float    *still, *hit;
        DigResult r;
        size_t    idle_changed = 0u, i;

        memset(zero, 0, sizeof zero);
        memset(two, 0, sizeof two);
        /* Ball 0, a stone, falls on the ground; ball 1 stays high in the sky. */
        two[0].pos_radius[0] = 30.5f; two[0].pos_radius[1] = 40.0f; two[0].pos_radius[2] = 100.5f; two[0].pos_radius[3] = 1.9f;
        two[0].vel_state[1]  = -30.0f;
        two[1].pos_radius[0] = 20.5f; two[1].pos_radius[1] = 62.0f; two[1].pos_radius[2] = 20.5f; two[1].pos_radius[3] = 1.5f;
        demo_upload(d, a->ball_data, two, sizeof two);
        demo_upload(d, a->impact_data, zero, MAX_BALLS * 16u);
        demo_upload(d, a->args_data, zero, (MAX_BALLS + 1u) * 16u);
        a->ball_count = 2u;
        a->view       = 0u;
        /* While nothing has hit, both brushes are dispatched with zero groups. */
        app_frame(a);
        app_frame(a);
        still = demo_read_floats(d, a->fine, texels * 4u);
        do {
            app_frame(a);
            demo_readback_buffer(d, (BufSlice){a->args_data.buffer, a->args_data.offset, 32u}, args, sizeof args);
        } while (args[0] == 0u && ++frames < 120u);
        /* The frame the stone hit: the Balls pass has written the impact and
         * the dispatch; the Edit pass has not run on them yet. */
        hit = demo_read_floats(d, a->fine, texels * 4u);
        demo_readback_buffer(d, (BufSlice){a->impact_data.buffer, a->impact_data.offset, 16u}, impact, sizeof impact);
        for (i = 0u; i < texels * 4u; ++i) idle_changed += still[i] != hit[i];
        free(still);
        app_frame(a);
        still = demo_read_floats(d, a->fine, texels * 4u);
        r = verify_dig(hit, still, impact, STONE_RADIUS, STONE_K);
        a->ball_count = 0u;
        demo_check(d, "stone: frames until it hit (reported)", (double)frames, "", true);
        demo_check(d, "stone: values changed while both dispatches had zero groups", (double)idle_changed, "== 0",
                         idle_changed == 0u);
        demo_check(d, "stone: groups the GPU wrote for its brush", (double)args[0], "== the brush's box",
                         args[0] == want && args[1] == want && args[2] == want);
        demo_check(d, "stone: groups written for the ball still in the air", (double)(args[4] + args[5] + args[6]),
                         "== 0", args[4] + args[5] + args[6] == 0u);
        demo_check(d, "stone: voxels its crater changed", (double)r.inside_changed, "> 300", r.inside_changed > 300u);
        demo_check(d, "stone: voxels changed outside the box around the impact", (double)r.outside_changed, "== 0",
                         r.outside_changed == 0u);
        demo_check(d, "stone: largest difference from the formula at the impact (voxels)", r.worst, "< 0.004",
                         r.worst < 0.004);
        free(still); free(hit);
    }
    free(before); free(after); free(reach);
}
