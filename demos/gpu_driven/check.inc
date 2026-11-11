/* check.inc — gpu_driven's acceptance test; included by main.c.
 *
 * At several cameras, including jumps that make last frame's visibility
 * useless and a start with no visibility at all:
 *   - the culled frame equals the frame with every box drawn, bit for bit
 *   - every box that shows at least one pixel is in a draw list, once
 *   - the GPU's frustum set is the CPU's
 */

typedef struct Lists {
    uint32_t  count[2];
    uint32_t *ids[2];
} Lists;

static void run_frame(App *a, uint32_t mode, uint32_t view) {
    a->mode = mode;
    a->view = view;
    set_camera(a);
    render(a);
}

/* The two draw commands the culling wrote, and the instance lists they draw. */
static void read_lists(App *a, Lists *l) {
    uint32_t i, cmd[4];
    for (i = 0u; i < 2u; ++i) {
        demo_readback_buffer(&a->demo, a->cmd[i], cmd, sizeof cmd);
        if (cmd[0] != 36u || cmd[1] > COUNT || cmd[2] != 0u || cmd[3] != 0u)
            demo_fatal(&a->demo, "check: draw command %u is {%u, %u, %u, %u}", i, cmd[0], cmd[1], cmd[2], cmd[3]);
        l->count[i] = cmd[1];
        demo_readback_buffer(&a->demo, a->list[i], l->ids[i], (size_t)COUNT * 4u);
    }
}

typedef struct Pose { bool overview; float yaw; const char *label; } Pose;

