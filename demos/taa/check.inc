/* check.inc — taa's acceptance test; included by main.c.
 *
 * The reference is the mean of 64 frames, each shifted to one cell of an
 * 8 x 8 grid inside the pixel and read back unresolved: 64 samples per pixel,
 * averaged on the CPU. The resolve shader has no part in it.
 */

#define END 7.854f   /* the ball crosses the middle at full speed */



/* The picture the last frame made, as floats (rgba). */
static float *grab(App *a) {
    return demo_read_floats(&a->demo, a->history[a->parity], (size_t)a->demo.width * a->demo.height * 4u);
}

static float *reference(App *a, float time) {
    Demo        *d     = &a->demo;
    const size_t count = (size_t)d->width * d->height * 4u;
    double      *sum   = (double *)calloc(count, sizeof(double));
    float       *out   = (float *)malloc(count * sizeof(float));
    const bool   taa   = a->taa;
    uint32_t     k;
    size_t       i;
    a->taa = false; a->override_jitter = true; a->manual_time = time;
    for (k = 0u; k < 64u; ++k) {
        float *f;
        a->manual_jitter[0] = ((float)(k % 8u) + 0.5f) / 8.0f - 0.5f;
        a->manual_jitter[1] = ((float)(k / 8u) + 0.5f) / 8.0f - 0.5f;
        app_frame(a);
        f = grab(a);
        for (i = 0u; i < count; ++i) sum[i] += (double)f[i];
        free(f);
    }
    a->taa = taa; a->override_jitter = false;
    for (i = 0u; i < count; ++i) out[i] = (float)(sum[i] / 64.0);
    free(sum);
    return out;
}

/* `frames` frames ending at time `end`, 1/60 s apart, from an empty history; returns the last picture. */
static float *run(App *a, bool taa, bool clamp, bool velocity, float end, uint32_t frames, bool moving) {
    uint32_t f;
    a->taa = taa; a->clamp = clamp; a->use_velocity = velocity;
    reset_history(a);
    for (f = 0u; f < frames; ++f) {
        a->manual_time = moving ? end - (float)(frames - 1u - f) / 60.0f : end;
        app_frame(a);
    }
    return grab(a);
}

/* Mean absolute difference of r, g, b over the pixels where mask is set (or all). */
static double difference(const float *x, const float *y, const uint8_t *mask, size_t pixels, size_t *used) {
    double sum = 0.0;
    size_t i, n = 0u;
    for (i = 0u; i < pixels; ++i) {
        if (mask && !mask[i]) continue;
        ++n;
        sum += (fabs((double)x[i * 4u] - y[i * 4u]) + fabs((double)x[i * 4u + 1u] - y[i * 4u + 1u]) +
                fabs((double)x[i * 4u + 2u] - y[i * 4u + 2u])) / 3.0;
    }
    if (used) *used = n;
    return n ? sum / (double)n : 0.0;
}

