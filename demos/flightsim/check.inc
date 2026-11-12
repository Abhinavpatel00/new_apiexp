/* check.inc — the numeric acceptance test for flightsim, run by --check.
 *
 * Two families of question, and they are checked differently because they fail
 * differently.
 *
 * The physics is checked by running it. No GPU, no readback, no tolerance
 * invented to make a picture pass: a trimmed aircraft either holds its altitude
 * over five seconds or it does not, and a stalled one either drops or it does
 * not. These are the checks that catch a sign error in a moment coefficient,
 * which renders as a perfectly beautiful aircraft that flies upside down.
 *
 * The renderer is checked by reading back what it drew. The one that matters
 * most is terrain_cpu_gpu: the simulator collides with fs_terrain_height() on
 * the CPU while the rasterizer draws fsTerrainHeight() on the GPU, and those two
 * are separate implementations of the same function reached through separate
 * noise ports. If they disagree, the aircraft flies through a mountain or lands
 * on nothing, and no screenshot shows it — both pictures look like terrain.
 * So the check reconstructs the centre pixel's ray, marches the CPU terrain
 * along it, and compares the distance the GPU stored. The tolerance is the
 * clipmap's chord error, which is a function of the ring spacing and not a
 * number chosen to make the test pass: a real disagreement between the noise
 * ports is hundreds of metres.
 *
 * GPU frames are expensive here (Lavapipe, two cores), so the image checks share
 * one or two frames rather than each taking their own.
 */

/* March the CPU terrain along one ray and return the distance to the first
 * intersection, or -1 when it does not hit within `far`. Coarse steps to find
 * the bracket, then bisection inside it: a fixed fine step over 20 km would be
 * a million evaluations of a four-octave noise field. */
static float check_ray_terrain(const float eye[3], const float rd[3], float far_m)
{
    float t = 0.0f, coarse = 24.0f;
    while (t < far_m) {
        const float x = eye[0] + rd[0] * t;
        const float y = eye[1] + rd[1] * t;
        const float z = eye[2] + rd[2] * t;
        if (y <= fs_terrain_height(x, z)) {
            /* Bisect the last coarse interval, which is the one that contains
             * the crossing. */
            float lo = t - coarse, hi = t;
            for (int k = 0; k < 24; ++k) {
                const float mid = 0.5f * (lo + hi);
                const float mx  = eye[0] + rd[0] * mid;
                const float my  = eye[1] + rd[1] * mid;
                const float mz  = eye[2] + rd[2] * mid;
                if (my <= fs_terrain_height(mx, mz)) hi = mid; else lo = mid;
            }
            return 0.5f * (lo + hi);
        }
        t += coarse;
        /* Widen as the ray goes: the terrain cannot be missed by a step that is
         * small compared to the distance already travelled. */
        coarse *= 1.06f;
    }
    return -1.0f;
}

