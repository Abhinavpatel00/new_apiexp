/* pbr — metallic/roughness shading with image-based lighting.
 *
 * Baked only when the sky changes (compute, storage images):
 *
 *   Sky -> Prefilter1..4 (GGX, one target per roughness) -> Irradiance,  DFG
 *
 * Every frame:
 *
 *   Background (sky or a prefiltered level) -> Forward (35 spheres) -> Present
 *
 * Two frame programs: one with the bake passes in front, one without. The
 * environment is octahedral - vk.h has no cube targets.
 *
 * --check is a white-furnace test (a white sphere in a white room must look
 * exactly as bright as the room) plus an independent integration of the DFG
 * table on the CPU.
 */
#include "demo.h"
#include "mesh.h"

#include "mu_math.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SHADERS    "demos/pbr/shaders/"
#define LEVELS     5u /* environment roughness levels, 0 = the sky itself */
#define COLS       7u
#define ROWS       5u
#define SPHERES    (COLS * ROWS)
#define IRR_SIZE   32u
#define DFG_SIZE   64u
#define PI         3.14159265358979323846

static const uint32_t k_level_size[LEVELS] = {256u, 128u, 64u, 32u, 32u};
static const char *const k_energy[] = {"single scattering", "multiple scattering", "mu_pbr_indirect as is"};
static const char *const k_dfg[]    = {"baked table", "mu_env_brdf_approx"};
static const char *const k_views[]  = {"shaded", "specular IBL", "diffuse IBL", "sun only"};

typedef struct FrameData {
    float    view_proj[16], eye[4], sun[4], right[4], up[4], forward[4];
    uint32_t level0123[4], maps[4], options[4];
    float    size[4];
} FrameData;
_Static_assert(sizeof(FrameData) == 208, "FrameData must match pbr.slang");

typedef struct DrawPush { uint32_t vertex_offset, object_offset, frame_offset, object_id; } DrawPush;

typedef struct BakePush {
    uint32_t source, sampler, target, size;
    float    roughness;
    uint32_t samples, mode, pad;
    float    sun[4];
} BakePush;
_Static_assert(sizeof(BakePush) == 48, "BakePush must match bake.slang");

typedef struct App {
    Demo         demo; /* first: see demo.h */
    DemoOrbit    orbit;
    DemoMesh     mesh;
    DemoRange    sphere;
    DemoObject   objects[SPHERES];
    BufSlice     vertices, object_data, frame_data;
    Target       level[LEVELS], irradiance, dfg, hdr, depth;
    uint32_t     level_index[LEVELS], irradiance_index, dfg_index;
    Target       forward_reads[LEVELS + 2u];
    Pipe         sky_pipe, prefilter_pipe, irradiance_pipe, dfg_pipe, background_pipe, forward_pipe;
    AttachDesc   background_color, forward_color, forward_depth;
    BufUse       reads[3];
    FrameProgram *bake_program, *frame_program;
    FrameData    frame;
    /* parameters */
    float    sun_yaw, sun_height, sun_power, background;
    uint32_t energy, dfg_source, view, env_mode;
    bool     furnace, show_maps;
    /* what the baked maps were built from */
    float    baked_yaw, baked_height;
    uint32_t baked_mode;
    bool     baked;
} App;

static void sun_direction(const App *a, float out[4]) {
    out[0] = cosf(a->sun_height) * sinf(a->sun_yaw);
    out[1] = sinf(a->sun_height);
    out[2] = cosf(a->sun_height) * cosf(a->sun_yaw);
    out[3] = 0.0f;
}

/* ---- bake passes ---------------------------------------------------------- */

static void bake(App *a, PassCtx *p, uint32_t target, uint32_t size, float roughness, uint32_t samples) {
    BakePush push;
    memset(&push, 0, sizeof push);
    push.source    = a->level_index[0];
    push.sampler   = a->demo.linear_clamp_index;
    push.target    = target;
    push.size      = size;
    push.roughness = roughness;
    push.samples   = samples;
    push.mode      = a->env_mode;
    sun_direction(a, push.sun);
    pass_push(p, &push, sizeof push);
    pass_dispatch(p, (size + 7u) / 8u, (size + 7u) / 8u, 1u);
}