static void app_check(App *a) {
    Demo        *d      = &a->demo;
    const size_t pixels = (size_t)d->width * d->height;
    const float  spin   = a->spin;
    char         label[160];

    a->manual = true;
    a->alpha  = 0.1f;

    /* 1. Nothing moves. */
    {
        float *ref, *none, *with_clamp, *no_clamp, *next;
        double e_none, e_clamp, e_free;
        a->spin = 0.0f;
        ref        = reference(a, 0.0f);
        none       = run(a, false, true, true, 0.0f, 1u, false);
        no_clamp   = run(a, true, false, true, 0.0f, 64u, false);
        with_clamp = run(a, true, true, true, 0.0f, 64u, false);
        app_frame(a);
        next = grab(a);
        e_none  = difference(none, ref, NULL, pixels, NULL);
        e_clamp = difference(with_clamp, ref, NULL, pixels, NULL);
        e_free  = difference(no_clamp, ref, NULL, pixels, NULL);
        demo_check(d, "still: no anti-aliasing, error", e_none, "> 0.005 (the scene aliases)", e_none > 0.005);
        demo_check(d, "still: TAA after 64 frames, error", e_clamp, "< half of no anti-aliasing", e_clamp < 0.5 * e_none);
        demo_check(d, "still: TAA without clamp after 64 frames, error", e_free, "< half of no anti-aliasing", e_free < 0.5 * e_none);
        {
            const double flicker = difference(next, with_clamp, NULL, pixels, NULL);
            demo_check(d, "still: TAA, change from one frame to the next", flicker, "< 0.01 (a guard; measured, not derived)", flicker < 0.01);
        }
        free(ref); free(none); free(with_clamp); free(no_clamp); free(next);
    }

    /* 2. Is the velocity right? Draw time END - 4/60 s, then one frame at END that only fetches
     *    the old picture from where the velocity target points (new-frame weight 0, no clamp),
     *    and compare with the picture drawn at END. Without velocity the old picture stays put. */
    {
        static const char *const k_what[2] = {"ball moves, on the ball", "camera turns, lowest quarter"};
        uint32_t test;
        for (test = 0u; test < 2u; ++test) {
            float   *now, *fetched[2];
            uint8_t *mask = NULL;
            size_t   used = pixels, i;
            uint32_t v;
            double   e[2];
            a->spin = test ? 0.3f : 0.0f;
            now = run(a, false, false, false, END, 1u, true);
            /* Ball: the ball is the only red thing. Camera: the lowest quarter of the picture, where
             * the floor's squares are many pixels wide; further up the floor aliases, and aliasing
             * does not move with the surface. */
            mask = (uint8_t *)calloc(pixels, 1u);
            used = 0u;
            for (i = 0u; i < pixels; ++i) {
                mask[i] = test == 0u ? now[i * 4u] > 3.0f * now[i * 4u + 1u] : i / d->width >= d->height - d->height / 4u;
                used += mask[i];
            }
            for (v = 0u; v < 2u; ++v) {
                float *old = run(a, false, false, false, END - 4.0f / 60.0f, 1u, true);
                free(old);
                a->taa = true; a->clamp = false; a->use_velocity = v == 1u; a->alpha = 0.0f;
                a->manual_time = END;
                app_frame(a);
                fetched[v] = grab(a);
                a->alpha = 0.1f;
                e[v] = difference(fetched[v], now, mask, pixels, NULL);
            }
            snprintf(label, sizeof label, "%s: pixels", k_what[test]);
            demo_check(d, label, (double)used, "> 1000", used > 1000u);
            snprintf(label, sizeof label, "%s: old picture left in place vs new, difference", k_what[test]);
            demo_check(d, label, e[0], "(reported)", true);
            snprintf(label, sizeof label, "%s: old picture fetched along the velocity vs new, difference", k_what[test]);
            demo_check(d, label, e[1], "< a quarter of that", e[1] < 0.25 * e[0]);
            free(now); free(fetched[0]); free(fetched[1]); free(mask);
        }
    }

    /* 3. The ball moves (2 m/s at that moment); 4. the camera turns. Sixty frames up to END. */
    {
        static const char *const k_what[2] = {"ball moves", "camera turns"};
        uint32_t test;
        for (test = 0u; test < 2u; ++test) {
            const float end = END;
            float      *ref, *before, *none, *full, *no_clamp, *no_velocity;
            uint8_t    *mask = NULL;
            size_t      used = pixels, i;
            a->spin = test ? 0.3f : 0.0f;
            ref    = reference(a, end);
            before = reference(a, end - 0.25f);
            none        = run(a, false, true, true, end, 1u, true);
            full        = run(a, true, true, true, end, 60u, true);
            no_clamp    = run(a, true, false, true, end, 60u, true);
            no_velocity = run(a, true, true, false, end, 60u, true);
            if (test == 0u) {            /* only where the last quarter second changed the picture */
                mask = (uint8_t *)calloc(pixels, 1u);
                used = 0u;
                for (i = 0u; i < pixels; ++i) {
                    const double e = fabs((double)ref[i * 4u] - before[i * 4u]) + fabs((double)ref[i * 4u + 1u] - before[i * 4u + 1u]) +
                                     fabs((double)ref[i * 4u + 2u] - before[i * 4u + 2u]);
                    mask[i] = e > 0.06;
                    used += mask[i];
                }
                snprintf(label, sizeof label, "%s: pixels the last 0.25 s changed", k_what[test]);
                demo_check(d, label, (double)used, "> 1000", used > 1000u);
            }
            {
                const double e_none = difference(none, ref, mask, pixels, NULL), e_full = difference(full, ref, mask, pixels, NULL);
                const double e_free = difference(no_clamp, ref, mask, pixels, NULL);
                const double e_put  = difference(no_velocity, ref, mask, pixels, NULL);
                snprintf(label, sizeof label, "%s: no anti-aliasing, error", k_what[test]);
                /* Where the ball has just been, TAA is NOT better than nothing (see README); only the turning camera asserts it. */
                demo_check(d, label, e_none, test ? "> TAA" : "(reported)", test ? e_none > e_full : true);
                snprintf(label, sizeof label, "%s: TAA, error", k_what[test]);
                demo_check(d, label, e_full, "(reported)", true);
                snprintf(label, sizeof label, "%s: TAA without clamp, error", k_what[test]);
                demo_check(d, label, e_free, "> TAA", e_free > e_full);
                snprintf(label, sizeof label, "%s: TAA without velocity, error", k_what[test]);
                demo_check(d, label, e_put, "> TAA", e_put > e_full);
            }
            if (mask) {
                snprintf(label, sizeof label, "%s: whole picture: no anti-aliasing, error (reported)", k_what[test]);
                demo_check(d, label, difference(none, ref, NULL, pixels, NULL), "", true);
                snprintf(label, sizeof label, "%s: whole picture: TAA, error (reported)", k_what[test]);
                demo_check(d, label, difference(full, ref, NULL, pixels, NULL), "", true);
            }
            free(ref); free(before); free(none); free(full); free(no_clamp); free(no_velocity); free(mask);
        }
    }
    a->spin = spin; a->manual = false;
    a->taa = a->clamp = a->use_velocity = true;
}
