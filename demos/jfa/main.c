/* jfa — distances to an arbitrary shape by jump flooding.
 *
 *   Init (compute: the shape) -> Flood x 9..11 (compute, steps 256 .. 1)
 *     -> Show -> Present
 *
 * The shape is redrawn and the whole field rebuilt every frame. --check
 * compares the result with the true closest cell, found on the CPU by trying
 * every boundary cell, for four orders of steps.
 */
#include "demo.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SHADERS    "demos/jfa/shaders/"
#define GRID_W     512u
#define GRID_H     288u
#define MAX_FLOODS 12u
#define VARIANTS   4u

typedef struct Push {
    uint32_t src, dst, view, pad;
    float    size[2];
    float    step, time;
} Push;
_Static_assert(sizeof(Push) == 32, "Push must match jfa.slang");

/* Steps per variant, 0-terminated. */
static const uint32_t k_steps[VARIANTS][MAX_FLOODS + 1u] = {
    {256, 128, 64, 32, 16, 8, 4, 2, 1, 0},
    {256, 128, 64, 32, 16, 8, 4, 2, 1, 1, 0},
    {256, 128, 64, 32, 16, 8, 4, 2, 1, 2, 1, 0},
    {1, 256, 128, 64, 32, 16, 8, 4, 2, 1, 0},
};
static const char *const k_variants[VARIANTS] = {"JFA", "JFA+1", "JFA+2", "1+JFA"};
static const char *const k_views[]            = {"distance", "closest cell", "outline and glow"};

typedef struct App {
    Demo          demo; /* first: see demo.h */
    Target        img[2], hdr;
    uint32_t      idx[2];
    Pipe          init_pipe, flood_pipe, show_pipe;
    AttachDesc    show_color;
    FrameProgram *programs[VARIANTS], *init_only;
    uint32_t      variant; /* which k_steps row the floods use */
    uint32_t      view, step;
    bool          still;
} App;

static uint32_t flood_count(uint32_t variant) {
    uint32_t n = 0u;
    while (k_steps[variant][n]) ++n;
    return n;
}

static void push(App *a, PassCtx *p, uint32_t src, uint32_t dst, float step) {
    Push push;
    memset(&push, 0, sizeof push);
    push.src = a->idx[src]; push.dst = a->idx[dst];
    push.view = a->view;
    push.size[0] = (float)GRID_W; push.size[1] = (float)GRID_H;
    push.step = step;
    push.time = (float)a->step / 60.0f;
    pass_push(p, &push, sizeof push);
}

static void seed(App *a, PassCtx *p) {
    push(a, p, 0u, 0u, 0.0f);
    pass_dispatch(p, GRID_W / 8u, GRID_H / 8u, 1u);
}

/* One frame: what changed since the last one, then every pass, in the order app_init listed them. */
static void app_frame(App *a) {
    Demo    *d = &a->demo;
    PassCtx *pass;
    uint32_t i;
    if (!a->still) ++a->step;
    d->present.tonemap = DEMO_TONEMAP_NONE;

    if (!demo_frame_begin(d)) return;
    seed(a, demo_pass(d, "Init"));
    /* Flood i reads image i & 1 and writes the other. */
    for (i = 0u; i < flood_count(a->variant); ++i) {
        pass = demo_pass(d, NULL);   /* "FloodA" / "FloodB" */
        push(a, pass, i & 1u, (i + 1u) & 1u, (float)k_steps[a->variant][i]);
        pass_dispatch(pass, GRID_W / 8u, GRID_H / 8u, 1u);
    }
    pass = demo_pass(d, "Show");
    push(a, pass, flood_count(a->variant) & 1u, 0u, 0.0f);
    pass_draw_fullscreen_triangle(pass, d->width, d->height);
    demo_present(d);
    demo_frame_end(d);
}

static void compute_image_pass(PassDesc *p, const char *name, Pipe pipe,
                         Target *read, Target *write) {
    memset(p, 0, sizeof *p);
    strcpy(p->name, name);
    p->kind   = PASS_COMPUTE;
    p->pipe   = pipe;
    p->state  = draw_state_opaque(SC_1, CULL_NONE);
    p->tex_reads  = read;  p->tex_read_count  = read ? 1u : 0u;
    p->tex_writes = write; p->tex_write_count = 1u;
}

