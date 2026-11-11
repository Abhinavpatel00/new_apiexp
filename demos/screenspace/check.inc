/* check.inc — screenspace's acceptance test; included by main.c.
 *
 * Occlusion: 16 samples and a blur against 256 samples of the same estimator;
 * a bare floor must stay unoccluded. Reflections: the screen-space result
 * against the scene actually mirrored in the floor, pixel by pixel.
 */

/* One frame, then a half-float target the size of the picture as floats. */
static float *render_read(App *a, Target t) {
    app_frame(a);
    return demo_read_floats(&a->demo, t, (size_t)a->demo.width * a->demo.height * 4u);
}

static void app_check(App *a) {
    static const DemoOrbit k_cameras[3] = {
        {.target = {0.0f, 0.8f, 0.0f}, .distance = 11.0f, .yaw = 0.45f, .pitch = 0.30f, .fov_deg = 50.0f, .znear = 0.1f, .zfar = 100.0f},
        {.target = {0.3f, 0.9f, 0.5f}, .distance = 8.0f, .yaw = -0.7f, .pitch = 0.10f, .fov_deg = 55.0f, .znear = 0.1f, .zfar = 100.0f},
        /* From above: what the floor should show is mostly the undersides and far sides, which no pixel has. */
        {.target = {0.0f, 0.5f, 0.5f}, .distance = 10.0f, .yaw = 0.45f, .pitch = 1.0f, .fov_deg = 50.0f, .znear = 0.1f, .zfar = 100.0f}};
    static const char *const k_name[3] = {"overview", "low camera", "from above"};
    Demo        *d      = &a->demo;
    const size_t pixels = (size_t)d->width * d->height;
    const App    saved  = *a;
    char     label[160];
    uint32_t cam;
    size_t   i;

    for (cam = 0u; cam < 3u; ++cam) {
        const bool judged = cam < 2u;
        float *albedo, *reference, *raw, *blurred, *ssr, *planar;
        size_t geometry = 0u, dark = 0u, floor_px = 0u;
        double raw_sum = 0.0, raw_worst = 0.0, blur_sum = 0.0, blur_worst = 0.0, mean = 0.0, darkest = 1.0;
        size_t shown = 0u, found = 0u, missed = 0u, wrong = 0u, false_hits = 0u;
        double colour = 0.0;

        a->orbit = k_cameras[cam];
        a->samples = 256u;
        reference = render_read(a, a->ao_raw);
        a->samples = saved.samples;
        raw     = render_read(a, a->ao_raw);
        blurred = render_read(a, a->ao);
        albedo  = render_read(a, a->scene.albedo);
        ssr     = render_read(a, a->ssr);
        planar  = render_read(a, a->planar);

        /* Geometry: the sky's occlusion is exactly 1 in all three. A pixel of geometry has albedo > 0. */
        for (i = 0u; i < pixels; ++i) {
            const float *al = albedo + i * 4u;
            const double r  = reference[i * 4u];
            if (al[0] + al[1] + al[2] <= 0.0f) continue;
            ++geometry;
            mean += r;
            if (r < 0.8) ++dark;
            if (r < darkest) darkest = r;
            raw_sum  += fabs(raw[i * 4u] - r);
            blur_sum += fabs(blurred[i * 4u] - r);
            if (fabs(raw[i * 4u] - r) > raw_worst) raw_worst = fabs(raw[i * 4u] - r);
            if (fabs(blurred[i * 4u] - r) > blur_worst) blur_worst = fabs(blurred[i * 4u] - r);
        }
        snprintf(label, sizeof label, "%s: pixels of geometry", k_name[cam]);
        demo_check(d, label, (double)geometry, "> 40 % of the picture", geometry * 5u > pixels * 2u);
        snprintf(label, sizeof label, "%s, occlusion with 256 samples: mean (reported)", k_name[cam]);
        demo_check(d, label, mean / (double)geometry, "", true);
        snprintf(label, sizeof label, "%s, occlusion with 256 samples: share of pixels below 0.8", k_name[cam]);
        demo_check(d, label, (double)dark / (double)geometry, "> 0.01", dark * 100u > geometry);
        snprintf(label, sizeof label, "%s, occlusion with 256 samples: darkest (reported)", k_name[cam]);
        demo_check(d, label, darkest, "", true);
        snprintf(label, sizeof label, "%s, %u samples as sampled vs 256: mean difference", k_name[cam], a->samples);
        demo_check(d, label, raw_sum / (double)geometry, "< 0.01", raw_sum < 0.01 * (double)geometry);
        snprintf(label, sizeof label, "%s, %u samples as sampled vs 256: largest (reported)", k_name[cam], a->samples);
        demo_check(d, label, raw_worst, "", true);
        snprintf(label, sizeof label, "%s, %u samples blurred vs 256: mean difference", k_name[cam], a->samples);
        demo_check(d, label, blur_sum / (double)geometry, "< as sampled", blur_sum < raw_sum);
        snprintf(label, sizeof label, "%s, %u samples blurred vs 256: largest (reported)", k_name[cam], a->samples);
        demo_check(d, label, blur_worst, "", true);

        /* Reflections, on the floor's pixels. */
        for (i = 0u; i < pixels; ++i) {
            const float *s = ssr + i * 4u, *p = planar + i * 4u;
            double       e;
            if (albedo[i * 4u + 3u] < 0.5f) continue;
            ++floor_px;
            e = fabs((double)s[0] - p[0]);
            if (fabs((double)s[1] - p[1]) > e) e = fabs((double)s[1] - p[1]);
            if (fabs((double)s[2] - p[2]) > e) e = fabs((double)s[2] - p[2]);
            if (p[3] > 0.5f) {
                ++shown;
                if (s[3] < 0.5f) ++missed;
                else if (e > 0.05) ++wrong;
                else { ++found; colour += e; }
            } else if (s[3] > 0.5f) ++false_hits;
        }
        snprintf(label, sizeof label, "%s: floor pixels", k_name[cam]);
        demo_check(d, label, (double)floor_px, "> 20 % of the picture", floor_px * 5u > pixels);
        snprintf(label, sizeof label, "%s: floor pixels whose true reflection shows an object", k_name[cam]);
        demo_check(d, label, (double)shown, "> 2000", shown > 2000u);
        snprintf(label, sizeof label, "%s: ... found in screen space, colour within 0.05 (share)", k_name[cam]);
        demo_check(d, label, (double)found / (double)(shown ? shown : 1u), judged ? "> 0.9 (guard)" : "reported",
                         !judged || found * 10u > shown * 9u);
        snprintf(label, sizeof label, "%s: ... not found, sky shown instead (share)", k_name[cam]);
        demo_check(d, label, (double)missed / (double)(shown ? shown : 1u), "reported", true);
        snprintf(label, sizeof label, "%s: ... something else shown (share)", k_name[cam]);
        demo_check(d, label, (double)wrong / (double)(shown ? shown : 1u), judged ? "< 0.05 (guard)" : "reported",
                         !judged || wrong * 20u < shown);
        snprintf(label, sizeof label, "%s: where found, mean colour difference", k_name[cam]);
        demo_check(d, label, colour / (double)(found ? found : 1u), "< 0.01", colour < 0.01 * (double)(found ? found : 1u));
        snprintf(label, sizeof label, "%s: an object shown where the true reflection is sky (share of the floor)", k_name[cam]);
        demo_check(d, label, (double)false_hits / (double)(floor_px ? floor_px : 1u), "< 0.005", false_hits * 200u < floor_px);
        free(albedo); free(reference); free(raw); free(blurred); free(ssr); free(planar);
    }

    /* A bare floor: nothing can block anything. */
    for (cam = 0u; cam < 2u; ++cam) {
        float *raw;
        double lowest = 1.0;
        a->orbit = k_cameras[cam];
        a->draw_count = 1u;
        raw = render_read(a, a->ao_raw);
        for (i = 0u; i < pixels; ++i)
            if (raw[i * 4u] < lowest) lowest = raw[i * 4u];
        free(raw);
        snprintf(label, sizeof label, "%s, the floor alone: lowest occlusion value", k_name[cam]);
        demo_check(d, label, lowest, "== 1", lowest == 1.0);
    }
    *a = saved;
}