/* ---- frame passes --------------------------------------------------------- */

/* One frame: what changed since the last one, then every pass, in the order app_init listed them. */
static void app_frame(App *a) {
    Demo      *d = &a->demo;
    FrameData *f = &a->frame;
    float      view[16], eye[3];
    const float aspect = (float)d->width / (float)d->height;
    const float tan_y  = tanf(a->orbit.fov_deg * 0.017453292f * 0.5f);
    uint32_t    i;
    PassCtx *pass;

    demo_orbit_input(d, &a->orbit);
    demo_orbit_camera(&a->orbit, aspect, view, NULL, f->view_proj, eye);
    for (i = 0u; i < 3u; ++i) {
        f->eye[i]     = eye[i];
        f->right[i]   = view[i * 4u] * tan_y * aspect;
        f->up[i]      = view[i * 4u + 1u] * tan_y;
        f->forward[i] = -view[i * 4u + 2u];
    }
    f->forward[3] = a->background;
    sun_direction(a, f->sun);
    f->sun[3] = a->furnace ? 0.0f : a->sun_power;
    for (i = 0u; i < 4u; ++i) f->level0123[i] = a->level_index[i];
    f->maps[0]    = a->level_index[4];
    f->maps[1]    = a->irradiance_index;
    f->maps[2]    = a->dfg_index;
    f->maps[3]    = d->linear_clamp_index;
    f->options[0] = a->energy;
    f->options[1] = a->dfg_source;
    f->options[2] = a->view;
    f->options[3] = (a->furnace ? 1u : 0u) | (a->show_maps ? 2u : 0u);
    f->size[0]    = (float)d->width;
    f->size[1]    = (float)d->height;
    f->size[2]    = (float)a->env_mode;
    demo_upload(d, a->frame_data, f, sizeof *f);

    /* Bake again only when the environment changed. */
    if (!a->baked || a->baked_yaw != a->sun_yaw || a->baked_height != a->sun_height ||
        a->baked_mode != a->env_mode) {
        d->program      = a->bake_program;
        a->baked        = true;
        a->baked_yaw    = a->sun_yaw;
        a->baked_height = a->sun_height;
        a->baked_mode   = a->env_mode;
    } else {
        d->program = a->frame_program;
    }

    if (!demo_frame_begin(d)) return;
    if (d->program == a->bake_program) {
        /* The sky, four blurrier copies of it, its irradiance, and the DFG table. */
        uint32_t level;
        for (level = 0u; level < LEVELS; ++level)   /* "Sky", "Prefilter1" .. "Prefilter4" */
            bake(a, demo_pass(d, NULL), a->level_index[level], k_level_size[level], (float)level / (float)(LEVELS - 1u), 256u);
        bake(a, demo_pass(d, "Irradiance"), a->irradiance_index, IRR_SIZE, 0.0f, 1024u);
        bake(a, demo_pass(d, "DFG"), a->dfg_index, DFG_SIZE, 0.0f, 1024u);
    }
    pass = demo_pass(d, "Background");
    {
        DrawPush push;
        memset(&push, 0, sizeof push);
        push.frame_offset = (uint32_t)a->frame_data.offset;
        pass_push(pass, &push, sizeof push);
        pass_draw_fullscreen_triangle(pass, d->width, d->height);
    }
    pass = demo_pass(d, "Forward");
    {
        DrawPush push;
        uint32_t i;
        push.vertex_offset = (uint32_t)a->vertices.offset + a->sphere.first * (uint32_t)sizeof(DemoVertex);
        push.frame_offset  = (uint32_t)a->frame_data.offset;
        for (i = 0u; i < SPHERES; ++i) {
            push.object_offset = (uint32_t)a->object_data.offset + i * (uint32_t)sizeof(DemoObject);
            push.object_id     = i;
            pass_push(pass, &push, sizeof push);
            pass_draw(pass, a->sphere.count, 1u, 0u, 0u);
        }
    }
    demo_present(d);
    demo_frame_end(d);
}

static void compute_image_pass(PassDesc *p, const char *name, Pipe pipe, const Target *read, const Target *write) {
    memset(p, 0, sizeof *p);
    strcpy(p->name, name);
    p->kind            = PASS_COMPUTE;
    p->pipe            = pipe;
    p->state           = draw_state_opaque(SC_1, CULL_NONE);
    p->tex_reads       = read;
    p->tex_read_count  = read ? 1u : 0u;
    p->tex_writes      = write;
    p->tex_write_count = 1u;
}

