/* shadows — shadow filtering lab.
 *
 *   Shadow0 (2048^2, whole scene)  Shadow1..3 (1024^2 cascades)   depth only
 *     -> Forward (RGBA16F + D32F: six filters, split screen, cascades)
 *     -> Present
 *
 * Filters, bias and cascade blending are external/mu_lib/mu/shaders/
 * shadows.slang. First demo to use comparison samplers (SamplerDesc.compare)
 * and pipe_intern_depth_only().
 *
 * --check measures the PCSS penumbra under a floating plate against the
 * geometric prediction, and the disagreement between neighbouring cascades
 * inside the blend band.
 */
#include "demo.h"
#include "mesh.h"

#include "mu_math.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SHADERS  "demos/shadows/shaders/"
#define MAPS     4u
#define MAX_OBJS 48u

typedef struct FrameData {
    float    view_proj[16], light_vp[MAPS][16];
    float    eye[4], forward[4], light[4], splits[4], bias[4], filter[4];
    float    texel_world[4], depth_range[4], inv_size[4];
    uint32_t map[4], samplers[4];
} FrameData;
_Static_assert(sizeof(FrameData) == 496, "FrameData must match shadows.slang");

typedef struct DrawPush {
    uint32_t vertex_offset, object_offset, frame_offset, map, technique_a, technique_b;
    float    split;
    uint32_t view, flags, pad[3];
} DrawPush;

typedef struct App {
    Demo      demo; /* first: see demo.h */
    DemoOrbit orbit;
    DemoMesh  mesh;
    DemoObject objects[MAX_OBJS];
    DemoRange  ranges[MAX_OBJS];
    uint32_t   object_count;
    BufSlice   vertices, object_data, frame_data;
    Target     maps[MAPS], hdr, depth;
    uint32_t   map_size[MAPS], map_index[MAPS];
    Sampler    cmp_nearest, cmp_linear;
    Pipe       shadow_pipe, forward_pipe;
    AttachDesc map_depth[MAPS], hdr_color, hdr_depth;
    BufUse     reads[3];
    FrameData  frame;
    /* parameters */
    uint32_t technique_a, technique_b, view;
    float    split, sun_yaw, sun_height, light_size, pcf_radius, search_radius;
    float    bias_const, bias_slope, bias_normal, blend_width;
    bool     no_bias;      /* --no-bias */
    float    split_depth[3];
    bool     cascades, calibration;
    uint32_t pcss_rotations;
} App;

static const char *const k_techniques[] = {
    "hard (1 tap)", "PCF 3x3, hard taps", "hardware bilinear (1 tap)",
    "PCF 3x3, bilinear taps", "Poisson 16, rotated", "PCSS"};
static const char *const k_views[] = {"shaded", "shadow factor", "cascades"};

/* Orthographic, depth 0 at znear and 1 at zfar, looking down -Z. */
static void ortho_zo(float *r, float half, float znear, float zfar) {
    mu_mat4_identity(r);
    r[0]  = 1.0f / half;
    r[5]  = 1.0f / half;
    r[10] = -1.0f / (zfar - znear);
    r[14] = -znear / (zfar - znear);
}

/* Light matrix for a sphere (centre, radius) of the world, snapped to whole
 * texels so the map does not crawl when the camera moves. */
static void light_matrix(App *a, uint32_t m, const float centre[3], float radius) {
    const float *L = a->frame.light;
    const float back = radius + 30.0f, zfar = 2.0f * radius + 60.0f;
    const float texel = 2.0f * radius / (float)a->map_size[m];
    mu_mat4 view, proj;
    mu_vec3 eye, target, up = {0.0f, 1.0f, 0.0f};
    mu_vec4 c = {centre[0], centre[1], centre[2], 1.0f}, lc;
    float   dx, dy;

    if (fabsf(L[1]) > 0.99f) { up[0] = 0.0f; up[1] = 0.0f; up[2] = 1.0f; }
    /* Snap: express the centre in light space, round x and y to texels. */
    eye[0] = L[0]; eye[1] = L[1]; eye[2] = L[2];
    target[0] = target[1] = target[2] = 0.0f;
    mu_mat4_look_at(view, eye, target, up);
    mu_mat4_mulv(lc, view, c);
    dx = floorf(lc[0] / texel + 0.5f) * texel - lc[0];
    dy = floorf(lc[1] / texel + 0.5f) * texel - lc[1];
    /* rows 0 and 1 of the view rotation are the light's right and up axes */
    target[0] = centre[0] + view[0] * dx + view[1] * dy;
    target[1] = centre[1] + view[4] * dx + view[5] * dy;
    target[2] = centre[2] + view[8] * dx + view[9] * dy;
    eye[0] = target[0] + L[0] * back;
    eye[1] = target[1] + L[1] * back;
    eye[2] = target[2] + L[2] * back;
    mu_mat4_look_at(view, eye, target, up);
    ortho_zo(proj, radius, 0.1f, zfar);
    mu_mat4_mul(proj, view, a->frame.light_vp[m]);
    a->frame.texel_world[m] = texel;
    a->frame.depth_range[m] = zfar - 0.1f;
    a->frame.inv_size[m]    = 1.0f / (float)a->map_size[m];
}

