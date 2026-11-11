/* pat — Points as Tori: signed distance to a point cloud, one query at a time.
 *
 *   Bake (when the cloud changes) -> Draw -> Dots -> Present
 *
 * Feng, Gkioulekas, Crane, "Points as Tori: Fast Pointwise Signed Distance
 * for Point Clouds" (2026). A small network looks at the 64 points around
 * each point of an oriented cloud and gives the torus that fits the surface
 * there; the distance at any x is then a weighted average of the distances
 * to the tori of the 32 points nearest to x. method.c is that on the CPU,
 * shaders/pat_common.slang the distance on the GPU.
 *
 * --fit runs the network (the authors' released weights) and writes the tori
 * of the five clouds to data/NAME.pat; the demo reads those files.
 * --check compares the network with the authors' own code, the tori with the
 * polynomials they come from, the distance with the true one, and the
 * shaders with method.c.
 */
#include "demo.h"
#include "method.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SHADERS    "demos/pat/shaders/"
#define DATA       "demos/pat/data/"
#define MAX_POINTS 4096u
#define GRID       16u                      /* cells along an edge of a cloud's grid */
#define CELLS      (GRID * GRID * GRID)
#define VOXELS     64u
#define HALF       1.3f                     /* the baked volume spans [-HALF, HALF]^3 */
#define SHAPES     5u

static const char *const k_shapes[SHAPES]   = {"torus", "box", "blob", "tooth", "snail"};
static const char *const k_views[]          = {"surface", "slice", "normals"};
static const char *const k_methods[]        = {"tori (the paper)", "tangent planes", "nearest torus only"};
static const char *const k_combines[]       = {"one cloud", "union", "intersection", "difference"};

typedef struct CloudHeader {
    uint32_t points, cells, count, grid;
    float    origin[3], cell;
} CloudHeader;

typedef struct Push {
    float    eye[4], right[4], up[4], forward[4];
    uint32_t header, combine, method, view;
    uint32_t volume, sampler, direct, points;
    float    half_size, voxel, dot_size;
    uint32_t count;
} Push;
_Static_assert(sizeof(Push) == 112, "Push must match draw.slang");

typedef struct BakePush {
    uint32_t header, combine, method, volume;
    float    half_size;
    uint32_t voxels;
} BakePush;

typedef struct App {
    Demo      demo;
    DemoOrbit orbit;
    BufSlice  points, cells, header;
    BufUse    reads[3][3];              /* for the compute, fragment and vertex stage */
    Target    volume, hdr;
    uint32_t  volume_index;
    PatPoint  cloud[2][MAX_POINTS];     /* as uploaded: ordered by cell */
    uint32_t  count[2], loaded[2], baked;
    /* options */
    uint32_t  shape, other, combine, method, view;
    float     offset, slice, dot_size;
    bool      direct, dots, fit;
    Push      push;                     /* the last frame's, for --check to rebuild its rays */
} App;

/* ---- the clouds ------------------------------------------------------------
 *
 * Three are sampled from a function whose zero set is the surface (the first
 * two are exact distances); two are scans that come with the authors' code. */

static double implicit(uint32_t shape, const double x[3]) {
    if (shape == 0u) return hypot(hypot(x[0], x[1]) - 0.6, x[2]) - 0.25;
    if (shape == 1u) {   /* a box of half size (0.65, 0.5, 0.35), edges rounded to 0.15 */
        const double q[3] = {fabs(x[0]) - 0.5, fabs(x[1]) - 0.35, fabs(x[2]) - 0.2};
        const double o[3] = {fmax(q[0], 0.0), fmax(q[1], 0.0), fmax(q[2], 0.0)};
        return sqrt(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]) + fmin(fmax(q[0], fmax(q[1], q[2])), 0.0) - 0.15;
    }
    /* A ball with a bumpy radius, as the paper's training blobs are (theirs use Perlin noise). */
    return sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]) -
           0.65 * (1.0 + 0.22 * sin(3.0 * x[0] + 0.5) * sin(3.4 * x[1] + 1.3) * sin(2.6 * x[2] + 2.1) +
                   0.1 * sin(5.2 * x[0] + 4.4 * x[1] + 1.0) * cos(4.8 * x[2]));
}

static void implicit_gradient(uint32_t shape, const double x[3], double g[3]) {
    uint32_t c;
    for (c = 0u; c < 3u; ++c) {
        double hi[3] = {x[0], x[1], x[2]}, lo[3] = {x[0], x[1], x[2]};
        hi[c] += 1e-6; lo[c] -= 1e-6;
        g[c] = (implicit(shape, hi) - implicit(shape, lo)) / 2e-6;
    }
}

/* The distance to the zero set of implicit(): itself for the first two shapes; for the blob, the
 * distance to the foot of x on the surface, found by walking there from the nearest input point. */
