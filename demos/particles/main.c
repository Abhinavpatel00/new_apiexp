/* particles — GPU particles: compute simulates, an indirect draw renders.
 *
 *   Simulate (compute: birth, curl-noise advection, compaction + vertex count)
 *     -> Draw (indirect, vertex pulling, additive, RGBA16F)
 *     -> Present
 *
 * The CPU never touches a particle. Per frame it uploads 16 bytes: the
 * indirect arguments with a zero vertex count, which the compute pass then
 * raises by six for every live particle.
 *
 * Life cycles run on integer milliseconds, so the CPU can predict how many
 * particles are alive without reading anything back; --check compares that
 * prediction with what the indirect draw actually rasterized.
 */
#include "demo.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SHADERS       "demos/particles/shaders/"
#define PARTICLE_SIZE 32u

typedef struct SimPush {
    uint32_t particles, compact, args, count;
    uint32_t now_ms, elapsed_ms, flags, pad;
    float    dt, time, curl_strength, curl_scale;
    float    drag, buoyancy, emit_radius, emit_speed;
} SimPush;
_Static_assert(sizeof(SimPush) == 64, "SimPush must match particles_abi.slang");

typedef struct DrawPush {
    float    vp[16], right_size[4], up_gain[4];
    uint32_t particles, compact, view, width;
    uint32_t height, pad[3];
} DrawPush;
_Static_assert(sizeof(DrawPush) == 128, "DrawPush must match particles_abi.slang");

/* The CPU's copy of the life cycle: timing only, no positions. */
typedef struct Timing { uint32_t life, period, offset, stored; } Timing;

typedef struct App {
    Demo      demo; /* first: see demo.h */
    DemoOrbit orbit;
    uint32_t  count;
    BufSlice  particles, compact, args;
    Target    hdr;
    Pipe      sim_pipe, draw_pipe;
    AttachDesc hdr_color;
    BufUse    sim_writes[3], draw_reads[3];
    SimPush   sim;
    DrawPush  draw;
    Timing   *timing;
    uint32_t  now_ms, live, fixed_ms;
    bool      reset;
    /* parameters */
    uint32_t  view;
    float     size, gain, curl_strength, curl_scale, drag, buoyancy, emit_radius, emit_speed;
} App;

static const char *const k_views[] = {"fire", "speed", "density"};

/* Mirrors hash32() and cycleOf() in particles_abi.slang. */
static uint32_t hash32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

/* One step of the life cycle for every particle; returns the live count the
 * compute pass must arrive at. */
static uint32_t predict_live(App *a, uint32_t now_ms, uint32_t elapsed_ms, bool reset) {
    uint32_t i, live = 0u;
    for (i = 0u; i < a->count; ++i) {
        Timing        *t     = &a->timing[i];
        const uint32_t time  = now_ms + t->offset;
        const uint32_t cycle = time / t->period, phase = time % t->period;
        if (reset) t->stored = 0u;
        if (phase < t->life && phase < elapsed_ms) t->stored = cycle + 1u;
        if (phase < t->life && t->stored == cycle + 1u) ++live;
    }
    return live;
}