static void draw_objects(App *a, PassCtx *p, uint32_t map) {
    DrawPush push;
    uint32_t i;
    memset(&push, 0, sizeof push);
    push.frame_offset = (uint32_t)a->frame_data.offset;
    push.map          = map;
    push.technique_a  = a->technique_a;
    push.technique_b  = a->technique_b;
    push.split        = a->split;
    push.view         = a->view;
    push.flags        = a->cascades ? 1u : 0u;
    for (i = 0u; i < a->object_count; ++i) {
        push.vertex_offset = (uint32_t)a->vertices.offset + a->ranges[i].first * (uint32_t)sizeof(DemoVertex);
        push.object_offset = (uint32_t)a->object_data.offset + i * (uint32_t)sizeof(DemoObject);
        pass_push(p, &push, sizeof push);
        pass_draw(p, a->ranges[i].count, 1u, 0u, 0u);
    }
}

/* One frame: what changed since the last one, then every pass, in the order app_init listed them. */
static void app_frame(App *a) {
    Demo      *d = &a->demo;
    FrameData *f = &a->frame;
    float      view[16], eye[3], lo, hi;
    const float aspect = (float)d->width / (float)d->height;
    const float tan_y  = tanf(a->orbit.fov_deg * 0.017453292f * 0.5f), tan_x = tan_y * aspect;
    static const float origin[3] = {0.0f, 0.0f, 0.0f};
    uint32_t    i;

    demo_orbit_input(d, &a->orbit);
    demo_orbit_camera(&a->orbit, aspect, view, NULL, f->view_proj, eye);
    f->eye[0] = eye[0]; f->eye[1] = eye[1]; f->eye[2] = eye[2]; f->eye[3] = 1.0f;
    f->forward[0] = -view[2]; f->forward[1] = -view[6]; f->forward[2] = -view[10];
    f->forward[3] = (float)d->width;
    f->light[0] = cosf(a->sun_height) * sinf(a->sun_yaw);
    f->light[1] = sinf(a->sun_height);
    f->light[2] = cosf(a->sun_height) * cosf(a->sun_yaw);
    f->light[3] = a->light_size;
    f->splits[0] = a->split_depth[0]; f->splits[1] = a->split_depth[1];
    f->splits[2] = a->split_depth[2]; f->splits[3] = a->blend_width;
    f->bias[0] = a->bias_const; f->bias[1] = a->bias_slope; f->bias[2] = a->bias_normal;
    f->filter[0] = a->pcf_radius; f->filter[1] = a->search_radius;
    f->filter[2] = (float)a->pcss_rotations;

    /* Map 0 covers the whole scene; maps 1..3 each cover the bounding sphere
     * of one slice of the view frustum. */
    light_matrix(a, 0u, origin, 15.0f);
    lo = a->orbit.znear;
    for (i = 0u; i < 3u; ++i) {
        /* The sphere through the eight corners of the slice [lo, hi]: centred
         * on the view axis at depth c, where the near and far corners are
         * equally far away. */
        const float k = tan_x * tan_x + tan_y * tan_y;
        float c, radius, centre[3];
        hi = a->split_depth[i];
        c  = 0.5f * (lo + hi) * (1.0f + k);
        if (c > hi) c = hi;
        radius = sqrtf((hi - c) * (hi - c) + hi * hi * k);
        centre[0] = eye[0] + f->forward[0] * c;
        centre[1] = eye[1] + f->forward[1] * c;
        centre[2] = eye[2] + f->forward[2] * c;
        light_matrix(a, i + 1u, centre, radius);
        /* The next slice starts where this one's blend band starts. */
        lo = hi - a->blend_width;
    }
    for (i = 0u; i < MAPS; ++i) f->map[i] = a->map_index[i];
    f->samplers[0] = d->nearest_clamp_index;
    f->samplers[1] = sampler_bindless_index(d->gpu, a->cmp_nearest);
    f->samplers[2] = sampler_bindless_index(d->gpu, a->cmp_linear);
    demo_upload(d, a->frame_data, f, sizeof *f);

    if (!demo_frame_begin(d)) return;
    for (i = 0u; i < MAPS; ++i) draw_objects(a, demo_pass(d, NULL), i);   /* "Shadow0" .. "Shadow3" */
    draw_objects(a, demo_pass(d, "Forward"), 0u);
    demo_present(d);
    demo_frame_end(d);
}

