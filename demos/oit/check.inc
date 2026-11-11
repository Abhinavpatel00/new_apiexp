/* check.inc — oit's acceptance test; included by main.c.
 *
 * The reference is depth peeling: per pixel, the layers in exact order.
 */

/* One frame in a mode, then a half-float target as floats. */
static float *render(App *a, uint32_t mode, Target target) {
    a->mode = mode;
    app_frame(a);
    return demo_read_floats(&a->demo, target, (size_t)a->demo.width * a->demo.height * 4u);
}

/* Mean over pixels of the mean absolute difference of r, g, b; pixels chosen by layer count. */
static double mean_error(const float *x, const float *ref, const float *layers, size_t pixels, uint32_t lo, uint32_t hi,
                         double *worst, size_t *used) {
    double sum = 0.0;
    size_t i, n = 0u;
    if (worst) *worst = 0.0;
    for (i = 0u; i < pixels; ++i) {
        const uint32_t c = (uint32_t)(layers[i * 4u] + 0.5f);
        uint32_t       k;
        if (c < lo || c > hi) continue;
        ++n;
        for (k = 0u; k < 3u; ++k) {
            const double e = fabs((double)x[i * 4u + k] - (double)ref[i * 4u + k]);
            sum += e / 3.0;
            if (worst && e > *worst) *worst = e;
        }
    }
    if (used) *used = n;
    return n ? sum / (double)n : 0.0;
}