/* One frame: what changed since the last one, then every pass, in the order app_init listed them. */
static void app_frame(App *a) {
    Demo *d = &a->demo;
    static const uint32_t k_args[4] = {0u, 1u, 0u, 0u}; /* vertexCount, instanceCount, first, first */
    float    view[16];
    uint32_t elapsed = a->fixed_ms;
    PassCtx *pass;

    if (!elapsed) {
        elapsed = (uint32_t)(d->dt * 1000.0f + 0.5f);
        if (elapsed < 1u) elapsed = 1u;
        if (elapsed > 50u) elapsed = 50u;
    }
    a->now_ms += elapsed;
    a->live = predict_live(a, a->now_ms, elapsed, a->reset);
    demo_upload(d, a->args, k_args, sizeof k_args);

    a->sim.particles     = (uint32_t)a->particles.offset;
    a->sim.compact       = (uint32_t)a->compact.offset;
    a->sim.args          = (uint32_t)a->args.offset;
    a->sim.count         = a->count;
    a->sim.now_ms        = a->now_ms;
    a->sim.elapsed_ms    = elapsed;
    a->sim.flags         = a->reset ? 1u : 0u;
    a->sim.dt            = (float)elapsed * 0.001f;
    a->sim.time          = (float)a->now_ms * 0.001f;
    a->sim.curl_strength = a->curl_strength;
    a->sim.curl_scale    = a->curl_scale;
    a->sim.drag          = a->drag;
    a->sim.buoyancy      = a->buoyancy;
    a->sim.emit_radius   = a->emit_radius;
    a->sim.emit_speed    = a->emit_speed;
    a->reset             = false;

    demo_orbit_input(d, &a->orbit);
    demo_orbit_camera(&a->orbit, (float)d->width / (float)d->height, view, NULL, a->draw.vp, NULL);
    a->draw.right_size[0] = view[0]; a->draw.right_size[1] = view[4]; a->draw.right_size[2] = view[8];
    a->draw.right_size[3] = a->size;
    a->draw.up_gain[0] = view[1]; a->draw.up_gain[1] = view[5]; a->draw.up_gain[2] = view[9];
    /* Brightness is per 2^18 particles, so --count does not change the exposure. */
    a->draw.up_gain[3] = a->gain * 262144.0f / (float)a->count;
    a->draw.particles  = (uint32_t)a->particles.offset;
    a->draw.compact    = (uint32_t)a->compact.offset;
    a->draw.view       = a->view;
    a->draw.width      = d->width;
    a->draw.height     = d->height;

    if (!demo_frame_begin(d)) return;
    pass = demo_pass(d, "Simulate");
    pass_push(pass, &a->sim, sizeof a->sim);
    pass_dispatch(pass, (a->count + 63u) / 64u, 1u, 1u);
    pass = demo_pass(d, "Draw");
    pass_push(pass, &a->draw, sizeof a->draw);
    /* The vertex count is whatever Simulate left in the buffer. */
    pass_draw_indirect(pass, a->args, 1u);
    demo_present(d);
    demo_frame_end(d);
}

static void app_init(App *a) {
    Demo    *d = &a->demo;
    ShaderId cs, vs, fs;
    PassDesc passes[4];
    uint32_t count = 0u, i;

    if (d->headless && !d->frames) d->frames = 300u; /* five seconds: let the plume form */

    a->timing = (Timing *)calloc(a->count, sizeof *a->timing);
    for (i = 0u; i < a->count; ++i) {
        Timing *t = &a->timing[i];
        t->life   = 2500u + hash32(i * 3u + 1u) % 3500u;
        t->period = t->life + 200u + hash32(i * 3u + 2u) % 1800u;
        t->offset = hash32(i * 3u + 3u) % t->period;
    }
    a->particles = demo_alloc(d, NULL, (uint64_t)a->count * PARTICLE_SIZE);
    a->compact   = demo_alloc(d, NULL, (uint64_t)a->count * 4u);
    a->args      = demo_alloc(d, NULL, 16u);
    a->reset = true;

    a->hdr = demo_target(d, "particles_hdr", FMT_RGBA16F, d->width, d->height, false, NULL);
    cs = demo_shader(d, SHADERS "simulate.comp.spv", STAGE_COMPUTE);
    vs = demo_shader(d, SHADERS "draw.vert.spv", STAGE_VERTEX);
    fs = demo_shader(d, SHADERS "draw.frag.spv", STAGE_FRAGMENT);
    a->sim_pipe  = pipe_intern_compute(d->gpu, cs, "main");
    a->draw_pipe = pipe_intern_graphics(d->gpu, vs, fs, FMT_RGBA16F, FMT_NONE, BLEND_ADD);

    a->sim_writes[0] = use_write(a->particles.buffer, a->particles.offset, a->particles.size);
    a->sim_writes[1] = use_write(a->compact.buffer, a->compact.offset, a->compact.size);
    a->sim_writes[2] = use_write(a->args.buffer, a->args.offset, a->args.size);
    a->draw_reads[0] = use_read(a->particles.buffer, a->particles.offset, a->particles.size);
    a->draw_reads[1] = use_read(a->compact.buffer, a->compact.offset, a->compact.size);
    a->draw_reads[0].stages = a->draw_reads[1].stages = SH_VERT;
    a->draw_reads[2] = use_indirect(a->args.buffer, a->args.offset, a->args.size);

    memset(passes, 0, sizeof passes);
    {
        PassDesc *p = &passes[count++];
        strcpy(p->name, "Simulate");
        p->kind            = PASS_COMPUTE;
        p->pipe            = a->sim_pipe;
        p->state           = draw_state_opaque(SC_1, CULL_NONE);
        p->buf_writes      = a->sim_writes;
        p->buf_write_count = 3u;
    }
    {
        PassDesc *p = &passes[count++];
        a->hdr_color = attach_clear(a->hdr, 0.0f, 0.0f, 0.0f, 0.0f);
        strcpy(p->name, "Draw");
        p->kind           = PASS_GRAPHICS;
        p->pipe           = a->draw_pipe;
        p->state          = draw_state_opaque(SC_1, CULL_NONE);
        p->state.blend    = BLEND_ADD;
        p->colors         = &a->hdr_color;
        p->color_count    = 1u;
        p->buf_reads      = a->draw_reads;
        p->buf_read_count = 3u;
    }
    passes[count++] = demo_present_pass(d, a->hdr);
    d->program = demo_program(d, passes, count);
}