static FrameProgram *build(App *a, uint32_t variant, bool init_only) {
    Demo    *d = &a->demo;
    PassDesc passes[MAX_FLOODS + 6u];
    uint32_t count = 0u, i, floods = init_only ? 0u : flood_count(variant);

    compute_image_pass(&passes[count++], "Init", a->init_pipe, NULL, &a->img[0]);
    for (i = 0u; i < floods; ++i)
        compute_image_pass(&passes[count++], (i & 1u) ? "FloodB" : "FloodA", a->flood_pipe, &a->img[i & 1u],
                     &a->img[(i + 1u) & 1u]);
    if (init_only) return frame_program_build(d->gpu, passes, count);
    {
        PassDesc *p = &passes[count++];
        memset(p, 0, sizeof *p);
        a->show_color = attach_color(a->hdr, ATT_WRITE);
        strcpy(p->name, "Show");
        p->kind  = PASS_GRAPHICS;
        p->pipe  = a->show_pipe;
        p->state = draw_state_opaque(SC_1, CULL_NONE);
        p->colors = &a->show_color;          p->color_count = 1u;
        p->tex_reads = &a->img[floods & 1u]; p->tex_read_count = 1u;
    }
    passes[count++] = demo_present_pass(d, a->hdr);
    return demo_program(d, passes, count);
}

static void app_init(App *a) {
    Demo    *d = &a->demo;
    ShaderId init_cs, flood_cs, show_fs;
    uint32_t i;

    a->img[0] = demo_target(d, "jfa_a", FMT_RGBA16F, GRID_W, GRID_H, true, &a->idx[0]);
    a->img[1] = demo_target(d, "jfa_b", FMT_RGBA16F, GRID_W, GRID_H, true, &a->idx[1]);
    a->hdr    = demo_target(d, "jfa_hdr", FMT_RGBA16F, d->width, d->height, false, NULL);
    init_cs  = demo_shader(d, SHADERS "init.comp.spv", STAGE_COMPUTE);
    flood_cs = demo_shader(d, SHADERS "flood.comp.spv", STAGE_COMPUTE);
    show_fs  = demo_shader(d, SHADERS "show.frag.spv", STAGE_FRAGMENT);
    a->init_pipe  = pipe_intern_compute(d->gpu, init_cs, "main");
    a->flood_pipe = pipe_intern_compute(d->gpu, flood_cs, "main");
    a->show_pipe  = pipe_intern_graphics(d->gpu, d->fullscreen_vs, show_fs, FMT_RGBA16F, FMT_NONE, BLEND_OPAQUE);
    for (i = 0u; i < VARIANTS; ++i) a->programs[i] = build(a, i, false);
    a->init_only = build(a, 0u, true);
    /* The variant shown is fixed at start; --check runs the others too. */
    d->program = a->programs[a->variant];
    if (d->headless && !d->frames) d->frames = 90u;
}

static void app_ui(App *a, struct nk_context *ctx) {
    Demo *d = &a->demo;
    if (nk_begin(ctx, "Jump flooding", nk_rect(10, 10, 300, 180),
                 NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_TITLE)) {
        demo_ui_text(ctx, "%.1f ms   %u x %u cells, %s, %u passes", (double)d->dt * 1000.0, GRID_W, GRID_H,
                     k_variants[a->variant], flood_count(a->variant));
        demo_ui_choice(ctx, "View", k_views, 3u, &a->view);
        demo_ui_bool(ctx, "Still", &a->still);
    }
    nk_end(ctx);
}

/* ---- acceptance ------------------------------------------------------------ */

static void read_grid(App *a, uint32_t which, float *out) {
    const size_t count = (size_t)GRID_W * GRID_H * 4u;
    float       *grid  = demo_read_floats(&a->demo, a->img[which], count);
    memcpy(out, grid, count * sizeof(float));
    free(grid);
}