static void app_check(App *a) {
    static const DemoOrbit k_cameras[2] = {
        {.target = {0.0f, 1.0f, 0.0f}, .distance = 6.5f, .yaw = 0.35f, .pitch = 0.32f, .fov_deg = 50.0f, .znear = 0.1f, .zfar = 100.0f},
        {.target = {0.0f, 1.0f, 0.0f}, .distance = 6.0f, .yaw = 1.9f, .pitch = 0.35f, .fov_deg = 50.0f, .znear = 0.1f, .zfar = 100.0f}};
    static const char *const k_name[2] = {"along the stack", "across it"};
    Demo        *d      = &a->demo;
    const size_t pixels = (size_t)d->width * d->height;
    uint32_t     cam;
    char         label[160];

    for (cam = 0u; cam < 2u; ++cam) {
        float   *layers, *ref, *ref_rev, *weighted, *weighted_rev, *plain, *fitted, *fitted_rev, *order, *order_rev, *ref_black, *weighted_black;
        double   worst, worst_fitted;
        size_t   used, i, covered = 0u;
        uint32_t deepest = 0u;

        a->orbit = k_cameras[cam];
        a->weight = 0u; a->reverse = false; a->black = false;
        a->count_layers = true;
        layers = render(a, MODE_WEIGHTED, a->accum);
        a->count_layers = false;
        ref      = render(a, MODE_PEELED, a->hdr);
        weighted = render(a, MODE_WEIGHTED, a->hdr);
        order    = render(a, MODE_DRAW_ORDER, a->hdr);
        a->weight = WEIGHT_NONE;
        plain = render(a, MODE_WEIGHTED, a->hdr);
        a->weight = WEIGHT_FITTED;
        fitted = render(a, MODE_WEIGHTED, a->hdr);
        a->reverse = true;
        fitted_rev = render(a, MODE_WEIGHTED, a->hdr);
        a->reverse = false;
        a->weight = WEIGHT_GENERAL;
        a->reverse = true;
        ref_rev      = render(a, MODE_PEELED, a->hdr);
        weighted_rev = render(a, MODE_WEIGHTED, a->hdr);
        order_rev    = render(a, MODE_DRAW_ORDER, a->hdr);
        a->reverse = false;
        a->black = true;
        ref_black      = render(a, MODE_PEELED, a->hdr);
        weighted_black = render(a, MODE_WEIGHTED, a->hdr);
        a->black = false;

        for (i = 0u; i < pixels; ++i) {
            const uint32_t c = (uint32_t)(layers[i * 4u] + 0.5f);
            if (c) ++covered;
            if (c > deepest) deepest = c;
        }
        snprintf(label, sizeof label, "%s: pixels with something transparent (share)", k_name[cam]);
        demo_check(d, label, (double)covered / (double)pixels, "> 0.1", covered * 10u > pixels);
        snprintf(label, sizeof label, "%s: most transparent layers on a pixel", k_name[cam]);
        demo_check(d, label, (double)deepest, "<= 12 (what the reference can separate)", deepest <= PEELS);

        /* The reference must not depend on draw order at all. */
        mean_error(ref_rev, ref, layers, pixels, 0u, 99u, &worst, NULL);
        snprintf(label, sizeof label, "%s: reference, drawn in reverse order, largest difference", k_name[cam]);
        demo_check(d, label, worst, "== 0", worst == 0.0);
        mean_error(weighted_rev, weighted, layers, pixels, 0u, 99u, &worst, NULL);
        snprintf(label, sizeof label, "%s: weighted, drawn in reverse order, largest difference", k_name[cam]);
        demo_check(d, label, worst, "< 0.005", worst < 0.005);
        mean_error(fitted_rev, fitted, layers, pixels, 0u, 99u, &worst, NULL);
        snprintf(label, sizeof label, "%s: fitted weight, drawn in reverse order, largest difference", k_name[cam]);
        demo_check(d, label, worst, "(reported)", true);
        snprintf(label, sizeof label, "%s: draw order, drawn in reverse order, mean difference (reported)", k_name[cam]);
        demo_check(d, label, mean_error(order_rev, order, layers, pixels, 1u, 99u, NULL, NULL), "", true);

        /* Where nothing is transparent, and where one layer is, there is nothing to guess. */
        mean_error(weighted, ref, layers, pixels, 0u, 0u, &worst, NULL);
        snprintf(label, sizeof label, "%s: no layer: weighted vs reference, largest difference", k_name[cam]);
        demo_check(d, label, worst, "== 0", worst == 0.0);
        mean_error(weighted, ref, layers, pixels, 1u, 1u, &worst, &used);
        snprintf(label, sizeof label, "%s: one layer: pixels", k_name[cam]);
        demo_check(d, label, (double)used, "> 1000", used > 1000u);
        snprintf(label, sizeof label, "%s: one layer: weighted vs reference, largest difference", k_name[cam]);
        demo_check(d, label, worst, "< 0.003", worst < 0.003);
        mean_error(fitted, ref, layers, pixels, 1u, 1u, &worst, NULL);
        snprintf(label, sizeof label, "%s: one layer: fitted weight vs reference, largest difference", k_name[cam]);
        demo_check(d, label, worst, "(reported)", true);
        /* Coverage is exact with any number of layers: with black glass only the background is left. */
        mean_error(weighted_black, ref_black, layers, pixels, 0u, 99u, &worst, NULL);
        snprintf(label, sizeof label, "%s: black glass (coverage only): weighted vs reference, largest difference", k_name[cam]);
        demo_check(d, label, worst, "< 0.003", worst < 0.003);

        /* The guess. */
        {
            static const uint32_t k_lo[3] = {2u, 3u, 5u}, k_hi[3] = {2u, 4u, 99u};
            static const char *const k_bucket[3] = {"2 layers", "3-4 layers", "5+ layers"};
            uint32_t b;
            for (b = 0u; b < 3u; ++b) {
                const double ew = mean_error(weighted, ref, layers, pixels, k_lo[b], k_hi[b], &worst, &used);
                const double ep = mean_error(plain, ref, layers, pixels, k_lo[b], k_hi[b], NULL, NULL);
                const double eo = mean_error(order, ref, layers, pixels, k_lo[b], k_hi[b], NULL, NULL);
                const double er = mean_error(order_rev, ref, layers, pixels, k_lo[b], k_hi[b], NULL, NULL);
                snprintf(label, sizeof label, "%s: %s: pixels (reported)", k_name[cam], k_bucket[b]);
                demo_check(d, label, (double)used, "", true);
                if (!used) continue;
                const double ef = mean_error(fitted, ref, layers, pixels, k_lo[b], k_hi[b], &worst_fitted, NULL);
                snprintf(label, sizeof label, "%s: %s: weighted, mean error", k_name[cam], k_bucket[b]);
                demo_check(d, label, ew, "< 0.06 (guards against regressions; measured, not derived)", ew < 0.06);
                snprintf(label, sizeof label, "%s: %s: fitted weight, mean error (reported)", k_name[cam], k_bucket[b]);
                demo_check(d, label, ef, "", true);
                snprintf(label, sizeof label, "%s: %s: fitted weight, largest error (reported)", k_name[cam], k_bucket[b]);
                demo_check(d, label, worst_fitted, "", true);
                snprintf(label, sizeof label, "%s: %s: weighted, largest error (reported)", k_name[cam], k_bucket[b]);
                demo_check(d, label, worst, "", true);
                snprintf(label, sizeof label, "%s: %s: no weight, mean error (reported)", k_name[cam], k_bucket[b]);
                demo_check(d, label, ep, "", true);
                snprintf(label, sizeof label, "%s: %s: draw order, mean error (reported)", k_name[cam], k_bucket[b]);
                demo_check(d, label, eo, "", true);
                snprintf(label, sizeof label, "%s: %s: reverse draw order, mean error (reported)", k_name[cam], k_bucket[b]);
                demo_check(d, label, er, "", true);
            }
        }
        free(layers); free(ref); free(weighted); free(order); free(plain); free(fitted); free(fitted_rev); free(ref_rev); free(weighted_rev);
        free(order_rev); free(ref_black); free(weighted_black);
    }
    a->mode = MODE_WEIGHTED;
}