static double truth(uint32_t shape, const PatPoint *seeds, uint32_t count, const double x[3], bool walk) {
    double s[3] = {x[0], x[1], x[2]}, g[3], best = 1e30, scale = 1.0;   /* gcc 14 cannot see that count > 0 */
    uint32_t i, c, step;
    if (!walk) return implicit(shape, x);
    for (i = 0u; i < count; ++i) {
        const double d[3] = {x[0] - seeds[i].pos[0], x[1] - seeds[i].pos[1], x[2] - seeds[i].pos[2]};
        const double r = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
        if (r < best) { best = r; s[0] = seeds[i].pos[0]; s[1] = seeds[i].pos[1]; s[2] = seeds[i].pos[2]; }
    }
    /* s slides over the surface towards x: a move along the tangent plane, then back onto the surface.
     * The move and half of it are both tried, and the one that ends nearer to x is taken. */
    for (step = 0u; step < 200u && scale > 1e-9; ++step) {
        double to[3], along = 0.0, gg, moved[2][3], d2[2] = {0.0, 0.0};
        uint32_t half;
        implicit_gradient(shape, s, g);
        gg = g[0] * g[0] + g[1] * g[1] + g[2] * g[2];
        for (c = 0u; c < 3u; ++c) { to[c] = x[c] - s[c]; along += to[c] * g[c] / gg; }
        for (half = 0u; half < 2u; ++half) {
            for (c = 0u; c < 3u; ++c) moved[half][c] = s[c] + scale / (1.0 + half) * (to[c] - along * g[c]);
            for (i = 0u; i < 4u; ++i) {
                const double f = implicit(shape, moved[half]);
                double       slope[3];
                implicit_gradient(shape, moved[half], slope);
                gg = slope[0] * slope[0] + slope[1] * slope[1] + slope[2] * slope[2];
                for (c = 0u; c < 3u; ++c) moved[half][c] -= f * slope[c] / gg;
            }
            for (c = 0u; c < 3u; ++c) d2[half] += (x[c] - moved[half][c]) * (x[c] - moved[half][c]);
        }
        half = d2[1] < d2[0];
        if (half || d2[0] >= best) scale *= 0.5;
        if (d2[half] < best) { best = d2[half]; s[0] = moved[half][0]; s[1] = moved[half][1]; s[2] = moved[half][2]; }
    }
    return implicit(shape, x) < 0.0 ? -sqrt(best) : sqrt(best);
}

/* 8 x count points dropped onto the surface, then the count of them that are farthest apart
 * (the paper's farthest point sampling). */
static void sample_implicit(uint32_t shape, uint32_t count, float (*pos)[3], float (*normal)[3]) {
    const uint32_t many = 8u * count;
    double (*cand)[3] = malloc((size_t)many * sizeof *cand), *apart = malloc((size_t)many * sizeof *apart);
    uint32_t rng = 12345u + shape, made = 0u, pick = 0u, i, j, c, step;
    while (made < many) {
        double x[3], g[3], gg, len;
        for (c = 0u; c < 3u; ++c) {
            rng  = rng * 1664525u + 1013904223u;
            x[c] = (double)(rng >> 8) / 8388608.0 - 1.0;
        }
        for (step = 0u; step < 20u; ++step) {
            implicit_gradient(shape, x, g);
            gg = g[0] * g[0] + g[1] * g[1] + g[2] * g[2];
            if (gg < 1e-12) break;
            len = implicit(shape, x) / gg;
            for (c = 0u; c < 3u; ++c) x[c] -= len * g[c];
        }
        if (fabs(implicit(shape, x)) > 1e-10) continue;
        for (c = 0u; c < 3u; ++c) cand[made][c] = x[c];
        apart[made++] = 1e30;
    }
    for (i = 0u; i < count; ++i) {
        double g[3], len, best = -1.0;
        implicit_gradient(shape, cand[pick], g);
        len = sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
        for (c = 0u; c < 3u; ++c) { pos[i][c] = (float)cand[pick][c]; normal[i][c] = (float)(g[c] / len); }
        j = pick;
        for (step = 0u; step < many; ++step) {
            const double d[3] = {cand[step][0] - cand[j][0], cand[step][1] - cand[j][1], cand[step][2] - cand[j][2]};
            const double r = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
            if (r < apart[step]) apart[step] = r;
            if (apart[step] > best) { best = apart[step]; pick = step; }
        }
    }
    free(cand); free(apart);
}

typedef struct Neighbour { float d; uint32_t i; } Neighbour;

static int compare_neighbour(const void *a, const void *b) {
    const Neighbour *x = a, *y = b;
    return x->d < y->d ? -1 : x->d > y->d ? 1 : x->i < y->i ? -1 : x->i > y->i;
}

/* The network's coefficients at point i of a cloud: its PAT_NEIGHBOURS nearest points, itself first. */
static void coefficients(const PatNet *net, const float (*pos)[3], const float (*normal)[3], uint32_t count, uint32_t i,
                         Neighbour *scratch, double coef[6]) {
    float near_pos[PAT_NEIGHBOURS][3], near_normal[PAT_NEIGHBOURS][3];
    uint32_t j;
    for (j = 0u; j < count; ++j) {
        const float d[3] = {pos[j][0] - pos[i][0], pos[j][1] - pos[i][1], pos[j][2] - pos[i][2]};
        scratch[j].d = j == i ? -1.0f : d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
        scratch[j].i = j;
    }
    qsort(scratch, count, sizeof *scratch, compare_neighbour);
    for (j = 0u; j < PAT_NEIGHBOURS; ++j) {
        memcpy(near_pos[j], pos[scratch[j].i], sizeof near_pos[j]);
        memcpy(near_normal[j], normal[scratch[j].i], sizeof near_normal[j]);
    }
    pat_net_forward(net, near_pos, near_normal, coef);
}

/* Positions and normals of a cloud, before any fitting. */
static uint32_t cloud_input(const Demo *d, uint32_t shape, float (*pos)[3], float (*normal)[3]) {
    char     path[96], line[128];
    uint32_t count = 0u, i;
    FILE    *f;
    if (shape < 3u) {
        sample_implicit(shape, 2048u, pos, normal);
        return 2048u;
    }
    /* A binary PLY with x y z nx ny nz floats per vertex and nothing else. */
    snprintf(path, sizeof path, DATA "%s.ply", k_shapes[shape]);
    f = fopen(path, "rb");
    if (!f) demo_fatal(d, "cannot open %s", path);
    while (fgets(line, sizeof line, f) && strncmp(line, "end_header", 10) != 0)
        sscanf(line, "element vertex %u", &count);
    if (count == 0u || count > MAX_POINTS) demo_fatal(d, "%s: %u points, at most %u", path, count, MAX_POINTS);
    for (i = 0u; i < count; ++i)
        if (fread(pos[i], 4, 3, f) != 3 || fread(normal[i], 4, 3, f) != 3) demo_fatal(d, "%s is cut short", path);
    fclose(f);
    return count;
}