static void add(App *a, DemoRange range, float x, float y, float z, float yaw, float r, float g, float b) {
    if (a->object_count >= MAX_OBJS) return;
    a->ranges[a->object_count] = range;
    demo_object(&a->objects[a->object_count++], x, y, z, yaw, 1.0f, r, g, b);
}

static void build_scene(App *a) {
    DemoMesh *m = &a->mesh;
    const DemoRange ground = demo_mesh_plane(m, 40.0f);
    add(a, ground, 0, 0, 0, 0, 0.46f, 0.46f, 0.48f);
    if (a->calibration) {
        /* One thin plate, 2 x 2, floating 2 above the ground. */
        add(a, demo_mesh_box(m, 1.0f, 0.025f, 1.0f), 0.0f, 2.0f, 0.0f, 0.0f, 0.8f, 0.4f, 0.3f);
        return;
    }
    {
        const DemoRange box    = demo_mesh_box(m, 0.6f, 0.6f, 0.6f);
        const DemoRange tall   = demo_mesh_box(m, 0.10f, 1.8f, 0.10f);
        const DemoRange slab   = demo_mesh_box(m, 1.6f, 0.06f, 1.0f);
        const DemoRange sphere = demo_mesh_sphere(m, 0.7f, 40u, 20u);
        const DemoRange torus  = demo_mesh_torus(m, 0.8f, 0.22f, 48u, 16u);
        const DemoRange fence  = demo_mesh_box(m, 0.04f, 0.9f, 0.04f);
        uint32_t i;
        add(a, box, -2.4f, 0.6f, 0.4f, 0.5f, 0.85f, 0.35f, 0.25f);
        add(a, box, -1.5f, 0.6f, -1.6f, 1.1f, 0.30f, 0.60f, 0.85f);
        add(a, sphere, 0.6f, 0.7f, 0.2f, 0.0f, 0.90f, 0.75f, 0.30f);
        add(a, torus, 2.6f, 1.1f, -0.6f, 0.4f, 0.45f, 0.80f, 0.50f);
        /* A pole: its shadow starts sharp at the base and softens with
         * distance under PCSS, and stays equally wide under plain PCF. */
        add(a, tall, 3.2f, 1.8f, 1.2f, 0.3f, 0.80f, 0.80f, 0.82f);
        /* A raised slab: a contact-free shadow, widest penumbra. */
        add(a, slab, -3.4f, 2.2f, 2.4f, 0.2f, 0.70f, 0.45f, 0.75f);
        /* A fence of thin posts: stresses resolution and bias. */
        for (i = 0u; i < 9u; ++i)
            add(a, fence, -4.0f + 0.5f * (float)i, 0.9f, -3.4f, 0.0f, 0.55f, 0.42f, 0.30f);
        /* Distant boxes, so the far cascades have something to resolve. */
        for (i = 0u; i < 14u; ++i)
            add(a, box, -12.0f + 2.0f * (float)i + (i % 3u) * 0.4f, 0.6f, -7.0f - 2.2f * (float)(i % 5u),
                0.37f * (float)i, 0.5f + 0.03f * (float)(i % 7u), 0.52f, 0.48f + 0.04f * (float)(i % 4u));
    }
}