/* Row 0 is the top row. Columns run from smooth (left) to rough (right). */
static void build_scene(App *a) {
    static const float k_color[ROWS][3] = {
        {1.00f, 0.77f, 0.34f},  /* metal: gold                    */
        {1.00f, 0.77f, 0.34f},  /* half metal                     */
        {1.00f, 0.77f, 0.34f},  /* dielectric of the same colour  */
        {0.55f, 0.03f, 0.03f},  /* clearcoat over red paint       */
        {0.05f, 0.08f, 0.30f}}; /* sheen over blue cloth          */
    static const float k_metal[ROWS] = {1.0f, 0.5f, 0.0f, 0.0f, 0.0f};
    uint32_t row, col;
    a->sphere = demo_mesh_sphere(&a->mesh, 0.43f, 64u, 32u);
    for (row = 0u; row < ROWS; ++row) {
        for (col = 0u; col < COLS; ++col) {
            DemoObject *o = &a->objects[row * COLS + col];
            demo_object(o, ((float)col - 3.0f) * 1.0f, (2.0f - (float)row) * 1.0f, 0.0f, 0.0f, 1.0f,
                        k_color[row][0], k_color[row][1], k_color[row][2]);
            o->params[0] = 0.05f + 0.95f * (float)col / (float)(COLS - 1u);
            o->params[1] = k_metal[row];
            o->params[2] = row == 3u ? 1.0f : 0.0f;
            o->params[3] = row == 4u ? 1.0f : 0.0f;
        }
    }
}