/* The fitted points of a cloud, in the order of the input. */
static uint32_t cloud_read(const Demo *d, uint32_t shape, PatPoint *out) {
    char     path[96], magic[4];
    uint32_t count = 0u;
    FILE    *f;
    snprintf(path, sizeof path, DATA "%s.pat", k_shapes[shape]);
    f = fopen(path, "rb");
    if (!f) demo_fatal(d, "cannot open %s (run from the repository root; pat --fit writes it)", path);
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "PAT1", 4) != 0 || fread(&count, 4, 1, f) != 1 || count > MAX_POINTS ||
        fread(out, sizeof *out, count, f) != count)
        demo_fatal(d, "%s is not a cloud this build reads", path);
    fclose(f);
    return count;
}

/* --fit: the precomputation of the paper's algorithm 1, for every cloud. */
static void fit(App *a) {
    Demo   *d = &a->demo;
    PatNet  net;
    float (*pos)[3] = malloc(MAX_POINTS * sizeof *pos), (*normal)[3] = malloc(MAX_POINTS * sizeof *normal);
    Neighbour *scratch = malloc(MAX_POINTS * sizeof *scratch);
    PatPoint  *out = malloc(MAX_POINTS * sizeof *out);
    uint32_t   shape, i, count;
    if (!pat_net_load(&net, DATA "weights.bin")) demo_fatal(d, "cannot read " DATA "weights.bin");
    for (shape = 0u; shape < SHAPES; ++shape) {
        char    path[96];
        FILE   *f;
        clock_t start = clock();
        count = cloud_input(d, shape, pos, normal);
        for (i = 0u; i < count; ++i) {
            double coef[6];
            coefficients(&net, pos, normal, count, i, scratch, coef);
            pat_fit_torus(pos[i], normal[i], coef, &out[i], NULL);
        }
        snprintf(path, sizeof path, DATA "%s.pat", k_shapes[shape]);
        f = fopen(path, "wb");
        if (!f || fwrite("PAT1", 1, 4, f) != 4 || fwrite(&count, 4, 1, f) != 1 || fwrite(out, sizeof *out, count, f) != count)
            demo_fatal(d, "cannot write %s", path);
        fclose(f);
        printf("%s: %u points, %.1f s -> %s\n", k_shapes[shape], count, (double)(clock() - start) / CLOCKS_PER_SEC, path);
        fflush(stdout);
    }
    free(pos); free(normal); free(scratch); free(out); free(net.w);
}

/* A cloud into slot 0 or 1 of the buffers: points ordered by grid cell, and for each cell the index
 * of its first point (CELLS + 1 of them, so a run of cells is first[a] .. first[b + 1]). */
static void load_cloud(App *a, uint32_t slot, uint32_t shape) {
    Demo       *d = &a->demo;
    PatPoint   *raw = malloc(MAX_POINTS * sizeof *raw), *sorted = a->cloud[slot];
    uint32_t   *first = calloc(CELLS + 2u, sizeof *first), *cell = malloc(MAX_POINTS * sizeof *cell);
    const uint32_t count = cloud_read(d, shape, raw);
    float       lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f}, size;
    CloudHeader header;
    BufSlice    dst;
    uint32_t    i, c;
    for (i = 0u; i < count; ++i)
        for (c = 0u; c < 3u; ++c) {
            if (raw[i].pos[c] < lo[c]) lo[c] = raw[i].pos[c];
            if (raw[i].pos[c] > hi[c]) hi[c] = raw[i].pos[c];
        }
    size = fmaxf(hi[0] - lo[0], fmaxf(hi[1] - lo[1], hi[2] - lo[2])) * 1.001f;
    for (i = 0u; i < count; ++i) {
        uint32_t at[3];
        for (c = 0u; c < 3u; ++c) {
            const int32_t n = (int32_t)floorf((raw[i].pos[c] - lo[c]) / (size / GRID));
            at[c] = n < 0 ? 0u : n > (int32_t)GRID - 1 ? GRID - 1u : (uint32_t)n;
        }
        cell[i] = at[0] + GRID * (at[1] + GRID * at[2]);
        first[cell[i] + 2u] += 1u;
    }
    for (i = 2u; i < CELLS + 2u; ++i) first[i] += first[i - 1u];      /* first[n + 1]: where cell n starts */
    for (i = 0u; i < count; ++i) sorted[first[cell[i] + 1u]++] = raw[i];   /* and now where it ends: first[n] is its start */

    header = (CloudHeader){.points = (uint32_t)(a->points.offset + (uint64_t)slot * MAX_POINTS * sizeof(PatPoint)),
                           .cells = (uint32_t)(a->cells.offset + (uint64_t)slot * (CELLS + 1u) * 4u), .count = count, .grid = GRID,
                           .origin = {lo[0], lo[1], lo[2]}, .cell = size / GRID};
    dst = a->points; dst.offset = header.points; dst.size = (uint64_t)count * sizeof(PatPoint);
    demo_upload(d, dst, sorted, dst.size);
    dst = a->cells; dst.offset = header.cells; dst.size = (uint64_t)(CELLS + 1u) * 4u;
    demo_upload(d, dst, first, dst.size);
    dst = a->header; dst.offset += slot * sizeof header; dst.size = sizeof header;
    demo_upload(d, dst, &header, sizeof header);
    a->count[slot]  = count;
    a->loaded[slot] = shape;
    free(raw); free(first); free(cell);
}