/* True squared distance from every cell to the closest cell with
 * mask == want. That cell is always one with a neighbour of the other kind
 * (or on the grid's edge it need not be, so edge cells count as boundary
 * too), which keeps "try them all" affordable. */
static void brute_force(const uint8_t *mask, uint8_t want, int32_t *out, size_t *boundary_count) {
    int32_t *bx = (int32_t *)malloc(sizeof(int32_t) * GRID_W * GRID_H * 2u);
    size_t   n  = 0u, i, k;
    int32_t  x, y;
    for (y = 0; y < (int32_t)GRID_H; ++y)
        for (x = 0; x < (int32_t)GRID_W; ++x) {
            const size_t c = (size_t)y * GRID_W + (size_t)x;
            if (mask[c] != want) continue;
            if ((x > 0 && mask[c - 1u] != want) || (x < (int32_t)GRID_W - 1 && mask[c + 1u] != want) ||
                (y > 0 && mask[c - GRID_W] != want) || (y < (int32_t)GRID_H - 1 && mask[c + GRID_W] != want)) {
                bx[n * 2u] = x; bx[n * 2u + 1u] = y; ++n;
            }
        }
    for (i = 0u; i < (size_t)GRID_W * GRID_H; ++i) {
        int32_t best = INT32_MAX;
        x = (int32_t)(i % GRID_W); y = (int32_t)(i / GRID_W);
        if (mask[i] == want) best = 0;
        else
            for (k = 0u; k < n; ++k) {
                const int32_t dx = bx[k * 2u] - x, dy = bx[k * 2u + 1u] - y, d2 = dx * dx + dy * dy;
                if (d2 < best) best = d2;
            }
        out[i] = best;
    }
    *boundary_count = n;
    free(bx);
}

/* Shapes at these steps of the animation; the errors depend on the shape. */
static const uint32_t k_check_steps[] = {1u, 90u, 180u, 300u, 450u, 600u, 800u, 1000u};
#define CHECK_SHAPES (sizeof k_check_steps / sizeof k_check_steps[0])