static void app_init(App *a) {
    Demo    *d = &a->demo;
    ShaderId sky_cs, prefilter_cs, irradiance_cs, dfg_cs, sky_fs, main_vs, main_fs;
    PassDesc passes[12];
    uint32_t count = 0u, first_frame_pass, i;
    char     name[MAX_PASS_NAME];

    build_scene(a);
    a->vertices    = demo_alloc(d, a->mesh.vertices, (uint64_t)a->mesh.count * sizeof(DemoVertex));
    a->object_data = demo_alloc(d, a->objects, sizeof a->objects);
    a->frame_data  = demo_alloc(d, NULL, sizeof(FrameData));

    /* Everything a compute pass writes is RGBA16F: bake.slang declares the
     * storage-image table with that one format. */
    for (i = 0u; i < LEVELS; ++i) {
        snprintf(name, sizeof name, "env_level%u", i);
        a->level[i] = demo_target(d, name, FMT_RGBA16F, k_level_size[i], k_level_size[i], true, &a->level_index[i]);
    }
    a->irradiance = demo_target(d, "env_irradiance", FMT_RGBA16F, IRR_SIZE, IRR_SIZE, true, &a->irradiance_index);
    a->dfg        = demo_target(d, "dfg", FMT_RGBA16F, DFG_SIZE, DFG_SIZE, true, &a->dfg_index);
    a->hdr        = demo_target(d, "pbr_hdr", FMT_RGBA16F, d->width, d->height, false, NULL);
    a->depth      = demo_target(d, "pbr_depth", FMT_D32F, d->width, d->height, false, NULL);

    sky_cs        = demo_shader(d, SHADERS "sky.comp.spv", STAGE_COMPUTE);
    prefilter_cs  = demo_shader(d, SHADERS "prefilter.comp.spv", STAGE_COMPUTE);
    irradiance_cs = demo_shader(d, SHADERS "irradiance.comp.spv", STAGE_COMPUTE);
    dfg_cs        = demo_shader(d, SHADERS "dfg.comp.spv", STAGE_COMPUTE);
    sky_fs        = demo_shader(d, SHADERS "sky.frag.spv", STAGE_FRAGMENT);
    main_vs       = demo_shader(d, SHADERS "main.vert.spv", STAGE_VERTEX);
    main_fs       = demo_shader(d, SHADERS "main.frag.spv", STAGE_FRAGMENT);
    a->sky_pipe        = pipe_intern_compute(d->gpu, sky_cs, "main");
    a->prefilter_pipe  = pipe_intern_compute(d->gpu, prefilter_cs, "main");
    a->irradiance_pipe = pipe_intern_compute(d->gpu, irradiance_cs, "main");
    a->dfg_pipe        = pipe_intern_compute(d->gpu, dfg_cs, "main");
    a->background_pipe = pipe_intern_graphics(d->gpu, d->fullscreen_vs, sky_fs, FMT_RGBA16F, FMT_NONE, BLEND_OPAQUE);
    a->forward_pipe    = pipe_intern_graphics(d->gpu, main_vs, main_fs, FMT_RGBA16F, FMT_D32F, BLEND_OPAQUE);

    a->reads[0] = use_read(a->vertices.buffer, a->vertices.offset, a->vertices.size);
    a->reads[1] = use_read(a->object_data.buffer, a->object_data.offset, a->object_data.size);
    a->reads[2] = use_read(a->frame_data.buffer, a->frame_data.offset, a->frame_data.size);
    for (i = 0u; i < 3u; ++i) a->reads[i].stages = SH_VERT | SH_FRAG;
    for (i = 0u; i < LEVELS; ++i) a->forward_reads[i] = a->level[i];
    a->forward_reads[LEVELS]      = a->irradiance;
    a->forward_reads[LEVELS + 1u] = a->dfg;

    /* ---- bake: sky, four prefiltered levels, irradiance, DFG ---- */
    compute_image_pass(&passes[count++], "Sky", a->sky_pipe, NULL, &a->level[0]);
    for (i = 1u; i < LEVELS; ++i) {
        snprintf(name, sizeof name, "Prefilter%u", i);
        compute_image_pass(&passes[count++], name, a->prefilter_pipe, &a->level[0], &a->level[i]);
    }
    compute_image_pass(&passes[count++], "Irradiance", a->irradiance_pipe, &a->level[0], &a->irradiance);
    compute_image_pass(&passes[count++], "DFG", a->dfg_pipe, NULL, &a->dfg);

    /* ---- frame ---- */
    first_frame_pass = count;
    {
        PassDesc *p = &passes[count++];
        memset(p, 0, sizeof *p);
        a->background_color = attach_color(a->hdr, ATT_WRITE);
        strcpy(p->name, "Background");
        p->kind           = PASS_GRAPHICS;
        p->pipe           = a->background_pipe;
        p->state          = draw_state_opaque(SC_1, CULL_NONE);
        p->colors         = &a->background_color;
        p->color_count    = 1u;
        p->tex_reads      = a->forward_reads;
        p->tex_read_count = LEVELS + 2u;
        p->buf_reads      = &a->reads[2];
        p->buf_read_count = 1u;
    }
    {
        PassDesc *p = &passes[count++];
        memset(p, 0, sizeof *p);
        a->forward_color = attach_color(a->hdr, ATT_WRITE_KEEP);
        a->forward_depth = attach_depth_clear(a->depth, 1.0f);
        strcpy(p->name, "Forward");
        p->kind  = PASS_GRAPHICS;
        p->pipe  = a->forward_pipe;
        p->state = draw_state_opaque(SC_1, CULL_BACK);
        p->state.depth_cmp   = CMP_LESS;
        p->state.depth_write = 1u;
        p->colors         = &a->forward_color;
        p->color_count    = 1u;
        p->depth          = &a->forward_depth;
        p->tex_reads      = a->forward_reads;
        p->tex_read_count = LEVELS + 2u;
        p->buf_reads      = a->reads;
        p->buf_read_count = 3u;
    }
    passes[count++] = demo_present_pass(d, a->hdr);

    a->bake_program  = demo_program(d, passes, count);
    a->frame_program = demo_program(d, &passes[first_frame_pass], count - first_frame_pass);
    d->program = a->bake_program; /* app_frame() picks one of the two every frame */
    if (a->furnace) a->env_mode = 1u;
}

static void app_shutdown(App *a) {
    Demo *d = &a->demo;
    /* The framework destroys d->program; the other one is ours. */
    FrameProgram *other = d->program == a->bake_program ? a->frame_program : a->bake_program;
    if (other && other != d->program) frame_program_destroy(d->gpu, other);
    demo_mesh_free(&a->mesh);
}

