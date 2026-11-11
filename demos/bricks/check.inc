/* check.inc — bricks' acceptance test; included by main.c.
 *
 * --check also builds the dense volume of the same samples (run it with
 * --bricks 18: 127^3 samples, 16 MB) and compares the sparse one with it
 * texel by texel, then the two pictures.
 */





static float *picture(App *a, uint32_t mode, uint32_t view) {
    a->mode = mode; a->view = view;
    app_frame(a);
    return demo_read_floats(&a->demo, a->hdr, (size_t)a->demo.width * a->demo.height * 4u);
}

static void app_check(App *a) {
    Demo          *d = &a->demo;
    const uint32_t B = a->bricks, N = samples(a), side = a->atlas_side * 8u;
    const size_t   pixels = (size_t)d->width * d->height;
    const double   voxel = WORLD / (double)(N - 1u);
    float         *ind, *atlas, *dense;
    uint8_t       *used;
    uint32_t       b, stored = 0u, bad_slots = 0u, t;
    size_t         empty_samples = 0u, unsafe = 0u, wrong_sign = 0u, stored_texels = 0u;
    double         worst = 0.0, least = 1e30;

    app_frame(a);   /* the build */
    app_frame(a);   /* the report */
    ind   = demo_read_floats(d, a->indirection, (size_t)B * B * B * 4u);
    atlas = demo_read_floats(d, a->atlas, (size_t)side * side * side * 4u);
    dense = demo_read_floats(d, a->dense, (size_t)N * N * N * 4u);
    used  = (uint8_t *)calloc((size_t)a->atlas_side * a->atlas_side * a->atlas_side, 1u);

    for (b = 0u; b < B * B * B; ++b) {
        const uint32_t bx = b % B, by = b / B % B, bz = b / (B * B);
        const float   *e  = ind + (size_t)b * 4u;
        if (e[3] == 0.0f) {
            /* Stored: its slot is its own, and all 512 texels are the dense volume's. */
            const uint32_t sx = (uint32_t)e[0], sy = (uint32_t)e[1], sz = (uint32_t)e[2];
            const uint32_t slot = sx + a->atlas_side * (sy + a->atlas_side * sz);
            ++stored;
            if (sx >= a->atlas_side || sy >= a->atlas_side || sz >= a->atlas_side || slot >= a->stored || used[slot]) { ++bad_slots; continue; }
            used[slot] = 1u;
            for (t = 0u; t < 512u; ++t) {
                const uint32_t tx = t % 8u, ty = t / 8u % 8u, tz = t / 64u;
                const float   *s  = atlas + ((size_t)(sx * 8u + tx) + side * ((size_t)(sy * 8u + ty) + side * (size_t)(sz * 8u + tz))) * 4u;
                const float   *r  = dense + ((size_t)(bx * 7u + tx) + N * ((size_t)(by * 7u + ty) + N * (size_t)(bz * 7u + tz))) * 4u;
                uint32_t       c;
                for (c = 0u; c < 4u; ++c) { const double x = fabs((double)s[c] - r[c]); if (x > worst) worst = x; }
                ++stored_texels;
            }
        } else {
            /* Empty: one distance for the whole brick. It must not exceed the true one anywhere in it. */
            if (fabs((double)e[3]) < least) least = fabs((double)e[3]);
            for (t = 0u; t < 512u; ++t) {
                const uint32_t tx = t % 8u, ty = t / 8u % 8u, tz = t / 64u;
                const float    r  = dense[((size_t)(bx * 7u + tx) + N * ((size_t)(by * 7u + ty) + N * (size_t)(bz * 7u + tz))) * 4u];
                ++empty_samples;
                if (fabs((double)e[3]) > fabs((double)r) + 1e-3 * voxel) ++unsafe;
                if ((e[3] < 0.0f) != (r < 0.0f)) ++wrong_sign;
            }
        }
    }
    demo_check(d, "bricks stored (share)", (double)stored / (double)(B * B * B), "0 < share < 0.5", stored > 0u && stored * 2u < B * B * B);
    demo_check(d, "bricks stored: indirection volume vs the allocator's count", (double)stored - (double)a->stored, "== 0", stored == a->stored);
    demo_check(d, "bricks that wanted a slot and got none", (double)a->wanted - (double)a->stored, "== 0", a->wanted == a->stored);
    demo_check(d, "slots out of range or given twice", (double)bad_slots, "== 0", bad_slots == 0u);
    demo_check(d, "stored texels compared", (double)stored_texels, "> 100000", stored_texels > 100000u);
    demo_check(d, "stored texels vs the dense volume, largest difference (distance and normal)", worst, "== 0", worst == 0.0);
    demo_check(d, "samples in empty bricks compared", (double)empty_samples, "> 1000000", empty_samples > 1000000u);
    demo_check(d, "... where the brick's distance exceeds the true one", (double)unsafe, "== 0", unsafe == 0u);
    demo_check(d, "... where the sign is wrong", (double)wrong_sign, "== 0", wrong_sign == 0u);
    demo_check(d, "smallest distance of an empty brick, voxels", least / voxel, ">= 2", least / voxel >= 1.999);
    free(ind); free(atlas); free(dense); free(used);

    /* The pictures. */
    {
        static const DemoOrbit k_cameras[2] = {
            {.target = {8.0f, 3.0f, 8.0f}, .distance = 21.0f, .yaw = 0.5f, .pitch = 0.42f, .fov_deg = 50.0f, .znear = 0.1f, .zfar = 200.0f},
            {.target = {8.0f, 1.8f, 8.0f}, .distance = 5.0f, .yaw = 2.2f, .pitch = 0.12f, .fov_deg = 50.0f, .znear = 0.1f, .zfar = 200.0f}};
        static const char *const k_name[2] = {"overview", "inside the field"};
        uint32_t cam;
        char     label[160];
        for (cam = 0u; cam < 2u; ++cam) {
            float *sparse_lit, *dense_lit, *sparse_ray, *dense_ray;
            size_t i, hit = 0u, differ = 0u, far_off = 0u;
            double colour = 0.0, colour_worst = 0.0, depth = 0.0, depth_worst = 0.0, steps[2] = {0.0, 0.0}, fetches[2] = {0.0, 0.0};
            a->orbit = k_cameras[cam];
            sparse_lit = picture(a, 0u, 0u); dense_lit = picture(a, 1u, 0u);
            sparse_ray = picture(a, 0u, 4u); dense_ray = picture(a, 1u, 4u);
            for (i = 0u; i < pixels; ++i) {
                const bool hs = sparse_ray[i * 4u + 2u] >= 0.0f, hd = dense_ray[i * 4u + 2u] >= 0.0f;
                uint32_t   c;
                steps[0] += sparse_ray[i * 4u]; steps[1] += dense_ray[i * 4u];
                fetches[0] += sparse_ray[i * 4u + 1u]; fetches[1] += dense_ray[i * 4u + 1u];
                if (hs != hd) { ++differ; continue; }
                for (c = 0u; c < 3u; ++c) {
                    const double x = fabs((double)sparse_lit[i * 4u + c] - dense_lit[i * 4u + c]);
                    colour += x / 3.0;
                    if (x > colour_worst) colour_worst = x;
                }
                if (hs) {
                    const double x = fabs((double)sparse_ray[i * 4u + 2u] - dense_ray[i * 4u + 2u]) / voxel;
                    ++hit; depth += x;
                    if (x > depth_worst) depth_worst = x;
                    if (x > 1.0) ++far_off;
                }
            }
            snprintf(label, sizeof label, "%s: rays that hit in both", k_name[cam]);
            demo_check(d, label, (double)hit, "> 20 % of the picture", hit * 5u > pixels);
            snprintf(label, sizeof label, "%s: rays that hit in one and miss in the other", k_name[cam]);
            demo_check(d, label, (double)differ, "< 0.1 % of the picture", differ * 1000u < pixels);
            snprintf(label, sizeof label, "%s: rays whose hit distance differs by more than a voxel", k_name[cam]);
            demo_check(d, label, (double)far_off, "< 0.5 % of the picture", far_off * 200u < pixels);
            snprintf(label, sizeof label, "%s: hit distance, mean difference (voxels)", k_name[cam]);
            demo_check(d, label, hit ? depth / (double)hit : 0.0, "< 0.2", depth < 0.2 * (double)hit);
            snprintf(label, sizeof label, "%s: hit distance, largest difference (voxels)", k_name[cam]);
            demo_check(d, label, depth_worst, "(reported)", true);
            snprintf(label, sizeof label, "%s: colour, mean difference", k_name[cam]);
            demo_check(d, label, colour / (double)pixels, "< 0.002", colour < 0.002 * (double)pixels);
            snprintf(label, sizeof label, "%s: colour, largest difference (reported)", k_name[cam]);
            demo_check(d, label, colour_worst, "", true);
            snprintf(label, sizeof label, "%s: steps per ray, sparse (reported)", k_name[cam]);
            demo_check(d, label, steps[0] / (double)pixels, "", true);
            snprintf(label, sizeof label, "%s: steps per ray, dense (reported)", k_name[cam]);
            demo_check(d, label, steps[1] / (double)pixels, "", true);
            snprintf(label, sizeof label, "%s: texture fetches per ray, sparse (reported)", k_name[cam]);
            demo_check(d, label, fetches[0] / (double)pixels, "", true);
            snprintf(label, sizeof label, "%s: texture fetches per ray, dense (reported)", k_name[cam]);
            demo_check(d, label, fetches[1] / (double)pixels, "", true);
            free(sparse_lit); free(dense_lit); free(sparse_ray); free(dense_ray);
        }
    }
    a->mode = 0u; a->view = 0u;
}