static void app_init(App *a) {
    Demo       *d = &a->demo;
    ShaderId    shadow_vs, main_vs, main_fs;
    SamplerDesc sd;
    PassDesc    passes[MAPS + 3u];
    uint32_t    count = 0u, i;
    char        name[MAX_PASS_NAME];

    if (a->no_bias) a->bias_const = a->bias_slope = a->bias_normal = 0.0f;

    build_scene(a);
    a->vertices    = demo_alloc(d, a->mesh.vertices, (uint64_t)a->mesh.count * sizeof(DemoVertex));
    a->object_data = demo_alloc(d, a->objects, (uint64_t)a->object_count * sizeof(DemoObject));
    a->frame_data  = demo_alloc(d, NULL, sizeof(FrameData));

    for (i = 0u; i < MAPS; ++i) {
        a->map_size[i] = i ? 1024u : 2048u;
        snprintf(name, sizeof name, "shadow_map%u", i);
        a->maps[i] = demo_target(d, name, FMT_D32F, a->map_size[i], a->map_size[i], false, &a->map_index[i]);
    }
    a->hdr   = demo_target(d, "shadows_hdr", FMT_RGBA16F, d->width, d->height, false, NULL);
    a->depth = demo_target(d, "shadows_depth", FMT_D32F, d->width, d->height, false, NULL);

    /* Comparison samplers: the fetch returns the result of `ref <= texel`,
     * and with a linear filter the hardware blends four such results. */
    sd = sampler_linear_clamp();
    sd.compare = CMP_LEQUAL;
    sd.mip_filter = FILT_NEAREST;
    a->cmp_linear = sampler_create(d->gpu, &sd);
    sd.min_filter = sd.mag_filter = FILT_NEAREST;
    a->cmp_nearest = sampler_create(d->gpu, &sd);

    shadow_vs = demo_shader(d, SHADERS "shadow.vert.spv", STAGE_VERTEX);
    main_vs   = demo_shader(d, SHADERS "main.vert.spv", STAGE_VERTEX);
    main_fs   = demo_shader(d, SHADERS "main.frag.spv", STAGE_FRAGMENT);
    a->shadow_pipe  = pipe_intern_depth_only(d->gpu, shadow_vs, SHADER_NONE, FMT_D32F);
    a->forward_pipe = pipe_intern_graphics(d->gpu, main_vs, main_fs, FMT_RGBA16F, FMT_D32F, BLEND_OPAQUE);

    a->reads[0] = use_read(a->vertices.buffer, a->vertices.offset, a->vertices.size);
    a->reads[1] = use_read(a->object_data.buffer, a->object_data.offset, a->object_data.size);
    a->reads[2] = use_read(a->frame_data.buffer, a->frame_data.offset, a->frame_data.size);
    for (i = 0u; i < 3u; ++i) a->reads[i].stages = SH_VERT | SH_FRAG;

    memset(passes, 0, sizeof passes);
    for (i = 0u; i < MAPS; ++i) {
        PassDesc *p = &passes[count++];
        a->map_depth[i] = attach_depth_clear(a->maps[i], 1.0f);
        snprintf(p->name, sizeof p->name, "Shadow%u", i);
        p->kind  = PASS_GRAPHICS;
        p->pipe  = a->shadow_pipe;
        /* Both faces cast: thin geometry (the plate, the posts) stays closed. */
        p->state = draw_state_opaque(SC_1, CULL_NONE);
        p->state.depth_cmp   = CMP_LESS;
        p->state.depth_write = 1u;
        p->depth          = &a->map_depth[i];
        p->buf_reads      = a->reads;
        p->buf_read_count = 3u;
    }
    {
        PassDesc *p = &passes[count++];
        a->hdr_color = attach_clear(a->hdr, 0.36f, 0.48f, 0.68f, 0.0f);
        a->hdr_depth = attach_depth_clear(a->depth, 1.0f);
        strcpy(p->name, "Forward");
        p->kind  = PASS_GRAPHICS;
        p->pipe  = a->forward_pipe;
        p->state = draw_state_opaque(SC_1, CULL_BACK);
        p->state.depth_cmp   = CMP_LESS;
        p->state.depth_write = 1u;
        p->colors         = &a->hdr_color;
        p->color_count    = 1u;
        p->depth          = &a->hdr_depth;
        p->tex_reads      = a->maps;
        p->tex_read_count = MAPS;
        p->buf_reads      = a->reads;
        p->buf_read_count = 3u;
    }
    passes[count++] = demo_present_pass(d, a->hdr);
    d->program = demo_program(d, passes, count);
}