/* ---- frames ---------------------------------------------------------------- */

static void app_frame(App *a) {
    Demo    *d = &a->demo;
    PassCtx *pass;
    float    view[16], eye[3];
    const float    aspect = (float)d->width / (float)d->height;
    const uint32_t key = a->shape | a->other << 8u | a->combine << 16u | a->method << 24u;
    Push     p = {.header = (uint32_t)a->header.offset, .combine = a->combine, .method = a->method, .view = a->view,
                  .volume = a->volume_index, .sampler = d->linear_clamp_index, .direct = a->direct ? 1u : 0u,
                  .points = (uint32_t)a->points.offset, .half_size = HALF, .voxel = 2.0f * HALF / VOXELS, .dot_size = a->dot_size};
    BakePush bake = {.header = p.header, .combine = a->combine, .method = a->method, .volume = a->volume_index,
                     .half_size = HALF, .voxels = VOXELS};
    d->present.tonemap = DEMO_TONEMAP_NONE;

    if (a->loaded[0] != a->shape) load_cloud(a, 0u, a->shape);
    if (a->combine != 0u && a->loaded[1] != a->other) load_cloud(a, 1u, a->other);
    p.count = a->count[0];

    demo_orbit_input(d, &a->orbit);
    demo_orbit_camera(&a->orbit, aspect, view, NULL, NULL, eye);
    /* Rows of the rotation part of a column-major view matrix are the camera axes; it looks down -Z. */
    p.eye[0] = eye[0]; p.eye[1] = eye[1]; p.eye[2] = eye[2]; p.eye[3] = a->offset;
    p.right[0] = view[0]; p.right[1] = view[4]; p.right[2] = view[8];
    p.right[3] = tanf(a->orbit.fov_deg * 0.017453292f * 0.5f);
    p.up[0] = view[1]; p.up[1] = view[5]; p.up[2] = view[9]; p.up[3] = aspect;
    p.forward[0] = -view[2]; p.forward[1] = -view[6]; p.forward[2] = -view[10]; p.forward[3] = a->slice;

    a->push = p;
    if (!demo_frame_begin(d)) return;
    pass = demo_pass(d, "Bake");
    if (a->baked != key) {
        pass_push(pass, &bake, sizeof bake);
        pass_dispatch(pass, VOXELS / 4u, VOXELS / 4u, VOXELS / 4u);
        a->baked = key;
    }
    pass = demo_pass(d, "Draw");
    pass_push(pass, &p, sizeof p);
    pass_draw_fullscreen_triangle(pass, d->width, d->height);
    pass = demo_pass(d, "Dots");
    if (a->dots && (a->view == 0u || a->view == 2u)) {
        pass_push(pass, &p, sizeof p);
        pass_draw(pass, a->count[0] * 6u, 1u, 0u, 0u);
    }
    demo_present(d);
    demo_frame_end(d);
}

static void app_init(App *a) {
    Demo    *d = &a->demo;
    PassDesc passes[5];
    /* Half floats hold a distance to 1 part in 2000; --check wants more. */
    const Format format = d->check ? FMT_RGBA32F : FMT_RGBA16F;
    uint32_t stage, i;

    a->points = demo_alloc(d, NULL, 2u * (uint64_t)MAX_POINTS * sizeof(PatPoint));
    a->cells  = demo_alloc(d, NULL, 2u * (uint64_t)(CELLS + 1u) * 4u);
    a->header = demo_alloc(d, NULL, 2u * sizeof(CloudHeader));
    a->volume = demo_volume(d, "pat_volume", FMT_RGBA16F, VOXELS, VOXELS, VOXELS, &a->volume_index);
    a->hdr    = demo_target(d, "pat_hdr", format, d->width, d->height, false, NULL);
    for (stage = 0u; stage < 3u; ++stage) {
        a->reads[stage][0] = use_read(a->points.buffer, a->points.offset, a->points.size);
        a->reads[stage][1] = use_read(a->cells.buffer, a->cells.offset, a->cells.size);
        a->reads[stage][2] = use_read(a->header.buffer, a->header.offset, a->header.size);
        for (i = 0u; i < 3u; ++i) a->reads[stage][i].stages = stage == 0u ? SH_COMP : stage == 1u ? SH_FRAG : SH_VERT;
    }
    a->loaded[0] = a->loaded[1] = a->baked = 0xffffffffu;

    passes[0] = (PassDesc){
        .name = "Bake", .kind = PASS_COMPUTE, .state = draw_state_opaque(SC_1, CULL_NONE),
        .pipe = pipe_intern_compute(d->gpu, demo_shader(d, SHADERS "bake.comp.spv", STAGE_COMPUTE), "main"),
        .buf_reads = a->reads[0], .buf_read_count = 3u, .tex_writes = &a->volume, .tex_write_count = 1u};
    passes[1] = (PassDesc){
        .name = "Draw", .kind = PASS_GRAPHICS, .state = draw_state_opaque(SC_1, CULL_NONE),
        .pipe = pipe_intern_graphics(d->gpu, d->fullscreen_vs, demo_shader(d, SHADERS "draw.frag.spv", STAGE_FRAGMENT),
                                     format, FMT_NONE, BLEND_OPAQUE),
        .colors = (AttachDesc[]){attach_color(a->hdr, ATT_WRITE)}, .color_count = 1u,
        .buf_reads = a->reads[1], .buf_read_count = 3u, .tex_reads = &a->volume, .tex_read_count = 1u};
    passes[2] = (PassDesc){
        .name = "Dots", .kind = PASS_GRAPHICS, .state = draw_state_opaque(SC_1, CULL_NONE),
        .pipe = pipe_intern_graphics(d->gpu, demo_shader(d, SHADERS "dots.vert.spv", STAGE_VERTEX),
                                     demo_shader(d, SHADERS "dots.frag.spv", STAGE_FRAGMENT), format, FMT_NONE, BLEND_OPAQUE),
        .colors = (AttachDesc[]){attach_color(a->hdr, ATT_WRITE_KEEP)}, .color_count = 1u,
        .buf_reads = a->reads[2], .buf_read_count = 1u};
    passes[3]  = demo_present_pass(d, a->hdr);
    d->program = demo_program(d, passes, 4u);
}