static void app_check(App *a) {
    /* "steady": the third frame at one camera. */
    static const Pose k_poses[] = {
        {true, 2.90f, NULL},
        {false, 0.60f, "cut to the plaza"},
        {false, 0.60f, "plaza, the frame after the cut"},
        {false, 0.60f, "plaza, steady"},
        {false, 0.65f, "plaza, turned 3 degrees"},
        {false, 1.40f, "plaza, turned 43 degrees"},
        {true, 0.60f, "cut to the overview"},
        {true, 0.60f, "overview, the frame after the cut"},
        {true, 0.60f, "overview, steady"},
        {false, 2.00f, NULL},
        {false, 4.20f, NULL}};
    Demo        *d      = &a->demo;
    const size_t pixels = (size_t)d->width * d->height;
    uint16_t    *culled = (uint16_t *)malloc(pixels * 8u), *all = (uint16_t *)malloc(pixels * 8u);
    uint8_t     *shows  = (uint8_t *)malloc(COUNT), *drawn = (uint8_t *)malloc(COUNT);
    uint32_t    *zeros  = (uint32_t *)calloc(COUNT, 4u);
    Lists        lists;
    size_t       different = 0u, missed = 0u, twice = 0u, frustum_wrong = 0u, bad_ids = 0u, i;
    size_t       steady_added = 0u, cold_a = 0u, lit_different = 0u;
    uint32_t     k;
    char         label[160];

    lists.ids[0] = (uint32_t *)malloc((size_t)COUNT * 4u);
    lists.ids[1] = (uint32_t *)malloc((size_t)COUNT * 4u);
    a->still = true;

    for (k = 0u; k <= sizeof k_poses / sizeof k_poses[0]; ++k) {
        /* The last round repeats the plaza after wiping what the GPU remembers. */
        const bool  cold = k == sizeof k_poses / sizeof k_poses[0];
        const Pose *pose = &k_poses[cold ? 1u : k];
        size_t      in_frustum = 0u, visible = 0u, in_gpu_frustum;
        Lists       two;
        set_overview(a, pose->overview);
        a->orbit.yaw = pose->yaw;
        if (cold) demo_upload(d, a->flags, zeros, (size_t)COUNT * 4u);

        /* Culled, then everything, both showing the instance per pixel. */
        run_frame(a, 0u, 1u);
        demo_readback(d, a->hdr, culled, pixels * 8u);
        read_lists(a, &lists);
        two = lists; /* counts; the ids are used right away */
        memset(drawn, 0, COUNT);
        for (i = 0u; i < 2u; ++i) {
            uint32_t n;
            for (n = 0u; n < lists.count[i]; ++n) {
                const uint32_t id = lists.ids[i][n];
                if (id >= COUNT) { ++bad_ids; continue; }
                if (drawn[id]) ++twice;
                drawn[id] = 1u;
            }
        }
        run_frame(a, 2u, 1u);
        demo_readback(d, a->hdr, all, pixels * 8u);
        memset(shows, 0, COUNT);
        for (i = 0u; i < pixels; ++i) {
            if (memcmp(&culled[i * 4u], &all[i * 4u], 8u) != 0) ++different;
            if (demo_half_to_float(all[i * 4u + 3u]) > 0.5f) {
                const uint32_t id = (uint32_t)demo_half_to_float(all[i * 4u]) + 1024u * (uint32_t)demo_half_to_float(all[i * 4u + 1u]);
                if (id < COUNT) shows[id] = 1u;
            }
        }
        for (i = 0u; i < COUNT; ++i) {
            visible += shows[i];
            if (shows[i] && !drawn[i]) ++missed;
        }

        /* Frustum only: the GPU's list against the CPU's test. */
        run_frame(a, 1u, 1u);
        read_lists(a, &lists);
        memset(drawn, 0, COUNT);
        for (i = 0u; i < lists.count[0]; ++i)
            if (lists.ids[0][i] < COUNT) drawn[lists.ids[0][i]] = 1u;
        in_gpu_frustum = lists.count[0];
        for (i = 0u; i < COUNT; ++i) {
            const bool cpu = cpu_in_frustum(a->push.vp, &a->scene[i]);
            in_frustum += cpu;
            if (cpu != (drawn[i] != 0u)) ++frustum_wrong;
        }
        if (lists.count[1] != 0u) ++frustum_wrong;
        (void)in_gpu_frustum;

        if (cold) cold_a = two.count[0];
        if (pose->label && strstr(pose->label, "steady")) steady_added += two.count[1];
        if (pose->label || cold) {
            const char *name = cold ? "nothing remembered" : pose->label;
            snprintf(label, sizeof label, "%s: boxes in the frustum, of %u (reported)", name, COUNT);
            demo_check(d, label, (double)in_frustum, "", true);
            snprintf(label, sizeof label, "%s: drawn in phase A (reported)", name);
            demo_check(d, label, (double)two.count[0], "", true);
            snprintf(label, sizeof label, "%s: added in phase B (reported)", name);
            demo_check(d, label, (double)two.count[1], "", true);
            snprintf(label, sizeof label, "%s: boxes that show at least one pixel (reported)", name);
            demo_check(d, label, (double)visible, "", true);
        }
        /* Once, the picture itself rather than the instance numbers. */
        if (k == 4u) {
            run_frame(a, 0u, 0u);
            demo_readback(d, a->hdr, culled, pixels * 8u);
            run_frame(a, 2u, 0u);
            demo_readback(d, a->hdr, all, pixels * 8u);
            for (i = 0u; i < pixels; ++i)
                if (memcmp(&culled[i * 4u], &all[i * 4u], 8u) != 0) ++lit_different;
        }
    }
    demo_check(d, "cameras tried (reported)", (double)k, "", true);
    demo_check(d, "pixels where the culled frame differs from drawing every box, all cameras", (double)different, "== 0", different == 0u);
    demo_check(d, "the same for the lit picture, one camera", (double)lit_different, "== 0", lit_different == 0u);
    demo_check(d, "boxes that show a pixel but are in neither draw list", (double)missed, "== 0", missed == 0u);
    demo_check(d, "boxes in both draw lists, or twice in one", (double)twice, "== 0", twice == 0u);
    demo_check(d, "list entries that are not a box", (double)bad_ids, "== 0", bad_ids == 0u);
    demo_check(d, "boxes where the GPU's frustum test and the CPU's disagree", (double)frustum_wrong, "== 0", frustum_wrong == 0u);
    demo_check(d, "steady cameras: boxes added in phase B", (double)steady_added, "== 0", steady_added == 0u);
    demo_check(d, "nothing remembered: boxes drawn in phase A", (double)cold_a, "== 0", cold_a == 0u);
    free(culled); free(all); free(shows); free(drawn); free(zeros); free(lists.ids[0]); free(lists.ids[1]);
}