static void app_shutdown(App *a) { demo_mesh_free(&a->mesh); }

static void app_ui(App *a, struct nk_context *ctx) {
    Demo *d = &a->demo;
    if (nk_begin(ctx, "Shadows", nk_rect(10, 10, 330, 520),
                 NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_TITLE)) {
        float rot = (float)a->pcss_rotations;
        demo_ui_text(ctx, "%.1f ms   %u objects", (double)d->dt * 1000.0, a->object_count);
        demo_ui_choice(ctx, "Left", k_techniques, 6u, &a->technique_a);
        demo_ui_choice(ctx, "Right", k_techniques, 6u, &a->technique_b);
        demo_ui_float(ctx, "Split", &a->split, 0.0f, 1.0f);
        demo_ui_choice(ctx, "View", k_views, 3u, &a->view);
        demo_ui_bool(ctx, "Cascades (3 x 1024)", &a->cascades);
        demo_ui_float(ctx, "Cascade blend", &a->blend_width, 0.1f, 6.0f);
        demo_ui_float(ctx, "PCF radius (texels)", &a->pcf_radius, 0.5f, 12.0f);
        demo_ui_float(ctx, "Light size", &a->light_size, 0.0f, 0.3f);
        demo_ui_float(ctx, "Blocker search", &a->search_radius, 1.0f, 32.0f);
        demo_ui_float(ctx, "PCSS rotations", &rot, 1.0f, 8.0f);
        demo_ui_float(ctx, "Bias const (texels)", &a->bias_const, 0.0f, 4.0f);
        demo_ui_float(ctx, "Bias slope", &a->bias_slope, 0.0f, 3.0f);
        demo_ui_float(ctx, "Normal offset (texels)", &a->bias_normal, 0.0f, 6.0f);
        demo_ui_float(ctx, "Sun yaw", &a->sun_yaw, -3.14f, 3.14f);
        demo_ui_float(ctx, "Sun height", &a->sun_height, 0.1f, 1.55f);
        a->pcss_rotations = (uint32_t)rot;
    }
    nk_end(ctx);
}

/* ---- acceptance ----------------------------------------------------------- */

/* One frame, then the HDR target as floats. */
static float *render_data(App *a) {
    app_frame(a);
    return demo_read_floats(&a->demo, a->hdr, (size_t)a->demo.width * a->demo.height * 4u);
}

/* x at which a rising profile crosses `level`, by linear interpolation. */
static double crossing(const double *profile, const double *xs, uint32_t n, double level) {
    uint32_t i;
    for (i = 1u; i < n; ++i)
        if (profile[i - 1u] < level && profile[i] >= level)
            return xs[i - 1u] + (xs[i] - xs[i - 1u]) * (level - profile[i - 1u]) / (profile[i] - profile[i - 1u]);
    return NAN;
}