static void app_shutdown(App *a) { free(a->timing); }

static void app_ui(App *a, struct nk_context *ctx) {
    Demo *d = &a->demo;
    if (nk_begin(ctx, "Particles", nk_rect(10, 10, 320, 400),
                 NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_TITLE)) {
        demo_ui_text(ctx, "%.1f ms", (double)d->dt * 1000.0);
        demo_ui_text(ctx, "%u particles, %u alive (predicted)", a->count, a->live);
        demo_ui_choice(ctx, "View", k_views, 3u, &a->view);
        demo_ui_float(ctx, "Flow strength", &a->curl_strength, 0.0f, 4.0f);
        demo_ui_float(ctx, "Flow scale", &a->curl_scale, 0.1f, 3.0f);
        demo_ui_float(ctx, "Drag", &a->drag, 0.0f, 8.0f);
        demo_ui_float(ctx, "Buoyancy", &a->buoyancy, -1.0f, 2.0f);
        demo_ui_float(ctx, "Emit speed", &a->emit_speed, 0.0f, 3.0f);
        demo_ui_float(ctx, "Emit radius", &a->emit_radius, 0.02f, 1.5f);
        demo_ui_float(ctx, "Sprite size", &a->size, 0.002f, 0.06f);
        demo_ui_float(ctx, "Brightness", &a->gain, 0.005f, 1.0f);
        nk_layout_row_dynamic(ctx, 24, 1);
        if (nk_button_label(ctx, "Restart")) a->reset = true;
    }
    nk_end(ctx);
}

/* ---- acceptance ----------------------------------------------------------- */

/* Run `frames` steps of 16 ms and return the state image of the last one:
 * pixel (id % w, id / w) = (position, 1) for every particle the draw drew. */
/* `frames` frames, then the HDR target as floats. */
static float *run_frames(App *a, uint32_t frames) {
    uint32_t f;
    for (f = 0u; f < frames; ++f) app_frame(a);
    return demo_read_floats(&a->demo, a->hdr, (size_t)a->demo.width * a->demo.height * 4u);
}