static void app_ui(App *a, struct nk_context *ctx) {
    Demo *d = &a->demo;
    if (nk_begin(ctx, "PBR", nk_rect(10, 10, 320, 330),
                 NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_TITLE)) {
        demo_ui_text(ctx, "%.1f ms   %s", (double)d->dt * 1000.0,
                     d->program == a->bake_program ? "baking" : "baked maps reused");
        demo_ui_choice(ctx, "View", k_views, 4u, &a->view);
        demo_ui_choice(ctx, "Energy", k_energy, 3u, &a->energy);
        demo_ui_choice(ctx, "DFG", k_dfg, 2u, &a->dfg_source);
        demo_ui_float(ctx, "Background blur", &a->background, 0.0f, 4.0f);
        demo_ui_float(ctx, "Sun yaw", &a->sun_yaw, -3.14f, 3.14f);
        demo_ui_float(ctx, "Sun height", &a->sun_height, 0.02f, 1.5f);
        demo_ui_float(ctx, "Sun power", &a->sun_power, 0.0f, 8.0f);
        demo_ui_bool(ctx, "Show baked maps", &a->show_maps);
        demo_ui_bool(ctx, "White furnace", &a->furnace);
        a->env_mode = a->furnace ? 1u : 0u;
    }
    nk_end(ctx);
}

/* ---- acceptance ----------------------------------------------------------- */

/* Render one frame and read `target` back as floats (4 per texel). */
/* One frame, then a half-float target as floats. */
static float *render_and_read(App *a, Target target, uint32_t width, uint32_t height) {
    app_frame(a);
    return demo_read_floats(&a->demo, target, (size_t)width * height * 4u);
}

/* The DFG integral by brute force: a midpoint rule over the hemisphere of
 * light directions, no importance sampling, in double precision. Shares
 * nothing with compDfg but the BRDF's definition. */
static void dfg_reference(double nov, double roughness, double out[2]) {
    enum { NT = 600, NP = 600 };
    const double alpha = fmax(roughness * roughness, 1e-4), a2 = alpha * alpha;
    const double vx = sqrt(1.0 - nov * nov), vz = nov;
    double scale = 0.0, bias = 0.0;
    int    it, ip;
    for (it = 0; it < NT; ++it) {
        const double theta = ((double)it + 0.5) * (0.5 * PI / NT), st = sin(theta), nol = cos(theta);
        for (ip = 0; ip < NP; ++ip) { /* half the azimuth; the other half mirrors it */
            const double phi = ((double)ip + 0.5) * (PI / NP);
            const double lx = st * cos(phi), ly = st * sin(phi), lz = nol;
            const double hx = vx + lx, hy = ly, hz = vz + lz, hl = sqrt(hx * hx + hy * hy + hz * hz);
            const double noh = hz / hl, voh = (vx * hx + vz * hz) / hl;
            const double dd  = noh * noh * (a2 - 1.0) + 1.0;
            const double D   = a2 / (PI * dd * dd);
            const double V   = 0.5 / (nol * sqrt(nov * nov * (1.0 - a2) + a2) + nov * sqrt(nol * nol * (1.0 - a2) + a2));
            const double w   = D * V * nol * st * (0.5 * PI / NT) * (PI / NP) * 2.0;
            const double fc  = pow(1.0 - voh, 5.0);
            scale += (1.0 - fc) * w;
            bias  += fc * w;
        }
    }
    out[0] = scale;
    out[1] = bias;
}

static void oct_decode(double u, double v, double d[3]) {
    const double px = u * 2.0 - 1.0, py = v * 2.0 - 1.0;
    double x = px, y = 1.0 - fabs(px) - fabs(py), z = py, t = y < 0.0 ? -y : 0.0, l;
    x += x >= 0.0 ? -t : t;
    z += z >= 0.0 ? -t : t;
    l = sqrt(x * x + y * y + z * z);
    d[0] = x / l; d[1] = y / l; d[2] = z / l;
}

/* Mean and worst value per sphere of a view-4 image (alpha = sphere + 1). */
typedef struct Furnace { double lo_mean, hi_mean, lo_pixel, hi_pixel; uint32_t empty; } Furnace;