static void app_check(App *a) {
    Demo  *d = &a->demo;
    const size_t pixels = (size_t)d->width * d->height;
    float *px;
    size_t i;

    if (!a->calibration) demo_fatal(d, "--check needs the calibration scene; run with --calibration --check");

    /* ---- 1. PCSS penumbra width under the plate --------------------------- */
    {
        enum { BINS = 120 };
        double sum[BINS] = {0}, cnt[BINS] = {0}, profile[BINS], xs[BINS];
        const double x0 = 0.4, x1 = 1.6;
        double lit_min = 1.0, x10, x50, x90, measured, expected, top, distance, zc;
        uint32_t n = 0u, b;

        a->technique_a = a->technique_b = 5u;
        a->view = 3u;
        a->cascades = false;
        a->sun_yaw = 0.0f;       /* light leans along +z only: the plate's x edges cast straight */
        a->sun_height = 1.2f;
        a->light_size = 0.08f;
        a->search_radius = 10.0f;
        a->pcss_rotations = 8u;
        top      = 2.025;
        distance = top / sin(1.2);
        expected = 0.687 * distance * 0.08;
        zc       = -top / tan(1.2); /* z of the shadow's centre line */
        /* Seen from the +x side and from low down, so that the plate does
         * not hide the shadow edge it casts. */
        a->orbit = (DemoOrbit){.target = {1.2f, 0.0f, (float)zc}, .distance = 5.0f, .yaw = 1.5708f, .pitch = 0.5f,
                               .fov_deg = 40.0f, .znear = 0.1f, .zfar = 80.0f};
        px = render_data(a);
        /* The blocker is the plate's top face. Along the light direction it
         * is `distance` away from the ground; a light of angular diameter s
         * smears its edge over W = distance * s. PCSS filters with a disc of
         * diameter W, and a straight edge swept across a disc goes from 10 %
         * to 90 % coverage over 0.687 of its diameter. */
        for (i = 0u; i < pixels; ++i) {
            const double s = px[i * 4u], x = px[i * 4u + 1u], z = px[i * 4u + 2u];
            if (px[i * 4u + 3u] < 0.5f) continue;
            if (x > 2.0 && s < lit_min) lit_min = s;
            if (fabs(z - zc) > 0.35 || x < x0 || x >= x1) continue;
            b = (uint32_t)((x - x0) / (x1 - x0) * BINS);
            sum[b] += s; cnt[b] += 1.0;
        }
        for (b = 0u; b < BINS; ++b) {
            if (cnt[b] < 1.0) continue;
            profile[n] = sum[b] / cnt[b];
            xs[n++]    = x0 + ((double)b + 0.5) * (x1 - x0) / BINS;
        }
        x10 = crossing(profile, xs, n, 0.1);
        x50 = crossing(profile, xs, n, 0.5);
        x90 = crossing(profile, xs, n, 0.9);
        measured = x90 - x10;
        demo_check(d, "PCSS edge position (plate edge x=1)", x50, "1 +- 0.03", fabs(x50 - 1.0) <= 0.03);
        demo_check(d, "PCSS penumbra 10-90 % width", measured, "predicted, next line", measured > 0.0);
        demo_check(d, "  measured / predicted", measured / expected, "1 +- 0.15",
                         fabs(measured / expected - 1.0) <= 0.15);
        demo_check(d, "PCSS: lowest factor on open ground", lit_min, "== 1 (no acne)", lit_min == 1.0);
        demo_check(d, "profile bins with samples", (double)n, ">= 100", n >= 100u);
        free(px);
    }

    /* ---- 2. a hard tap is binary; bias removes acne, and only bias -------- */
    {
        static const char *const k_label[3] = {"hard tap", "PCF 3x3 bilinear", "Poisson 16"};
        static const uint32_t    k_tech[3]  = {0u, 3u, 4u};
        const float bc = a->bias_const, bs = a->bias_slope, bn = a->bias_normal;
        uint32_t t, pass;
        char     label[96];
        a->sun_height = 0.6f; /* a low sun: the hard case for acne */
        for (pass = 0u; pass < 2u; ++pass) {
            for (t = 0u; t < 3u; ++t) {
                size_t grey = 0u, open = 0u, dark = 0u;
                a->technique_a = a->technique_b = k_tech[t];
                px = render_data(a);
                for (i = 0u; i < pixels; ++i) {
                    const float s = px[i * 4u];
                    if (px[i * 4u + 3u] < 0.5f) continue;
                    if (s != 0.0f && s != 1.0f) ++grey;
                    /* the low sun throws the shadow to -z; +x beyond 2.6 stays open */
                    if (px[i * 4u + 1u] > 2.6f) { ++open; if (s < 1.0f) ++dark; }
                }
                free(px);
                if (pass == 0u) {
                    if (t == 0u) demo_check(d, "hard tap: pixels neither 0 nor 1", (double)grey, "== 0", grey == 0u);
                    snprintf(label, sizeof label, "%s: darkened open-ground pixels", k_label[t]);
                    demo_check(d, label, (double)dark, "== 0", open > 1000u && dark == 0u);
                } else {
                    snprintf(label, sizeof label, "%s, bias off: darkened fraction", k_label[t]);
                    demo_check(d, label, open ? (double)dark / (double)open : 0.0, "> 0.05 (control)",
                                     open > 1000u && (double)dark / (double)open > 0.05);
                }
            }
            a->bias_const = a->bias_slope = a->bias_normal = 0.0f;
        }
        a->bias_const = bc; a->bias_slope = bs; a->bias_normal = bn;
    }

    /* ---- 3. neighbouring cascades agree inside the blend band ------------- */
    {
        double diff = 0.0, band = 0.0, far_off = 0.0;
        a->technique_a = a->technique_b = 3u;
        a->view = 4u;
        a->cascades = true;
        a->sun_yaw = 0.7f;
        a->sun_height = 0.9f;
        a->orbit = (DemoOrbit){.target = {0.0f, 0.5f, 0.0f}, .distance = 7.0f, .yaw = 0.5f, .pitch = 0.25f,
                               .fov_deg = 50.0f, .znear = 0.1f, .zfar = 80.0f};
        px = render_data(a);
        for (i = 0u; i < pixels; ++i) {
            double e;
            if (px[i * 4u + 3u] < 0.5f) continue;
            e = fabs((double)px[i * 4u] - (double)px[i * 4u + 1u]);
            diff += e; band += 1.0;
            if (e > 0.5) far_off += 1.0;
        }
        demo_check(d, "pixels inside cascade blend bands", band, "> 1000", band > 1000.0);
        demo_check(d, "mean |cascade k - cascade k+1|", band > 0.0 ? diff / band : 1.0, "< 0.02",
                         band > 0.0 && diff / band < 0.02);
        demo_check(d, "band pixels differing by > 0.5", band > 0.0 ? far_off / band : 1.0, "< 1 %",
                         band > 0.0 && far_off / band < 0.01);
        free(px);
    }
}