static void app_ui(App *a, struct nk_context *ctx) {
    Demo *d = &a->demo;
    if (nk_begin(ctx, "Points as Tori", nk_rect(10, 10, 300, 380),
                 NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_TITLE)) {
        demo_ui_text(ctx, "%.1f ms   %u points", (double)d->dt * 1000.0, a->count[0]);
        demo_ui_choice(ctx, "Cloud", k_shapes, SHAPES, &a->shape);
        demo_ui_choice(ctx, "View", k_views, 3u, &a->view);
        demo_ui_choice(ctx, "Blend", k_methods, 3u, &a->method);
        demo_ui_float(ctx, "Offset (level drawn)", &a->offset, -0.15f, 0.3f);
        demo_ui_float(ctx, "Slice z", &a->slice, -1.0f, 1.0f);
        demo_ui_choice(ctx, "Combine", k_combines, 4u, &a->combine);
        demo_ui_choice(ctx, "Second cloud", k_shapes, SHAPES, &a->other);
        demo_ui_bool(ctx, "Show the points", &a->dots);
        demo_ui_float(ctx, "Dot size", &a->dot_size, 0.002f, 0.02f);
        demo_ui_bool(ctx, "Every step pointwise (slow)", &a->direct);
    }
    nk_end(ctx);
}

/* One frame of a --check view, read back: four floats a pixel, the top row first. */
static void capture(App *a, uint32_t view, float *px) {
    a->view = view;
    app_frame(a);
    demo_readback(&a->demo, a->hdr, px, (size_t)a->demo.width * a->demo.height * 16u);
}

/* The largest |value| of a torus's distance on its own polynomial, on a circle of radius h around the point. */
static double contact(const PatPoint *torus, const float pos[3], const float normal[3], const double coef[6], double h) {
    double n[3] = {normal[0], normal[1], normal[2]}, s[3], t[3], worst = 0.0;
    uint32_t k, c;
    pat_frame(n, s, t);
    for (k = 0u; k < 8u; ++k) {
        const double u = h * cos(k * 0.7853981634), v = h * sin(k * 0.7853981634);
        const double q = coef[0] + coef[2] * u + coef[1] * v + coef[3] * u * v + coef[5] * u * u + coef[4] * v * v;
        double x[3], g;
        for (c = 0u; c < 3u; ++c) x[c] = pos[c] + u * s[c] + v * t[c] + q * n[c];
        g = fabs(pat_torus_distance(torus, x, NULL));
        if (g > worst) worst = g;
    }
    return worst;
}