static void app_check(App *a)
{
    Demo  *d = &a->demo;
    const uint32_t W = d->width, H = d->height;

    /* ---- the airframe ------------------------------------------------------ */
    demo_check(d, "airframe vertices", (double)a->vertex_count, "> 0 and <= the buffer",
               a->vertex_count > 0u && a->vertex_count <= AIRFRAME_MAX);

    /* ---- terrain, CPU side ------------------------------------------------- */
    {
        float lowest = 1e30f, ground_lowest = 1e30f;
        int   finite = 1;
        for (int i = 0; i < 512; ++i) {
            const float x = (float)(i % 32 - 16) * 4200.0f;
            const float z = (float)(i / 32 - 8) * 4200.0f;
            const float h = fs_terrain_height(x, z);
            const float g = fs_ground_height(x, z);
            if (!isfinite(h) || !isfinite(g)) finite = 0;
            if (h < lowest) lowest = h;
            if (g < ground_lowest) ground_lowest = g;
        }
        demo_check(d, "terrain finite over 512 samples", (double)finite, "== 1", finite);
        /* The basin floor is clamped in both implementations; a value below it
         * means one of them lost the clamp. */
        demo_check(d, "terrain above the basin floor", (double)lowest, ">= -420.5", lowest >= -420.5f);
        demo_check(d, "ground never below sea level", (double)ground_lowest, ">= 0", ground_lowest >= 0.0f);
    }

    /* ---- the flight models ------------------------------------------------- */
    {
        /* A trimmed aircraft in still air should hold its altitude. This is the
         * check that catches a sign error in cm_alpha or cl_alpha, because the
         * failure mode is a phugoid that grows until the aircraft is inverted. */
        Sim s; GroundHeights g = {0};
        fs_sim_reset(&s, &fs_model_default, 0u, 12345u);
        s.arcade = false; s.turbulence = 0.0f; s.wind_base[0] = s.wind_base[1] = s.wind_base[2] = 0.0f;
        g.player = -400.0f;
        for (int i = 0; i < 1200; ++i) fs_sim_step(&s, 1.0f / 240.0f, &g);   /* 5 s */
        const float held = fabsf(s.player.pos[1] - 2200.0f);
        demo_check(d, "6DOF trimmed holds altitude over 5 s", (double)held, "< 250 m", held < 250.0f);
        demo_check(d, "6DOF trimmed stays flying", (double)s.player.airspeed, "> 30 m/s",
                   s.player.airspeed > 30.0f && !s.crashed);

        /* Full back stick has to stall: the angle of attack has to pass the
         * stall angle and the aircraft has to lose altitude. A model that cannot
         * stall is a model whose lift curve never flattens, which is the other
         * half of the same sign error. */
        fs_sim_reset(&s, &fs_model_default, 0u, 12345u);
        s.arcade = false; s.turbulence = 0.0f; s.wind_base[0] = s.wind_base[1] = s.wind_base[2] = 0.0f;
        s.player.cmd[0] = -1.0f;                      /* elevator full back     */
        g.player = -400.0f;
        float peak_alpha = 0.0f, peak_alt = 0.0f;
        int   stalled = 0;
        for (int i = 0; i < 3600; ++i) {          /* 15 s: zoom climb, departure, fall */
            fs_sim_step(&s, 1.0f / 240.0f, &g);
            if (s.player.alpha > peak_alpha) peak_alpha = s.player.alpha;
            /* Full back stick buys a zoom climb first: the aircraft trades
             * speed for height and only then departs. Measuring the loss
             * against the start altitude would therefore punish a model that
             * stalls correctly, so the descent is measured from the peak
             * reached after the stall actually began. */
            if (!stalled && s.player.alpha > fs_model_default.alpha_stall) {
                stalled = 1;
                peak_alt = s.player.pos[1];
            }
            if (stalled && s.player.pos[1] > peak_alt) peak_alt = s.player.pos[1];
        }
        demo_check(d, "6DOF full back stick exceeds the stall angle",
                   (double)mu_degrees(peak_alpha), "> 15.5 deg",
                   peak_alpha > fs_model_default.alpha_stall);
        demo_check(d, "6DOF stalled aircraft loses altitude",
                   (double)(peak_alt - s.player.pos[1]), "> 100 m after the stall",
                   stalled && peak_alt - s.player.pos[1] > 100.0f);

        /* The arcade model is a rate controller with a speed target: at full
         * throttle it must reach that target and it must not stall, because not
         * stalling is the entire reason the mode exists. */
        fs_sim_reset(&s, &fs_model_default, 0u, 12345u);
        s.arcade = true; s.turbulence = 0.0f; s.wind_base[0] = s.wind_base[1] = s.wind_base[2] = 0.0f;
        s.player.cmd_throttle = 1.0f;
        g.player = -400.0f;
        for (int i = 0; i < 2400; ++i) fs_sim_step(&s, 1.0f / 240.0f, &g);
        demo_check(d, "arcade reaches its full-throttle speed", (double)s.player.airspeed,
                   "within 15 m/s of the target",
                   fabsf(s.player.airspeed - fs_model_default.arcade_speed_max) < 15.0f);

        /* Both models have to produce a finite attitude, or everything
         * downstream of them is drawing garbage. */
        demo_check(d, "arcade attitude finite", (double)s.player.att[3], "|w| <= 1",
                   isfinite(s.player.att[0]) && isfinite(s.player.att[1]) &&
                   isfinite(s.player.att[2]) && fabsf(s.player.att[3]) <= 1.0001f);

        /* Ground contact has to be detected, because a simulator that does not
         * notice the ground is a simulator with no consequences. */
        fs_sim_reset(&s, &fs_model_default, 0u, 12345u);
        s.arcade = true; s.turbulence = 0.0f; s.wind_base[0] = s.wind_base[1] = s.wind_base[2] = 0.0f;
        s.player.pos[1] = 400.0f;
        /* A held full stick does not dive, it loops: at the arcade pitch rate
         * the aircraft closes a circle a few hundred metres across and comes
         * back over the top. Forward stick for 0.6 s and then hands off puts
         * it into a steady dive that the ground has to notice. */
        g.player = 0.0f;
        for (int i = 0; i < 2400 && !s.crashed; ++i) {
            s.player.cmd[0] = i < 144 ? 1.0f : 0.0f;
            fs_sim_step(&s, 1.0f / 240.0f, &g);
        }
        demo_check(d, "flying into the ground crashes", (double)s.crashed, "== 1", s.crashed);
        demo_check(d, "crash is reported at a plausible speed", (double)s.crash_speed,
                   "> 10 m/s", s.crash_speed > 10.0f);

        /* The traffic law has to hold an altitude and not wander off: a pursuit
         * law that overshoots produces aircraft orbiting a point they passed
         * ten seconds ago. */
        fs_sim_reset(&s, &fs_model_default, 6u, 999u);
        s.arcade = false; s.turbulence = 0.0f; s.wind_base[0] = s.wind_base[1] = s.wind_base[2] = 0.0f;
        for (int i = 0; i < 6; ++i) g.traffic[i] = -400.0f;
        float drift = 0.0f;
        for (int i = 0; i < 2400; ++i) {                      /* 10 s */
            fs_sim_step(&s, 1.0f / 240.0f, &g);
            for (uint32_t k = 0u; k < s.traffic_count; ++k) {
                const float err = fabsf(s.traffic[k].pos[1] - s.waypoint[k][1]);
                if (err > drift) drift = err;
            }
        }
        demo_check(d, "traffic holds its assigned altitude", (double)drift, "< 400 m over 10 s",
                   drift < 400.0f);
        demo_check(d, "traffic stays airborne", (double)s.traffic_count, "== 6",
                   s.traffic_count == 6u && s.traffic_lost == 0u);
    }

    /* ---- the renderer ------------------------------------------------------ */
    /* One frame with clouds off and the HUD off, at the defaults the run was
     * started with, is the reference every image check below reads. */
    a->cloud_mode = 0u;
    a->show_hud = false;
    app_frame(a);
    app_frame(a);

    float *linear = (float *)malloc((size_t)W * H * sizeof(float));
    demo_readback(d, a->scene_linear, linear, (size_t)W * H * sizeof(float));

    /* The centre pixel's ray, reconstructed exactly as clouds.slang does it, so
     * the comparison is against the same ray the GPU traced. */
    {
        const float aspect = (float)W / (float)H;
        const float tan_y  = tanf(mu_radians(a->fov_deg) * 0.5f);
        const float ndcx   = ((float)(W / 2u) + 0.5f) / (float)W * 2.0f - 1.0f;
        const float ndcy   = 1.0f - ((float)(H / 2u) + 0.5f) / (float)H * 2.0f;
        float rd[3] = {a->fwd[0] + a->right[0] * (ndcx * aspect * tan_y) + a->up[0] * (ndcy * tan_y),
                       a->fwd[1] + a->right[1] * (ndcx * aspect * tan_y) + a->up[1] * (ndcy * tan_y),
                       a->fwd[2] + a->right[2] * (ndcx * aspect * tan_y) + a->up[2] * (ndcy * tan_y)};
        mu_vec3_normalize(rd, rd);

        const float gpu_dist = linear[(size_t)(H / 2u) * W + (W / 2u)];
        const float cpu_dist = check_ray_terrain(a->eye, rd, 60000.0f);
        const bool  gpu_hit  = gpu_dist > 0.0f;
        const bool  cpu_hit  = cpu_dist > 0.0f;

        if (gpu_hit && cpu_hit) {
            /* The GPU's surface is a clipmap mesh, so it sits off the exact
             * field by its chord error, which is bounded by the ring spacing
             * covering the hit point and the steepest slope the terrain
             * generator makes. That is the tolerance, and it is derived rather
             * than tuned: a disagreement between the CPU and GPU noise ports
             * moves the surface by hundreds of metres, not by a chord. */
            const float tol = 6.0f * a->ring_spacing + 40.0f;
            demo_check(d, "CPU and GPU terrain agree at the centre pixel",
                       (double)fabsf(gpu_dist - cpu_dist), "< the clipmap chord error",
                       fabsf(gpu_dist - cpu_dist) < tol);
            const float gpu_h = a->eye[1] - gpu_dist * -rd[1];
            const float cpu_h = a->eye[1] - cpu_dist * -rd[1];
            demo_check(d, "terrain height agrees in metres", (double)fabsf(gpu_h - cpu_h),
                       "< 80 m", fabsf(gpu_h - cpu_h) < 80.0f);
        } else {
            /* Either both hit or both miss; one of each means the mesh and the
             * field disagree about whether there is ground there at all. */
            demo_check(d, "CPU and GPU agree on whether the centre ray hits ground",
                       (double)(gpu_hit ? 1 : 0), "== the CPU's answer", gpu_hit == cpu_hit);
        }
    }

    /* The image must not be blank, and must contain both sky and ground: a
     * frame that is all one colour is a pass that did not run. */
    {
        uint8_t *img = (uint8_t *)malloc((size_t)W * H * 4u);
        demo_readback(d, d->out, img, (size_t)W * H * 4u);
        uint32_t bright = 0u, dark = 0u, distinct = 0u;
        uint32_t last = 0xFFFFFFFFu;
        for (uint32_t i = 0u; i < W * H; ++i) {
            const uint32_t px = (uint32_t)img[i * 4u] | ((uint32_t)img[i * 4u + 1u] << 8) |
                                ((uint32_t)img[i * 4u + 2u] << 16);
            const uint32_t luma = (img[i * 4u] * 299u + img[i * 4u + 1u] * 587u +
                                   img[i * 4u + 2u] * 114u) / 1000u;
            if (luma > 150u) bright++;
            /* Ground counts when it is dark OR green-dominant: sunlit grass at
             * the spawn sits around luma 150, so a luma threshold alone would
             * call a bright field sky and fail a frame that is mostly field. */
            if (luma < 70u || img[i * 4u + 1u] > img[i * 4u + 2u] + 8u) dark++;
            if (px != last) distinct++;
            last = px;
        }
        demo_check(d, "the frame has sky in it", (double)bright, "> 1% of pixels",
                   bright * 100u > W * H);
        demo_check(d, "the frame has ground in it", (double)dark, "> 1% of pixels",
                   dark * 100u > W * H);
        demo_check(d, "the frame is not one colour", (double)distinct, "> 5% of pixels",
                   distinct * 20u > W * H);
        free(img);
    }
    free(linear);

    /* ---- the shape volumes actually got baked ------------------------------ */
    /* Every cloud mode above 1 reads its density from the baked volumes, and a
     * dispatch that silently ran nothing would leave them zero and the sky
     * empty with no other symptom. Read the base volume back and look at it. */
    {
        const size_t n = (size_t)BASE_HI_DIM * BASE_HI_DIM * BASE_HI_DIM;
        uint16_t *vol = (uint16_t *)malloc(n * 4u * sizeof(uint16_t));
        demo_readback(d, a->base_hi, vol, n * 4u * sizeof(uint16_t));
        double sum = 0.0, peak = 0.0;
        for (size_t i = 0; i < n; ++i) {
            const float v = demo_half_to_float(vol[i * 4u]);
            sum += v;
            if (v > peak) peak = v;
        }
        demo_check(d, "the base cloud volume is baked", sum / (double)n,
                   "> 0.02 mean density", sum / (double)n > 0.02 && peak > 0.5);
        free(vol);
    }

    /* ---- the weather map responds to coverage ------------------------------ */
    /* Read the baked map at two coverage settings. Coverage remaps the field
     * rather than scaling it, so a low setting has to produce fewer cloud
     * texels and not merely thinner ones; that is the property this checks. */
    {
        uint8_t *map = (uint8_t *)malloc((size_t)WEATHER_DIM * WEATHER_DIM * 4u);
        double   means[2] = {0.0, 0.0};
        const float settings[2] = {0.15f, 0.95f};
        for (int k = 0; k < 2; ++k) {
            /* Coverage decides how many cells there are, and the cells are
             * drawn at reset, so moving the slider alone would re-bake the
             * same sky. Reset with the same seed at both settings: same cell
             * shapes, more of them, which is the property under test. */
            fs_weather_reset(&a->weather, a->sim.player.pos, settings[k], (uint64_t)a->seed32);
            a->bake_weather = true;
            app_frame(a);
            demo_readback(d, a->weather_map, map, (size_t)WEATHER_DIM * WEATHER_DIM * 4u);
            uint64_t sum = 0u;
            for (uint32_t i = 0u; i < WEATHER_DIM * WEATHER_DIM; ++i) sum += map[i * 4u];
            means[k] = (double)sum / (double)(WEATHER_DIM * WEATHER_DIM);
        }
        demo_check(d, "more coverage bakes a denser weather map", means[1] - means[0],
                   "> 20/255 higher at 0.95 than at 0.15", means[1] - means[0] > 20.0);
        demo_check(d, "the weather map is not saturated", means[1], "< 250/255", means[1] < 250.0);
        /* Back to the run's own sky: cells are drawn at reset, so putting the
         * field back means re-seeding it, not just writing the slider. */
        fs_weather_reset(&a->weather, a->sim.player.pos, a->coverage, (uint64_t)a->seed32);
        a->bake_weather = true;
        free(map);
    }

    /* ---- the clouds actually draw ------------------------------------------ */
    {
        uint8_t *off_img = (uint8_t *)malloc((size_t)W * H * 4u);
        uint8_t *on_img  = (uint8_t *)malloc((size_t)W * H * 4u);
        double   off_sum = 0.0, on_sum = 0.0, diff = 0.0;

        a->cloud_mode = 0u;                      /* off: resolve returns the scene */
        app_frame(a); app_frame(a);
        demo_readback(d, d->out, off_img, (size_t)W * H * 4u);

        a->cloud_mode = 4u;                      /* temporal, the default and the fullest */
        app_frame(a); app_frame(a); app_frame(a);
        demo_readback(d, d->out, on_img, (size_t)W * H * 4u);

        for (uint32_t i = 0u; i < (size_t)W * H * 4u; ++i) {
            off_sum += off_img[i];
            on_sum  += on_img[i];
            diff    += fabs((double)on_img[i] - (double)off_img[i]);
        }
        diff /= (double)(W * H);
        demo_check(d, "clouds change the picture", diff, "> 0.5 mean channel difference",
                   diff > 0.5);
        /* Clouds are brighter than the sky they replace only near the sun and
         * darker elsewhere, so the honest invariant is that the total changes,
         * in either direction, by a visible amount. */
        demo_check(d, "clouds change the frame's total light",
                   fabs(on_sum - off_sum) / (double)((size_t)W * H * 4u), "> 0.5",
                   fabs(on_sum - off_sum) / (double)((size_t)W * H * 4u) > 0.5);
        free(off_img); free(on_img);
    }

    a->show_hud = true;
}