int main(int argc, char **argv) {
    static App app;
    static const DemoOption options[] = {
        {"--technique", .u32 = &app.technique_a, .lo = 0, .hi = 5, .help = "left filter: 0 hard, 1 PCF 3x3 hard taps, 2 hardware bilinear, 3 PCF 3x3 bilinear taps, 4 Poisson 16, 5 PCSS"},
        {"--right", .u32 = &app.technique_b, .lo = 0, .hi = 5, .help = "right filter"},
        {"--split", .f32 = &app.split, .lo = 0, .hi = 1, .help = "where the divider between the two is"},
        {"--cascades", .flag = &app.cascades, .help = "three 1024^2 cascades instead of the single 2048^2 map"},
        {"--view", .u32 = &app.view, .lo = 0, .hi = 2, .help = "0 shaded, 1 shadow factor, 2 cascade tint"},
        {"--light-size", .f32 = &app.light_size, .lo = 0, .hi = 1, .help = "angular diameter of the sun (tan), for PCSS"},
        {"--calibration", .flag = &app.calibration, .help = "one floating plate; required by --check"},
        {"--no-bias", .flag = &app.no_bias, .help = "all three biases off (shows acne)"},
        {0}};
    demo_defaults(&app.demo, "shadows", "Shadows - filtering lab (mu_lib shadows.slang)");
    app.orbit = (DemoOrbit){.target = {0.0f, 0.8f, 0.0f}, .distance = 9.5f, .yaw = 0.55f, .pitch = 0.42f,
                            .fov_deg = 50.0f, .znear = 0.1f, .zfar = 80.0f,
                            .min_distance = 2.0f, .max_distance = 40.0f};
    app.technique_a   = 3u;
    app.technique_b   = 5u;
    app.split         = 0.5f;
    app.sun_yaw       = 0.7f;
    app.sun_height    = 0.9f;
    app.light_size    = 0.06f;
    app.pcf_radius    = 2.5f;
    app.search_radius = 8.0f;
    app.pcss_rotations = 2u;
    app.bias_const    = 0.5f;
    app.bias_slope    = 1.0f;
    app.bias_normal   = 1.5f;
    app.blend_width   = 1.5f;
    app.split_depth[0] = 7.0f;
    app.split_depth[1] = 18.0f;
    app.split_depth[2] = 45.0f;
    demo_open(&app.demo, options, argc, argv);
    app_init(&app);
    while (demo_running(&app.demo)) {
        if (app.demo.ui) app_ui(&app, app.demo.ui);
        app_frame(&app);
    }
    if (app.demo.check) app_check(&app);
    app_shutdown(&app);
    return demo_close(&app.demo);
}