static void app_check(App *a) {
    Demo  *d = &a->demo;
    const size_t pixels = (size_t)d->width * d->height;
    float *first[2] = {NULL, NULL}, *px;
    char   label[96];
    uint32_t run, stage;
    size_t i;

    if ((size_t)a->count > pixels) demo_fatal(d, "--check needs one pixel per particle: lower --count or raise --size");
    a->view     = 3u;
    a->fixed_ms = 16u;
    for (run = 0u; run < 2u; ++run) {
        a->reset  = true;
        a->now_ms = 0u;
        for (stage = 0u; stage < 2u; ++stage) {
            double drawn = 0.0, doubled = 0.0, wild = 0.0, height = 0.0;
            px = run_frames(a, 150u);
            for (i = 0u; i < pixels; ++i) {
                const float *p = &px[i * 4u];
                if (p[3] == 0.0f) continue;
                drawn += p[3];
                if (p[3] != 1.0f) doubled += 1.0;
                if (!(fabsf(p[0]) < 40.0f && fabsf(p[1]) < 40.0f && fabsf(p[2]) < 40.0f)) wild += 1.0;
                height += p[1];
            }
            if (run == 0u) {
                snprintf(label, sizeof label, "frame %u: particles alive (CPU prediction)", (stage + 1u) * 150u);
                demo_check(d, label, (double)a->live, "between 10 % and 95 % of all",
                                 a->live > a->count / 10u && a->live < a->count / 100u * 95u);
                /* What Simulate left in the buffer: the indirect arguments and
                 * the compacted index list, read back directly. */
                {
                    uint32_t  args[4] = {0u, 0u, 0u, 0u};
                    uint32_t *list = (uint32_t *)malloc((size_t)a->count * 4u);
                    uint8_t  *seen = (uint8_t *)calloc(a->count, 1u);
                    uint32_t  k, bad = 0u;
                    demo_readback_buffer(d, a->args, args, sizeof args);
                    demo_readback_buffer(d, a->compact, list, (size_t)a->count * 4u);
                    /* The first `live` entries must be distinct particles. */
                    for (k = 0u; k < a->live && k < a->count; ++k) {
                        if (list[k] >= a->count || seen[list[k]]) ++bad;
                        else seen[list[k]] = 1u;
                    }
                    free(list);
                    free(seen);
                    demo_check(d, "  indirect args read back: vertexCount - 6 x predicted",
                                     (double)args[0] - 6.0 * (double)a->live, "== 0",
                                     args[0] == 6u * a->live && args[1] == 1u && args[2] == 0u && args[3] == 0u);
                    demo_check(d, "  compacted list: repeated or out-of-range entries", (double)bad, "== 0",
                                     bad == 0u);
                }
                demo_check(d, "  drawn by the indirect draw - predicted", drawn - (double)a->live, "== 0",
                                 drawn == (double)a->live);
                demo_check(d, "  pixels drawn more than once", doubled, "== 0", doubled == 0.0);
                demo_check(d, "  positions outside +-40 or not finite", wild, "== 0", wild == 0.0);
                demo_check(d, "  mean height (emitter at 0.3)", drawn > 0.0 ? height / drawn : 0.0, "> 0.5",
                                 drawn > 0.0 && height / drawn > 0.5);
                first[stage] = px;
            } else {
                size_t diff = 0u;
                for (i = 0u; i < pixels * 4u; ++i)
                    if (memcmp(&px[i], &first[stage][i], sizeof(float)) != 0) ++diff;
                snprintf(label, sizeof label, "frame %u, second run: values that differ", (stage + 1u) * 150u);
                demo_check(d, label, (double)diff, "== 0 (deterministic)", diff == 0u);
                free(px);
            }
        }
    }
    free(first[0]);
    free(first[1]);
}

int main(int argc, char **argv) {
    static App app;
    static const DemoOption options[] = {
        {"--count", .u32 = &app.count, .lo = 64, .hi = 1500000, .help = "particles; 1000000 fits"},
        {"--view", .u32 = &app.view, .lo = 0, .hi = 2, .help = "0 fire, 1 speed, 2 density"},
        {0}};
    demo_defaults(&app.demo, "particles", "Particles - compute simulation, indirect draw");
    app.orbit = (DemoOrbit){.target = {0.0f, 1.5f, 0.0f}, .distance = 6.5f, .yaw = 0.4f, .pitch = 0.12f,
                            .fov_deg = 50.0f, .znear = 0.05f, .zfar = 100.0f,
                            .min_distance = 1.0f, .max_distance = 40.0f};
    app.count         = 262144u;
    app.size          = 0.014f;
    app.gain          = 0.12f;
    app.curl_strength = 1.2f;
    app.curl_scale    = 0.8f;
    app.drag          = 2.5f;
    app.buoyancy      = 0.55f;
    app.emit_radius   = 0.25f;
    app.emit_speed    = 0.6f;
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