static void app_check(App *a) {
    /* a00 a01 a10 a11 a02 a20 at the first four points of the torus cloud, from the authors' own code:
     * their JAX network (github.com/nzfeng/points-as-tori, 253bf9f) run on the points that --fit samples. */
    static const double k_reference[4][6] = {
        {0.000227667872, -0.0061578881, -0.00110460259, 0.023566585, -1.60098112, -1.46542168},
        {0.000137803174, -0.0087997634, -0.00585114025, 0.0229342636, -1.52243876, -1.39319134},
        {0.000339187391, -0.00893921964, 0.00166316144, 0.0249957945, -1.55037022, -1.42092192},
        {3.43002794e-05, -0.00632759742, 0.000157305971, 0.0251924228, -1.56906557, -1.4376651}};
    Demo          *d = &a->demo;
    const uint32_t w = d->width, h = d->height, step = 4u;
    float         *px = malloc((size_t)w * h * 16u), *px2 = malloc((size_t)w * h * 16u);
    PatPoint      *raw = malloc(MAX_POINTS * sizeof *raw);
    char           label[160];
    uint32_t       shape, i, c, x, y;

    a->dots = false;
    a->direct = false;
    a->combine = 0u;
    a->offset = 0.0f;

    /* 1. The network in C against the authors' code, and the cloud files against the network. */
    {
        PatNet     net;
        float    (*pos)[3] = malloc(MAX_POINTS * sizeof *pos), (*normal)[3] = malloc(MAX_POINTS * sizeof *normal);
        Neighbour *scratch = malloc(MAX_POINTS * sizeof *scratch);
        double     worst = 0.0, stored = 0.0;
        uint32_t   count;
        if (!pat_net_load(&net, DATA "weights.bin")) demo_fatal(d, "cannot read " DATA "weights.bin");
        count = cloud_input(d, 0u, pos, normal);
        cloud_read(d, 0u, raw);
        for (i = 0u; i < 4u; ++i) {
            double   coef[6];
            PatPoint again;
            coefficients(&net, pos, normal, count, i, scratch, coef);
            for (c = 0u; c < 6u; ++c) worst = fmax(worst, fabs(coef[c] - k_reference[i][c]));
            pat_fit_torus(pos[i], normal[i], coef, &again, NULL);
            for (c = 0u; c < 20u; ++c) stored = fmax(stored, fabs((double)((const float *)&again)[c] - ((const float *)&raw[i])[c]));
        }
        demo_check(d, "network, 4 points of the torus cloud: largest difference from the authors' code in a coefficient", worst,
                   "< 2e-5", worst < 2e-5);
        demo_check(d, "  the same 4 points fitted again against data/torus.pat: largest difference in a stored number", stored,
                   "< 1e-4", stored < 1e-4);
        free(pos); free(normal); free(scratch); free(net.w);
    }

    /* 2. The torus of equations 19 to 22 against the polynomial it is built from. */
    {
        static const double k_coef[4][6] = {{0.02, 0.1, -0.2, 0.3, -0.5, -1.0},      /* bends away from the normal both ways */
                                            {0.02, 0.1, -0.2, 0.3, 0.5, 1.0},        /* towards it both ways */
                                            {0.02, 0.1, -0.2, 0.03, 0.25, -1.0},     /* a saddle */
                                            {0.0, 0.0, 0.0, 0.0, -1e-7, -2e-7}};     /* all but flat */
        static const char *const k_names[3] = {"bending away from the normal", "bending towards the normal", "a saddle"};
        const float pos[3] = {0.1f, 0.2f, 0.3f}, normal[3] = {0.0f, 0.6f, 0.8f};
        PatPoint torus;
        PatTorus paper;
        double   forms = 0.0, flat[2] = {0.0, 0.0};
        uint32_t rng = 7u;
        for (i = 0u; i < 3u; ++i) {
            double n[3] = {normal[0], normal[1], normal[2]}, s[3], t[3], star[3], g[3], at[3], along = 0.0;
            const double area = sqrt(1.0 + k_coef[i][1] * k_coef[i][1] + k_coef[i][2] * k_coef[i][2]);
            double order;
            pat_fit_torus(pos, normal, k_coef[i], &torus, &paper);
            order = log2(contact(&torus, pos, normal, k_coef[i], 0.02) / contact(&torus, pos, normal, k_coef[i], 0.01));
            pat_frame(n, s, t);
            for (c = 0u; c < 3u; ++c) {
                star[c] = (n[c] - k_coef[i][2] * s[c] - k_coef[i][1] * t[c]) / area;
                at[c]   = pos[c] + k_coef[i][0] * n[c];
            }
            pat_torus_distance(&torus, at, g);
            for (c = 0u; c < 3u; ++c) along += g[c] * star[c];
            snprintf(label, sizeof label, "torus on a polynomial %s: order of contact", k_names[i]);
            demo_check(d, label, order, i < 2u ? ">= 2.9" : "(reported)", i == 2u || order >= 2.9);
            demo_check(d, "  gradient at the point . the polynomial's normal", along, i < 2u ? "> 0.999999" : "(reported)",
                       i == 2u || along > 0.999999);
            /* The curvature form of the distance against equation 23 as printed, both in double. */
            for (x = 0u; x < 1000u; ++x) {
                double q[3], rel[3], axial = 0.0, radial, value;
                for (c = 0u; c < 3u; ++c) {
                    rng  = rng * 1664525u + 1013904223u;
                    q[c] = (double)(rng >> 8) / 8388608.0 - 1.0;
                    rel[c] = q[c] - paper.centre[c];
                    axial += rel[c] * paper.axis[c];
                }
                radial = sqrt(fmax(rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2] - axial * axial, 0.0));
                value  = paper.sign * (hypot(radial - paper.major, axial) - paper.minor);
                forms  = fmax(forms, fabs(value - pat_torus_distance(&torus, q, NULL)));
            }
        }
        demo_check(d, "curvature form of the torus distance, from floats - equation 23 in double, 3000 points", forms, "< 1e-5", forms < 1e-5);
        /* Radii of five million: equation 23 in float against the curvature form, both against the tangent plane. */
        pat_fit_torus(pos, normal, k_coef[3], &torus, &paper);
        for (x = 0u; x < 1000u; ++x) {
            double q[3], plane = 0.0;
            float  rel[3], axial = 0.0f, radial;
            for (c = 0u; c < 3u; ++c) {
                rng  = rng * 1664525u + 1013904223u;
                q[c] = pos[c] + 0.5 * ((double)(rng >> 8) / 8388608.0 - 1.0);
                plane += (q[c] - pos[c]) * torus.normal[c];
                rel[c] = (float)q[c] - (float)paper.centre[c];
                axial += rel[c] * (float)paper.axis[c];
            }
            radial  = sqrtf(fmaxf(rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2] - axial * axial, 0.0f));
            flat[0] = fmax(flat[0], fabs((double)((float)paper.sign * (hypotf(radial - (float)paper.major, axial) - (float)paper.minor)) - plane));
            flat[1] = fmax(flat[1], fabs(pat_torus_distance(&torus, q, NULL) - plane));
        }
        demo_check(d, "an all but flat polynomial (radii 2.5e6, 5e6): equation 23 in float - the plane (reported)", flat[0], "", true);
        demo_check(d, "  the curvature form - the plane", flat[1], "< 1e-6", flat[1] < 1e-6);
    }

    /* 3. The distance against the true one on a 32^3 grid over [-1, 1]^3, the paper's measure (its
     * figures 7 and 18), for the three clouds whose surface is known. */
    for (shape = 0u; shape < 3u; ++shape) {
        const uint32_t n = 32u;
        double error[3] = {0, 0, 0}, wrong[3] = {0, 0, 0}, eikonal = 0.0, walk = 0.0, slope = 0.0;
        uint32_t method, near_count = 0u;
        a->shape = shape;
        app_frame(a);            /* loads the cloud */
        for (i = 0u; i < n * n * n; ++i) {
            const double q[3] = {((i % n) + 0.5) / n * 2.0 - 1.0, ((i / n % n) + 0.5) / n * 2.0 - 1.0, ((i / (n * n)) + 0.5) / n * 2.0 - 1.0};
            const double real = truth(shape, a->cloud[0], a->count[0], q, shape == 2u);
            double g[3];
            for (method = 0u; method < 3u; ++method) {
                const double v = pat_distance(a->cloud[0], a->count[0], q, (int)method, method == 0u ? g : NULL);
                error[method] += fabs(v - real) / (n * n * n);
                wrong[method] += (v < 0.0) != (real < 0.0);
            }
            eikonal += fabs(sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]) - 1.0) / (n * n * n);
            if (shape == 0u) walk = fmax(walk, fabs(truth(shape, a->cloud[0], a->count[0], q, true) - real));
            if (fabs(real) < 0.1) {
                /* The gradient of appendix D against central differences of the distance itself. */
                double numeric[3], diff = 0.0;
                for (c = 0u; c < 3u; ++c) {
                    double hi[3] = {q[0], q[1], q[2]}, lo[3] = {q[0], q[1], q[2]};
                    hi[c] += 1e-5; lo[c] -= 1e-5;
                    numeric[c] = (pat_distance(a->cloud[0], a->count[0], hi, PAT_TORI, NULL) -
                                  pat_distance(a->cloud[0], a->count[0], lo, PAT_TORI, NULL)) / 2e-5;
                    diff += (numeric[c] - g[c]) * (numeric[c] - g[c]);
                }
                slope += sqrt(diff);
                near_count += 1u;
            }
        }
        if (shape == 0u)
            demo_check(d, "the walk used as the blob's true distance, tried on the torus: largest error", walk, "< 1e-5", walk < 1e-5);
        snprintf(label, sizeof label, "%s, 2048 points, 32^3 grid: mean |distance - true|, tori", k_shapes[shape]);
        demo_check(d, label, error[0], "< 0.005", error[0] < 0.005);
        demo_check(d, "  tangent planes (the baseline of equation 29)", error[1], "> tori", error[1] > error[0]);
        demo_check(d, "  the nearest torus alone (reported)", error[2], "", true);
        demo_check(d, "  grid points with the wrong sign, of 32768: tori", wrong[0], "<= 16", wrong[0] <= 16.0);
        demo_check(d, "  tangent planes (reported)", wrong[1], "", true);
        demo_check(d, "  mean | |gradient| - 1 |, tori", eikonal, "< 0.05", eikonal < 0.05);
        demo_check(d, "  within 0.1 of the surface: mean |gradient of appendix D - central differences|", slope / near_count, "< 0.05",
                   slope / near_count < 0.05);
    }

    /* 4. The shaders against method.c: the slice view, every fourth pixel. */
    a->shape = 0u;
    a->other = 1u;
    a->slice = 0.1f;
    for (i = 0u; i < 4u; ++i) {
        static const uint32_t k_method[4] = {0u, 1u, 0u, 0u}, k_combine[4] = {0u, 0u, 1u, 3u};
        double worst = 0.0, worst_gradient = 0.0;
        a->method  = k_method[i];
        a->combine = k_combine[i];
        capture(a, 8u, px);
        for (y = 0u; y < h; y += step)
            for (x = 0u; x < w; x += step) {
                const float *p = &px[((size_t)y * w + x) * 4u];
                const double q[3] = {(((double)x + 0.5) / w * 2.0 - 1.0) * w / h * HALF, (1.0 - ((double)y + 0.5) / h * 2.0) * HALF, a->slice};
                double g[3], g2[3], v = pat_distance(a->cloud[0], a->count[0], q, (int)a->method, g), v2;
                if (a->combine != 0u) {
                    v2 = pat_distance(a->cloud[1], a->count[1], q, (int)a->method, g2);
                    if (a->combine == 3u) { v2 = -v2; g2[0] = -g2[0]; g2[1] = -g2[1]; g2[2] = -g2[2]; }
                    if ((a->combine == 1u) == (v2 < v)) { v = v2; g[0] = g2[0]; g[1] = g2[1]; g[2] = g2[2]; }
                }
                worst = fmax(worst, fabs(v - p[0]));
                /* Where the two clouds are equally near, a float and a double may pick differently; the distance agrees, the gradient need not. */
                if (a->combine == 0u)
                    for (c = 0u; c < 3u; ++c) worst_gradient = fmax(worst_gradient, fabs(g[c] - p[1u + c]));
            }
        snprintf(label, sizeof label, "GPU - CPU on a slice, %s, %s: largest difference in distance", k_methods[a->method],
                 a->combine == 0u ? "torus" : a->combine == 1u ? "torus or box" : "torus minus box");
        demo_check(d, label, worst, "< 1e-4", worst < 1e-4);
        if (a->combine == 0u) demo_check(d, "  in a component of the gradient", worst_gradient, "< 2e-3", worst_gradient < 2e-3);
    }
    a->method  = 0u;
    a->combine = 0u;

    /* 5. What the blending is for. Along a line through the cloud, in steps of 1e-5: the largest change
     * of the distance over a step, in units of the step. A true distance changes by at most 1. */
    {
        const double length = 1e-5;
        double before[2] = {0.0, 0.0}, jump[2] = {0.0, 0.0};
        for (i = 0u; i <= 200000u; ++i) {
            const double q[3] = {-1.0 + i * length, 0.37, 0.21};
            for (c = 0u; c < 2u; ++c) {
                const double v = pat_distance(a->cloud[0], a->count[0], q, c == 0u ? PAT_TORI : PAT_SNAP, NULL);
                if (i > 0u) jump[c] = fmax(jump[c], fabs(v - before[c]) / length);
                before[c] = v;
            }
        }
        demo_check(d, "largest change of the distance over a step of 1e-5 along a line / step: blended", jump[0], "< 2", jump[0] < 2.0);
        demo_check(d, "  the nearest torus alone: it jumps", jump[1], "> 10 x blended", jump[1] > 10.0 * jump[0]);
    }

    /* 6. Where the rays stop. The torus's distance is known, so a hit can be judged by itself. */
    for (i = 0u; i < 2u; ++i) {
        double mean = 0.0, worst = 0.0, hits = 0.0, apart = 0.0, differ = 0.0, off = 0.0, spent = 0.0, loose = 0.0;
        a->offset = i == 0u ? 0.0f : 0.1f;
        a->direct = false;
        capture(a, 9u, px);
        a->direct = true;
        capture(a, 9u, px2);
        a->direct = false;
        for (y = 0u; y < h; ++y)
            for (x = 0u; x < w; ++x) {
                const float *p = &px[((size_t)y * w + x) * 4u], *p2 = &px2[((size_t)y * w + x) * 4u];
                const double nx = ((double)x + 0.5) / w * 2.0 - 1.0, ny = 1.0 - ((double)y + 0.5) / h * 2.0;
                double dir[3], q[3], len, e;
                if (p[2] >= 200.0f || p2[2] >= 200.0f) { spent += 1.0; continue; }
                if ((p[0] >= 0.0f) != (p2[0] >= 0.0f)) differ += 1.0;
                if (p[0] < 0.0f) continue;
                for (c = 0u; c < 3u; ++c)
                    dir[c] = a->push.forward[c] + nx * a->push.right[3] * a->push.up[3] * a->push.right[c] + ny * a->push.right[3] * a->push.up[c];
                len = sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
                for (c = 0u; c < 3u; ++c) q[c] = a->push.eye[c] + dir[c] / len * p[0];
                e = fabs(implicit(0u, q) - a->offset);
                mean += e;
                worst = fmax(worst, e);
                off   = fmax(off, fabs((double)p[1]));
                if (fabs((double)p[1]) > 1.0001e-4) loose += 1.0;
                hits += 1.0;
                if (p2[0] >= 0.0f) apart = fmax(apart, fabs((double)p[0] - p2[0]));
            }
        snprintf(label, sizeof label, "rays that hit the torus's level %.1f (reported)", (double)a->offset);
        demo_check(d, label, hits, "", true);
        demo_check(d, "  rays that used up their 200 steps on either path, left out below", spent, "<= 0.001 of the picture", spent <= 0.001 * w * h);
        demo_check(d, "  hits where more than 1e-4 of distance is left (a step of the blend between two tori)", loose, "<= 0.01 of the hits",
                   loose <= 0.01 * hits);
        demo_check(d, "  the most that is left", off, "< 0.005", off < 0.005);
        demo_check(d, "  mean |true distance - level| at the hits", mean / hits, "< 0.003", mean / hits < 0.003);
        demo_check(d, "  largest (reported)", worst, "", true);
        demo_check(d, "  pixels where the ray through the volume and the all-pointwise ray disagree about hitting", differ, "<= 0.0005 of the picture",
                   differ <= 0.0005 * w * h);
        demo_check(d, "  largest difference between their depths (reported)", apart, "", true);
    }
    a->offset = 0.0f;
    free(px); free(px2); free(raw);
}