static Furnace furnace_stats(const float *px, size_t pixels, uint32_t first, uint32_t last) {
    double   sum[SPHERES] = {0}, n[SPHERES] = {0};
    Furnace  r = {1e9, -1e9, 1e9, -1e9, 0u};
    size_t   i;
    uint32_t s;
    for (i = 0u; i < pixels; ++i) {
        const float *p = &px[i * 4u];
        double v;
        if (p[3] < 0.5f) continue;
        s = (uint32_t)(p[3] + 0.5f) - 1u;
        if (s < first || s > last || s >= SPHERES) continue;
        v = ((double)p[0] + p[1] + p[2]) / 3.0;
        sum[s] += v; n[s] += 1.0;
        if (v < r.lo_pixel) r.lo_pixel = v;
        if (v > r.hi_pixel) r.hi_pixel = v;
    }
    for (s = first; s <= last; ++s) {
        double m;
        if (n[s] < 500.0) { ++r.empty; continue; }
        m = sum[s] / n[s];
        if (m < r.lo_mean) r.lo_mean = m;
        if (m > r.hi_mean) r.hi_mean = m;
    }
    return r;
}

static void app_check(App *a) {
    Demo  *d = &a->demo;
    const size_t pixels = (size_t)d->width * d->height;
    float *px;
    char   label[96];
    uint32_t i;

    /* ---- 1. the DFG table against brute-force integration ----------------- */
    {
        static const uint32_t k_x[3] = {12u, 32u, 57u}, k_y[4] = {19u, 32u, 47u, 63u};
        double worst = 0.0, worst_fit = 0.0;
        uint32_t ix, iy;
        px = render_and_read(a, a->dfg, DFG_SIZE, DFG_SIZE);
        for (iy = 0u; iy < 4u; ++iy) {
            for (ix = 0u; ix < 3u; ++ix) {
                const double nov = (k_x[ix] + 0.5) / DFG_SIZE, rough = (k_y[iy] + 0.5) / DFG_SIZE;
                const float *t = &px[((size_t)k_y[iy] * DFG_SIZE + k_x[ix]) * 4u];
                double ref[2], e;
                dfg_reference(nov, rough, ref);
                e = fmax(fabs(t[0] - ref[0]), fabs(t[1] - ref[1]));
                if (e > worst) worst = e;
                /* Karis' analytic fit, for the README: how far off is it? */
                {
                    const double r0 = rough * -1.0 + 1.0, r1 = rough * -0.0275 + 0.0425;
                    const double a004 = fmin(r0 * r0, exp2(-9.28 * nov)) * r0 + r1;
                    const double fa = -1.04 * a004 + (rough * -0.572 + 1.04), fb = 1.04 * a004 + (rough * 0.022 - 0.04);
                    e = fmax(fabs(fa - ref[0]), fabs(fb - ref[1]));
                    if (e > worst_fit) worst_fit = e;
                }
            }
        }
        free(px);
        demo_check(d, "DFG table vs brute force, 12 points: max error", worst, "<= 0.01", worst <= 0.01);
        demo_check(d, "  (mu_env_brdf_approx vs brute force, same points)", worst_fit, "reported only", true);
    }

    /* ---- 2. irradiance of a half-white sky, every texel ------------------- */
    {
        double worst = 0.0;
        uint32_t x, y;
        a->env_mode = 2u;
        px = render_and_read(a, a->irradiance, IRR_SIZE, IRR_SIZE);
        for (y = 0u; y < IRR_SIZE; ++y) {
            for (x = 0u; x < IRR_SIZE; ++x) {
                double n[3], e;
                oct_decode((x + 0.5) / IRR_SIZE, (y + 0.5) / IRR_SIZE, n);
                /* white above the horizon, black below: E / pi = (1 + n.y) / 2 */
                e = fabs(px[((size_t)y * IRR_SIZE + x) * 4u] - 0.5 * (1.0 + n[1]));
                if (e > worst) worst = e;
            }
        }
        free(px);
        demo_check(d, "irradiance, half-white sky, 1024 texels: max error", worst, "<= 0.02", worst <= 0.02);
    }

    /* ---- 3. white furnace ------------------------------------------------- */
    a->env_mode = 1u;
    a->furnace  = true;
    a->view     = 4u;
    a->energy   = 1u;
    {
        /* A constant environment must stay constant through every filter. */
        double worst = 0.0;
        size_t k;
        for (i = 1u; i <= LEVELS; ++i) {
            const uint32_t size = i < LEVELS ? k_level_size[i] : IRR_SIZE;
            px = render_and_read(a, i < LEVELS ? a->level[i] : a->irradiance, size, size);
            for (k = 0u; k < (size_t)size * size; ++k) {
                const double e = fabs((double)px[k * 4u] - 1.0);
                if (e > worst) worst = e;
            }
            free(px);
        }
        demo_check(d, "white sky: prefiltered levels and irradiance, |x - 1|", worst, "<= 0.002", worst <= 0.002);
    }
    for (i = 0u; i < 3u; ++i) {
        static const char *const k_row[3] = {"metal", "half metal", "dielectric"};
        Furnace f;
        px = render_and_read(a, a->hdr, d->width, d->height);
        f = furnace_stats(px, pixels, i * COLS, i * COLS + COLS - 1u);
        free(px);
        snprintf(label, sizeof label, "furnace, %s, 7 roughnesses: lowest sphere mean", k_row[i]);
        demo_check(d, label, f.lo_mean, ">= 0.98", f.empty == 0u && f.lo_mean >= 0.98);
        snprintf(label, sizeof label, "furnace, %s: highest sphere mean", k_row[i]);
        demo_check(d, label, f.hi_mean, "<= 1.02", f.empty == 0u && f.hi_mean <= 1.02);
        snprintf(label, sizeof label, "furnace, %s: worst single pixel, |x - 1|", k_row[i]);
        demo_check(d, label, fmax(1.0 - f.lo_pixel, f.hi_pixel - 1.0), "<= 0.03",
                         fmax(1.0 - f.lo_pixel, f.hi_pixel - 1.0) <= 0.03);
    }
    {
        /* Controls: the test must be able to fail. */
        Furnace f;
        a->energy = 0u;
        px = render_and_read(a, a->hdr, d->width, d->height);
        f = furnace_stats(px, pixels, 0u, COLS - 1u);
        free(px);
        demo_check(d, "control, single scattering: roughest metal sphere", f.lo_mean, "< 0.9 (energy lost)",
                         f.lo_mean < 0.9);
        a->energy = 2u;
        px = render_and_read(a, a->hdr, d->width, d->height);
        f = furnace_stats(px, pixels, 0u, COLS - 1u);
        free(px);
        demo_check(d, "mu_pbr_indirect as is: brightest metal sphere", f.hi_mean, "reported only (should be 1)", true);
    }
}