static void app_check(App *a) {
    Demo          *d     = &a->demo;
    const size_t   cells = (size_t)GRID_W * GRID_H;
    float         *grid  = (float *)malloc(cells * 4u * sizeof(float));
    uint8_t       *mask  = (uint8_t *)malloc(cells);
    int32_t       *truth[2];
    size_t         i, boundary[2], most_boundary = 0u;
    size_t         bad_kind[VARIANTS] = {0}, unknown[VARIANTS] = {0}, most_wrong[VARIANTS] = {0};
    double         worst[VARIANTS] = {0};
    uint32_t       worst_at[VARIANTS] = {0}, large[VARIANTS] = {0};
    uint32_t       v, t;
    const uint32_t shown = a->variant, step = a->step;
    char           label[160];

    truth[0] = (int32_t *)malloc(cells * sizeof(int32_t));
    truth[1] = (int32_t *)malloc(cells * sizeof(int32_t));
    a->still = true; /* the runs below choose the shape themselves */

    for (t = 0u; t < CHECK_SHAPES; ++t) {
        a->step = k_check_steps[t];
        frame_begin(d->gpu);
        seed(a, frame_pass(d->gpu, a->init_only, "Init"));
        frame_end(d->gpu);
        read_grid(a, 0u, grid);
        for (i = 0u; i < cells; ++i) mask[i] = grid[i * 4u] >= 0.0f;
        brute_force(mask, 1u, truth[0], &boundary[0]);
        brute_force(mask, 0u, truth[1], &boundary[1]);
        if (boundary[0] + boundary[1] > most_boundary) most_boundary = boundary[0] + boundary[1];
        for (v = 0u; v < VARIANTS; ++v) {
            size_t wrong = 0u;
            double shape_worst = 0.0;
            a->variant = v;
            d->program = a->programs[v];
            app_frame(a);
            read_grid(a, flood_count(v) & 1u, grid);
            for (i = 0u; i < cells; ++i) {
                const int32_t x = (int32_t)(i % GRID_W), y = (int32_t)(i / GRID_W);
                uint32_t      k;
                for (k = 0u; k < 2u; ++k) {
                    const float   sx = grid[i * 4u + k * 2u], sy = grid[i * 4u + k * 2u + 1u];
                    const int32_t ix = (int32_t)sx, iy = (int32_t)sy;
                    int32_t       d2;
                    double        err;
                    if (sx < 0.0f) { ++unknown[v]; continue; }
                    if (ix >= (int32_t)GRID_W || iy >= (int32_t)GRID_H || iy < 0 ||
                        mask[(size_t)iy * GRID_W + (size_t)ix] != (k == 0u)) { ++bad_kind[v]; continue; }
                    d2 = (ix - x) * (ix - x) + (iy - y) * (iy - y);
                    if (d2 == truth[k][i]) continue;
                    ++wrong;
                    err = sqrt((double)d2) - sqrt((double)truth[k][i]);
                    if (err > shape_worst) shape_worst = err;
                }
            }
            if (wrong > most_wrong[v]) most_wrong[v] = wrong;
            if (shape_worst > worst[v]) { worst[v] = shape_worst; worst_at[v] = k_check_steps[t]; }
            if (shape_worst > 1.0) ++large[v];
        }
    }
    demo_check(d, "shapes tried (reported)", (double)CHECK_SHAPES, "", true);
    demo_check(d, "most boundary cells the CPU tried per cell (reported)", (double)most_boundary, "", true);
    for (v = 0u; v < VARIANTS; ++v) {
        /* The rules bind the variant the demo runs by default; the others are
         * there to be compared. */
        const bool   rule    = v == 3u;
        const double percent = 100.0 * (double)most_wrong[v] / (double)(cells * 2u);
        snprintf(label, sizeof label, "%s (%u passes): answers that name a cell of the wrong kind", k_variants[v], flood_count(v));
        demo_check(d, label, (double)bad_kind[v], "== 0", bad_kind[v] == 0u);
        snprintf(label, sizeof label, "%s: cells with no answer", k_variants[v]);
        demo_check(d, label, (double)unknown[v], "== 0", unknown[v] == 0u);
        snprintf(label, sizeof label, "%s: answers that are not the closest cell, worst shape, %% of %u", k_variants[v], (unsigned)(cells * 2u));
        demo_check(d, label, percent, rule ? "< 0.02" : "reported", !rule || percent < 0.02);
        snprintf(label, sizeof label, "%s: largest distance error, worst shape (cells)", k_variants[v]);
        demo_check(d, label, worst[v], rule ? "< 1" : "reported", !rule || worst[v] < 1.0);
        snprintf(label, sizeof label, "%s: the animation step of that shape (reported)", k_variants[v]);
        demo_check(d, label, (double)worst_at[v], "", true);
        snprintf(label, sizeof label, "%s: shapes with an error above 1 cell", k_variants[v]);
        demo_check(d, label, (double)large[v], rule ? "== 0" : "reported", !rule || large[v] == 0u);
    }
    a->variant = shown;
    d->program = a->programs[shown];
    a->step    = step;
    free(grid); free(mask); free(truth[0]); free(truth[1]);
}

int main(int argc, char **argv) {
    static App app;
    static const DemoOption options[] = {
        {"--view", .u32 = &app.view, .lo = 0, .hi = 2, .help = "0 distance, 1 closest cell, 2 outline and glow"},
        {"--variant", .u32 = &app.variant, .lo = 0, .hi = VARIANTS - 1, .help = "0 JFA, 1 JFA+1, 2 JFA+2, 3 1+JFA"},
        {"--still", .flag = &app.still, .help = "do not animate the shape"},
        {0}};
    demo_defaults(&app.demo, "jfa", "Jump flooding - distances to any shape");
    app.variant = 3u;
    demo_open(&app.demo, options, argc, argv);
    app_init(&app);
    while (demo_running(&app.demo)) {
        if (app.demo.ui) app_ui(&app, app.demo.ui);
        app_frame(&app);
    }
    if (app.demo.check) app_check(&app);
    return demo_close(&app.demo);
}