int main(int argc, char **argv) {
    static App app;
    static const DemoOption options[] = {
        {"--cloud", .u32 = &app.shape, .lo = 0, .hi = SHAPES - 1, .help = "0 torus, 1 box, 2 blob, 3 tooth, 4 snail"},
        {"--view", .u32 = &app.view, .lo = 0, .hi = 9, .help = "0 surface, 1 slice, 2 normals"},
        {"--blend", .u32 = &app.method, .lo = 0, .hi = 2, .help = "0 tori (the paper), 1 tangent planes, 2 the nearest torus only"},
        {"--offset", .f32 = &app.offset, .lo = -0.3, .hi = 0.5, .help = "the level of the distance that is drawn"},
        {"--slice", .f32 = &app.slice, .lo = -1.3, .hi = 1.3, .help = "z of the slice view"},
        {"--combine", .u32 = &app.combine, .lo = 0, .hi = 3, .help = "0 one cloud, 1 union, 2 intersection, 3 difference with --other"},
        {"--other", .u32 = &app.other, .lo = 0, .hi = SHAPES - 1, .help = "the second cloud"},
        {"--dots", .flag = &app.dots, .help = "draw the input points"},
        {"--direct", .flag = &app.direct, .help = "every step of a ray is a pointwise query; no baked volume"},
        {"--fit", .flag = &app.fit, .help = "run the network on the five clouds, write " DATA "*.pat, and stop"},
        {0}};
    demo_defaults(&app.demo, "pat", "Points as Tori - signed distance to point clouds");
    app.orbit = (DemoOrbit){.target = {0.0f, 0.0f, 0.0f}, .distance = 3.4f, .yaw = 0.6f, .pitch = 0.5f,
                            .fov_deg = 40.0f, .znear = 0.1f, .zfar = 20.0f, .min_distance = 1.5f, .max_distance = 10.0f};
    app.other    = 1u;
    app.dot_size = 0.006f;
    demo_open(&app.demo, options, argc, argv);
    if (app.fit) {
        fit(&app);
        return demo_close(&app.demo);
    }
    app_init(&app);
    while (demo_running(&app.demo)) {
        if (app.demo.ui) app_ui(&app, app.demo.ui);
        app_frame(&app);
    }
    if (app.demo.check) app_check(&app);
    return demo_close(&app.demo);
}