int main(int argc, char **argv) {
    static App app;
    static const DemoOption options[] = {
        {"--view", .u32 = &app.view, .lo = 0, .hi = 3, .help = "0 shaded, 1 specular IBL, 2 diffuse IBL, 3 sun only"},
        {"--energy", .u32 = &app.energy, .lo = 0, .hi = 2, .help = "0 single scattering, 1 multiple scattering, 2 mu_lib's mu_pbr_indirect unchanged"},
        {"--dfg", .u32 = &app.dfg_source, .lo = 0, .hi = 1, .help = "0 baked table, 1 mu_env_brdf_approx"},
        {"--background", .f32 = &app.background, .lo = 0, .hi = 4, .help = "0 the sky, 1..4 a prefiltered level"},
        {"--sun-height", .f32 = &app.sun_height, .lo = -1.6, .hi = 1.6, .help = "radians above the horizon"},
        {"--maps", .flag = &app.show_maps, .help = "show the baked maps along the bottom"},
        {"--furnace", .flag = &app.furnace, .help = "white spheres in a white environment"},
        {0}};
    demo_defaults(&app.demo, "pbr", "PBR - metallic/roughness, image-based lighting");
    app.orbit = (DemoOrbit){.target = {0.0f, -0.3f, 0.0f}, .distance = 9.6f, .yaw = 0.0f, .pitch = 0.0f,
                            .fov_deg = 36.0f, .znear = 0.1f, .zfar = 100.0f,
                            .min_distance = 2.0f, .max_distance = 40.0f};
    app.sun_yaw    = 0.9f;
    app.sun_height = 0.55f;
    app.sun_power  = 2.2f;
    app.energy     = 1u;
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
