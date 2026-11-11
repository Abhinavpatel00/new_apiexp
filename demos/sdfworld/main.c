/* sdfworld — a world made of distances that can be dug, built on and hit.
 *
 *   Edit (compute: brushes, in place, brush-sized boxes only)
 *     -> Occupied -> Reach x, y, z (compute: the empty-space level)
 *     -> Pick, Balls (compute: pointer ray; falling stones and clay)
 *     -> Prepass (1/8 resolution cone trace: start depths)
 *     -> Trace (sphere tracing, soft shadow, AO, RGBA16F)
 *     -> Present
 *
 * After Claybook (Aaltonen, GDC 2018): a band-limited distance volume, local
 * edits, a coarse level for empty space, a cone-trace pre-pass. The volume
 * is a 128 x 64 x 128 RGBA16F 3D texture of 25 cm voxels. Nothing here is a
 * triangle.
 *
 * The CPU never reads the world. Brushes placed with the pointer and the
 * impacts of the balls reach the Edit pass through the storage buffer: the
 * centre, and the arguments of an indirect dispatch that are zero when
 * there is nothing to do.
 */
#include "demo.h"

#include "mu_math.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SHADERS   "demos/sdfworld/shaders/"
#define VX        128u
#define VY        64u
#define VZ        128u
#define CELL      4u
#define CX        (VX / CELL)
#define CY        (VY / CELL)
#define CZ        (VZ / CELL)
#define REACH     8
#define BAND      4.0f
#define VOXEL     0.25f                     /* metres */
#define BLOCK     8u
#define MAX_BALLS 32u
#define MAX_EDITS 8u
#define STONE_RADIUS 3.0f
#define STONE_K      1.2f
#define CLAY_RADIUS  2.0f
#define CLAY_K       1.0f

typedef struct FrameData {
    float    eye[4], right[4], up[4], forward[4], sun[4], size[4], vol[4], relax[4];
    uint32_t index[4], options[4];
    float    mouse[4];
    uint32_t balls[4];
} FrameData;
_Static_assert(sizeof(FrameData) == 192, "FrameData must match world_common.slang");

typedef struct EditPush {
    uint32_t target, op, slot, flags;
    float    centre_radius[4], color_k[4];
    int32_t  base[4];
    uint32_t dims_tiles[4];
    float    band_voxel[4];
} EditPush;
_Static_assert(sizeof(EditPush) == 96, "EditPush must match edit.slang");

typedef struct CoarsePush {
    uint32_t source, target, pad_a, pad_b, dims[4];
    float    band;
    uint32_t axis, pad[2];
} CoarsePush;
typedef struct SimPush { uint32_t frame_offset, pick_offset, args_offset, pad, groups[4]; } SimPush;

typedef struct Ball { float pos_radius[4], vel_state[4]; } Ball;
typedef struct Edit { uint32_t op; float centre[3], radius, k, color[3]; } Edit;
enum { OP_GENERATE = 0, OP_ADD = 1, OP_DIG = 2 };

typedef struct App {
    Demo      demo; /* first: see demo.h */
    DemoOrbit orbit;
    BufSlice  frame_data, pick_data, ball_data, impact_data;
    BufSlice  args_data;        /* MAX_BALLS + 1 dispatch arguments, 16 bytes apart; the last is the pointer's */
    Target    fine, occupied, reach, prepass, hdr;
    uint32_t  fine_index, occupied_index, reach_index, prepass_index;
    Pipe      edit_pipe, occupied_pipe, reach_pipe, pick_pipe, balls_pipe, prepass_pipe, trace_pipe;
    AttachDesc prepass_color, trace_color;
    Target    sim_reads[2], trace_reads[3];
    BufUse    edit_reads[3], frame_read, pick_writes[2], balls_writes[3], trace_buf_reads[2];
    FrameData frame;
    Edit      edits[MAX_EDITS], extra;      /* extra: one brush from --check */
    uint32_t  edit_count, step, budget;
    bool      has_extra;
    bool      generated;
    /* parameters */
    uint32_t  view, tool, ball_count;
    bool      auto_sculpt, use_coarse, use_prepass, picking;
    bool      still;        /* --still */
    float     sun_yaw, sun_height, brush_radius;
    float     relax;            /* over-relaxation factor; 1 = plain sphere tracing */
} App;

static const char *const k_views[] = {"lit", "steps per ray", "normal", "light only", "start depth",
                                      "light only, plain shadow"};
static const char *const k_tools[] = {"orbit", "dig", "add clay"};
#define VIEW_COUNT 6u

/* The colour of ball i; must match ballColor() in world_common.slang. */
static void ball_color(uint32_t i, float *rgb) {
    const float h = (float)i * 0.618034f - floorf((float)i * 0.618034f);
    uint32_t    k;
    for (k = 0u; k < 3u; ++k) {
        float t = h + (float)k / 3.0f;
        t -= floorf(t);
        t = fabsf(t * 6.0f - 3.0f) - 1.0f;
        rgb[k] = (t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t) * 0.75f + 0.15f;
    }
}

/* Height of the starting ground at voxel column (x, z), in voxels: the same
 * formula as terrain() in edit.slang. Only the scripted brushes use it, to
 * stay near the surface. */
static float ground_voxels(float x, float z) {
    const float mx = x * VOXEL - 0.5f * VX * VOXEL, mz = z * VOXEL - 0.5f * VZ * VOXEL;
    return (3.2f + 1.1f * sinf(0.33f * mx) * cosf(0.29f * mz) + 0.5f * sinf(0.85f * mx + 1.3f) * sinf(0.8f * mz + 0.4f)) / VOXEL;
}

static void queue(App *a, uint32_t op, float x, float y, float z, float radius, float k, const float *rgb) {
    Edit *e;
    if (a->edit_count >= MAX_EDITS) return;
    e = &a->edits[a->edit_count++];
    e->op = op; e->centre[0] = x; e->centre[1] = y; e->centre[2] = z; e->radius = radius; e->k = k;
    e->color[0] = rgb ? rgb[0] : 0.0f; e->color[1] = rgb ? rgb[1] : 0.0f; e->color[2] = rgb ? rgb[2] : 0.0f;
}

/* ---- per frame ------------------------------------------------------------ */

/* ---- passes --------------------------------------------------------------- */

static uint32_t box_half(float radius, float k) { return (uint32_t)ceilf(radius + BAND + k); }

static void edit_push(const App *a, EditPush *push, uint32_t op, float radius, float k, const float *rgb) {
    memset(push, 0, sizeof *push);
    push->target = a->fine_index;
    push->op     = op;
    push->centre_radius[3] = radius;
    if (rgb) memcpy(push->color_k, rgb, 3u * sizeof(float));
    push->color_k[3]    = k;
    push->base[3]       = (int32_t)box_half(radius, k);
    push->dims_tiles[0] = VX; push->dims_tiles[1] = VY; push->dims_tiles[2] = VZ;
    push->band_voxel[0] = BAND; push->band_voxel[1] = VOXEL;
}

/* Groups of 4^3 threads covering a box of 2 * half + 1 voxels. */
static uint32_t box_groups(uint32_t half) { return (2u * half + 1u + 3u) / 4u; }

static void dispatch_edits(App *a, PassCtx *p) {
    EditPush push;
    uint32_t i, g;
    /* Brushes the CPU placed. */
    for (i = 0u; i < a->edit_count; ++i) {
        const Edit *e = &a->edits[i];
        edit_push(a, &push, e->op, e->radius, e->k, e->color);
        if (e->op == OP_GENERATE) {
            pass_push(p, &push, sizeof push);
            pass_dispatch(p, VX / 4u, VY / 4u, VZ / 4u);
            continue;
        }
        memcpy(push.centre_radius, e->centre, 3u * sizeof(float));
        push.base[0] = (int32_t)floorf(e->centre[0]) - push.base[3];
        push.base[1] = (int32_t)floorf(e->centre[1]) - push.base[3];
        push.base[2] = (int32_t)floorf(e->centre[2]) - push.base[3];
        g = box_groups((uint32_t)push.base[3]);
        pass_push(p, &push, sizeof push);
        pass_dispatch(p, g, g, g);
    }
    /* Brushes the GPU placed last frame: the pointer's, and one per ball. The
     * centre is in the storage buffer, and so are the dispatch's arguments:
     * the brush's box, or zero groups when there was no hit. */
    if (a->tool != 0u) {
        static const float clay[3] = {0.80f, 0.42f, 0.30f};
        edit_push(a, &push, a->tool == 1u ? OP_DIG : OP_ADD, a->brush_radius, 1.5f, clay);
        push.flags = 1u;
        push.slot  = (uint32_t)a->pick_data.offset;
        pass_push(p, &push, sizeof push);
        pass_dispatch_indirect(p, (BufSlice){a->args_data.buffer, a->args_data.offset + MAX_BALLS * 16u, 12u});
    }
    for (i = 0u; i < a->ball_count; ++i) {
        float rgb[3];
        ball_color(i, rgb);
        /* Even balls are stones and leave a crater; odd ones are clay and stay. */
        if (i & 1u) edit_push(a, &push, OP_ADD, CLAY_RADIUS, CLAY_K, rgb);
        else        edit_push(a, &push, OP_DIG, STONE_RADIUS, STONE_K, NULL);
        push.flags = 1u;
        push.slot  = (uint32_t)a->impact_data.offset + i * 16u;
        pass_push(p, &push, sizeof push);
        pass_dispatch_indirect(p, (BufSlice){a->args_data.buffer, a->args_data.offset + i * 16u, 12u});
    }
}

static void dispatch_coarse(PassCtx *p, uint32_t source, uint32_t target, uint32_t axis) {
    CoarsePush push;
    memset(&push, 0, sizeof push);
    push.source = source; push.target = target;
    push.dims[0] = VX; push.dims[1] = VY; push.dims[2] = VZ; push.dims[3] = CELL;
    push.band = BAND;
    push.axis = axis;
    pass_push(p, &push, sizeof push);
    pass_dispatch(p, CX / 4u, CY / 4u, CZ / 4u);
}

static void sim_push(const App *a, PassCtx *p) {
    SimPush push;
    memset(&push, 0, sizeof push);
    push.frame_offset = (uint32_t)a->frame_data.offset;
    push.pick_offset  = (uint32_t)a->pick_data.offset;
    push.args_offset  = (uint32_t)a->args_data.offset;
    push.groups[0]    = box_groups(box_half(STONE_RADIUS, STONE_K));
    push.groups[1]    = box_groups(box_half(CLAY_RADIUS, CLAY_K));
    push.groups[2]    = box_groups(box_half(a->brush_radius, 1.5f));
    pass_push(p, &push, sizeof push);
}
/* One frame: what changed since the last one, then every pass, in the order app_init listed them. */
static void app_frame(App *a) {
    Demo      *d = &a->demo;
    FrameData *f = &a->frame;
    float      view[16], eye[3];
    const float aspect = (float)d->width / (float)d->height;
    const float tan_y  = tanf(a->orbit.fov_deg * 0.017453292f * 0.5f);
    bool        sculpting = false;
    uint32_t    i;
    PassCtx *pass;

    if (d->platform) {
        const MuInputState *s = platform_state(d->platform);
        sculpting = a->tool != 0u && !d->ui_hovered && mu_input_mouse_down(s, MU_MOUSE_LEFT);
        if (sculpting) {
            f->mouse[0] = s->mouse_x / (float)d->width * 2.0f - 1.0f;
            f->mouse[1] = 1.0f - s->mouse_y / (float)d->height * 2.0f;
        }
    }
    if (a->picking) sculpting = true;   /* --check drives the pointer itself */
    f->mouse[2] = sculpting ? 1.0f : 0.0f;
    if (!sculpting) demo_orbit_input(d, &a->orbit);
    demo_orbit_camera(&a->orbit, aspect, view, NULL, NULL, eye);
    /* Everything the shaders see is in voxels: the volume's corner is the origin. */
    f->eye[0] = (eye[0] + 0.5f * VX * VOXEL) / VOXEL;
    f->eye[1] = eye[1] / VOXEL;
    f->eye[2] = (eye[2] + 0.5f * VZ * VOXEL) / VOXEL;
    for (i = 0u; i < 3u; ++i) {
        f->right[i]   = view[i * 4u] * tan_y * aspect;
        f->up[i]      = view[i * 4u + 1u] * tan_y;
        f->forward[i] = -view[i * 4u + 2u];
    }
    f->sun[0] = cosf(a->sun_height) * sinf(a->sun_yaw);
    f->sun[1] = sinf(a->sun_height);
    f->sun[2] = cosf(a->sun_height) * cosf(a->sun_yaw);
    f->sun[3] = 2.6f;
    f->size[0] = (float)d->width; f->size[1] = (float)d->height;
    f->size[2] = tan_y / (float)d->height;   /* tan of one pixel's half-angle */
    f->size[3] = 1.0f / 60.0f;               /* the balls' time step: fixed, so runs repeat */
    f->vol[0] = (float)VX; f->vol[1] = (float)VY; f->vol[2] = (float)VZ; f->vol[3] = BAND;
    f->index[0] = a->fine_index; f->index[1] = a->reach_index; f->index[2] = d->linear_clamp_index;
    f->index[3] = a->prepass_index;
    f->options[0] = a->view; f->options[1] = a->use_coarse ? 1u : 0u; f->options[2] = a->use_prepass ? 1u : 0u;
    f->options[3] = a->budget;
    f->relax[0]   = a->relax;
    f->balls[0] = (uint32_t)a->ball_data.offset; f->balls[1] = a->ball_count;
    f->balls[2] = (uint32_t)a->impact_data.offset; f->balls[3] = a->step;
    demo_upload(d, a->frame_data, f, sizeof *f);

    /* This frame's brushes. The first frame generates the world. */
    a->edit_count = 0u;
    if (!a->generated) {
        queue(a, OP_GENERATE, 0, 0, 0, 0, 0, NULL);
        a->generated = true;
    } else if (a->auto_sculpt) {
        /* A digger wandering over the ground and a builder winding a tower. */
        const float t  = (float)a->step;
        const float dx = 64.0f + 44.0f * sinf(0.021f * t), dz = 64.0f + 44.0f * sinf(0.033f * t + 1.0f);
        const float bx = 96.0f + 7.0f * cosf(0.13f * t), bz = 34.0f + 7.0f * sinf(0.13f * t);
        float       rgb[3], by = ground_voxels(96.0f, 34.0f) + 1.0f + 0.07f * t;
        queue(a, OP_DIG, dx, ground_voxels(dx, dz) - 0.5f, dz, 3.4f, 1.5f, NULL);
        if ((a->step & 1u) == 0u && by < (float)VY - 8.0f) {
            ball_color(1u + 2u * (a->step / 24u), rgb);
            queue(a, OP_ADD, bx, by, bz, 2.6f, 1.2f, rgb);
        }
    }
    if (a->has_extra && a->edit_count < MAX_EDITS) a->edits[a->edit_count++] = a->extra;
    a->has_extra = false;
    ++a->step;
    d->present.tonemap = (a->view == 0u || a->view == 3u || a->view == 5u) ? DEMO_TONEMAP_ACES_FITTED : DEMO_TONEMAP_NONE;

    if (!demo_frame_begin(d)) return;
    dispatch_edits(a, demo_pass(d, "Edit"));
    /* The two coarse images take turns: occupied -> reach -> occupied -> reach. */
    dispatch_coarse(demo_pass(d, "Occupied"), a->fine_index, a->occupied_index, 0u);
    dispatch_coarse(demo_pass(d, "ReachX"), a->occupied_index, a->reach_index, 0u);
    dispatch_coarse(demo_pass(d, "ReachY"), a->reach_index, a->occupied_index, 1u);
    dispatch_coarse(demo_pass(d, "ReachZ"), a->occupied_index, a->reach_index, 2u);

    pass = demo_pass(d, "Pick");
    sim_push(a, pass);
    pass_dispatch(pass, 1u, 1u, 1u);

    pass = demo_pass(d, "Balls");
    if (a->ball_count) {
        sim_push(a, pass);
        pass_dispatch(pass, (a->ball_count + 31u) / 32u, 1u, 1u);
    }

    pass = demo_pass(d, "Prepass");
    sim_push(a, pass);
    pass_draw_fullscreen_triangle(pass, (d->width + BLOCK - 1u) / BLOCK, (d->height + BLOCK - 1u) / BLOCK);

    pass = demo_pass(d, "Trace");
    sim_push(a, pass);
    pass_draw_fullscreen_triangle(pass, d->width, d->height);
    demo_present(d);
    demo_frame_end(d);
}

/* ---- setup ---------------------------------------------------------------- */

static void reset_balls(App *a) {
    Demo *d = &a->demo;
    Ball  balls[MAX_BALLS];
    float zero[(MAX_BALLS + 1u) * 4u];
    uint32_t i;
    memset(zero, 0, sizeof zero);
    for (i = 0u; i < MAX_BALLS; ++i) {
        /* Spread over the sky at different heights, so impacts are spread in time. */
        const float fx = (float)((i * 37u) % 97u) / 97.0f, fz = (float)((i * 61u) % 89u) / 89.0f;
        balls[i].pos_radius[0] = 10.0f + fx * ((float)VX - 20.0f);
        balls[i].pos_radius[1] = (float)VY - 2.0f - (float)(i % 8u) * 3.0f;
        balls[i].pos_radius[2] = 10.0f + fz * ((float)VZ - 20.0f);
        balls[i].pos_radius[3] = (i & 1u) ? 1.5f : 1.9f;
        balls[i].vel_state[0] = (fz - 0.5f) * 10.0f;
        balls[i].vel_state[1] = -6.0f - (float)(i % 5u) * 2.0f;
        balls[i].vel_state[2] = (fx - 0.5f) * 10.0f;
        balls[i].vel_state[3] = 0.0f;
    }
    demo_upload(d, a->ball_data, balls, sizeof balls);
    demo_upload(d, a->impact_data, zero, MAX_BALLS * 16u);
    demo_upload(d, a->pick_data, zero, 16u);
    demo_upload(d, a->args_data, zero, (MAX_BALLS + 1u) * 16u);
}

static void compute_pass(PassDesc *p, const char *name, Pipe pipe) {
    memset(p, 0, sizeof *p);
    strcpy(p->name, name);
    p->kind   = PASS_COMPUTE;
    p->pipe   = pipe;
    p->state  = draw_state_opaque(SC_1, CULL_NONE);
}

static void app_init(App *a) {
    Demo     *d = &a->demo;
    ShaderId  edit_cs, occupied_cs, reach_cs, pick_cs, balls_cs, prepass_fs, trace_fs;
    PassDesc  passes[12];
    uint32_t  count = 0u;
    const uint32_t pw = (d->width + BLOCK - 1u) / BLOCK, ph = (d->height + BLOCK - 1u) / BLOCK;

    if (a->still) { a->auto_sculpt = false; a->ball_count = 0u; }

    a->frame_data  = demo_alloc(d, NULL, sizeof(FrameData));
    a->pick_data   = demo_alloc(d, NULL, 16u);
    a->ball_data   = demo_alloc(d, NULL, MAX_BALLS * sizeof(Ball));
    a->impact_data = demo_alloc(d, NULL, MAX_BALLS * 16u);
    a->args_data   = demo_alloc(d, NULL, (MAX_BALLS + 1u) * 16u);
    reset_balls(a);

    a->fine     = demo_volume(d, "sdf_volume", FMT_RGBA16F, VX, VY, VZ, &a->fine_index);
    a->occupied = demo_volume(d, "sdf_occupied", FMT_RGBA16F, CX, CY, CZ, &a->occupied_index);
    a->reach    = demo_volume(d, "sdf_reach", FMT_RGBA16F, CX, CY, CZ, &a->reach_index);
    a->prepass  = demo_target(d, "sdf_prepass", FMT_R32F, pw, ph, false, &a->prepass_index);
    a->hdr      = demo_target(d, "sdf_hdr", FMT_RGBA16F, d->width, d->height, false, NULL);

    edit_cs     = demo_shader(d, SHADERS "edit.comp.spv", STAGE_COMPUTE);
    occupied_cs = demo_shader(d, SHADERS "occupied.comp.spv", STAGE_COMPUTE);
    reach_cs    = demo_shader(d, SHADERS "reach.comp.spv", STAGE_COMPUTE);
    pick_cs     = demo_shader(d, SHADERS "pick.comp.spv", STAGE_COMPUTE);
    balls_cs    = demo_shader(d, SHADERS "balls.comp.spv", STAGE_COMPUTE);
    prepass_fs  = demo_shader(d, SHADERS "prepass.frag.spv", STAGE_FRAGMENT);
    trace_fs    = demo_shader(d, SHADERS "trace.frag.spv", STAGE_FRAGMENT);
    a->edit_pipe     = pipe_intern_compute(d->gpu, edit_cs, "main");
    a->occupied_pipe = pipe_intern_compute(d->gpu, occupied_cs, "main");
    a->reach_pipe    = pipe_intern_compute(d->gpu, reach_cs, "main");
    a->pick_pipe     = pipe_intern_compute(d->gpu, pick_cs, "main");
    a->balls_pipe    = pipe_intern_compute(d->gpu, balls_cs, "main");
    a->prepass_pipe  = pipe_intern_graphics(d->gpu, d->fullscreen_vs, prepass_fs, FMT_R32F, FMT_NONE, BLEND_OPAQUE);
    a->trace_pipe    = pipe_intern_graphics(d->gpu, d->fullscreen_vs, trace_fs, FMT_RGBA16F, FMT_NONE, BLEND_OPAQUE);

    a->frame_read = use_read(a->frame_data.buffer, a->frame_data.offset, a->frame_data.size);
    a->frame_read.stages = SH_COMP | SH_FRAG;
    a->edit_reads[0] = use_read(a->pick_data.buffer, a->pick_data.offset, a->pick_data.size);
    a->edit_reads[1] = use_read(a->impact_data.buffer, a->impact_data.offset, a->impact_data.size);
    a->edit_reads[2] = use_indirect(a->args_data.buffer, a->args_data.offset, a->args_data.size);
    a->edit_reads[0].stages = a->edit_reads[1].stages = a->edit_reads[2].stages = SH_COMP;
    a->pick_writes[0] = use_write(a->pick_data.buffer, a->pick_data.offset, a->pick_data.size);
    a->pick_writes[1] = use_write(a->args_data.buffer, a->args_data.offset + MAX_BALLS * 16u, 16u);
    a->pick_writes[0].stages = a->pick_writes[1].stages = SH_COMP;
    a->balls_writes[0] = use_write(a->ball_data.buffer, a->ball_data.offset, a->ball_data.size);
    a->balls_writes[1] = use_write(a->impact_data.buffer, a->impact_data.offset, a->impact_data.size);
    a->balls_writes[2] = use_write(a->args_data.buffer, a->args_data.offset, MAX_BALLS * 16u);
    a->balls_writes[0].stages = a->balls_writes[1].stages = a->balls_writes[2].stages = SH_COMP;
    a->trace_buf_reads[0] = a->frame_read;
    a->trace_buf_reads[1] = use_read(a->ball_data.buffer, a->ball_data.offset, a->ball_data.size);
    a->trace_buf_reads[1].stages = SH_FRAG;
    a->sim_reads[0] = a->fine; a->sim_reads[1] = a->reach;
    a->trace_reads[0] = a->fine; a->trace_reads[1] = a->reach; a->trace_reads[2] = a->prepass;

    /* Brushes first - including those the GPU asked for last frame - then
     * the coarse level, then everything that reads the world. */
    {
        PassDesc *p = &passes[count++];
        compute_pass(p, "Edit", a->edit_pipe);
        p->tex_writes = &a->fine;      p->tex_write_count = 1u;
        p->buf_reads  = a->edit_reads; p->buf_read_count  = 3u;
    }
    {
        PassDesc *p = &passes[count++];
        compute_pass(p, "Occupied", a->occupied_pipe);
        p->tex_reads  = &a->fine;     p->tex_read_count  = 1u;
        p->tex_writes = &a->occupied; p->tex_write_count = 1u;
    }
    {
        PassDesc *p = &passes[count++];
        compute_pass(p, "ReachX", a->reach_pipe);
        p->tex_reads  = &a->occupied; p->tex_read_count  = 1u;
        p->tex_writes = &a->reach;    p->tex_write_count = 1u;
    }
    {
        PassDesc *p = &passes[count++];
        compute_pass(p, "ReachY", a->reach_pipe);
        p->tex_reads  = &a->reach;    p->tex_read_count  = 1u;
        p->tex_writes = &a->occupied; p->tex_write_count = 1u;
    }
    {
        PassDesc *p = &passes[count++];
        compute_pass(p, "ReachZ", a->reach_pipe);
        p->tex_reads  = &a->occupied; p->tex_read_count  = 1u;
        p->tex_writes = &a->reach;    p->tex_write_count = 1u;
    }
    {
        PassDesc *p = &passes[count++];
        compute_pass(p, "Pick", a->pick_pipe);
        p->tex_reads  = a->sim_reads;   p->tex_read_count  = 2u;
        p->buf_reads  = &a->frame_read; p->buf_read_count  = 1u;
        p->buf_writes = a->pick_writes; p->buf_write_count = 2u;
    }
    {
        PassDesc *p = &passes[count++];
        compute_pass(p, "Balls", a->balls_pipe);
        p->tex_reads  = a->sim_reads;    p->tex_read_count  = 2u;
        p->buf_reads  = &a->frame_read;  p->buf_read_count  = 1u;
        p->buf_writes = a->balls_writes; p->buf_write_count = 3u;
    }
    {
        PassDesc *p = &passes[count++];
        memset(p, 0, sizeof *p);
        a->prepass_color = attach_color(a->prepass, ATT_WRITE);
        strcpy(p->name, "Prepass");
        p->kind  = PASS_GRAPHICS;
        p->pipe  = a->prepass_pipe;
        p->state = draw_state_opaque(SC_1, CULL_NONE);
        p->colors = &a->prepass_color; p->color_count = 1u;
        p->tex_reads = a->sim_reads;   p->tex_read_count = 2u;
        p->buf_reads = &a->frame_read; p->buf_read_count = 1u;
    }
    {
        PassDesc *p = &passes[count++];
        memset(p, 0, sizeof *p);
        a->trace_color = attach_color(a->hdr, ATT_WRITE);
        strcpy(p->name, "Trace");
        p->kind  = PASS_GRAPHICS;
        p->pipe  = a->trace_pipe;
        p->state = draw_state_opaque(SC_1, CULL_NONE);
        p->colors = &a->trace_color;        p->color_count = 1u;
        p->tex_reads = a->trace_reads;      p->tex_read_count = 3u;
        p->buf_reads = a->trace_buf_reads;  p->buf_read_count = 2u;
    }
    passes[count++] = demo_present_pass(d, a->hdr);
    d->program = demo_program(d, passes, count);
    /* Headless: let the digger, the builder and the balls work for a while. */
    if (d->headless && !d->frames) d->frames = 240u;
}

static void app_ui(App *a, struct nk_context *ctx) {
    Demo *d = &a->demo;
    if (nk_begin(ctx, "SDF world", nk_rect(10, 10, 330, 400),
                 NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_TITLE)) {
        float balls = (float)a->ball_count;
        demo_ui_text(ctx, "%.1f ms   %u x %u x %u voxels", (double)d->dt * 1000.0, VX, VY, VZ);
        demo_ui_choice(ctx, "Left button", k_tools, 3u, &a->tool);
        demo_ui_float(ctx, "Brush radius (voxels)", &a->brush_radius, 1.5f, 8.0f);
        demo_ui_bool(ctx, "Digger and builder", &a->auto_sculpt);
        demo_ui_float(ctx, "Falling balls", &balls, 0.0f, (float)MAX_BALLS);
        a->ball_count = (uint32_t)balls;
        demo_ui_choice(ctx, "View", k_views, VIEW_COUNT, &a->view);
        demo_ui_bool(ctx, "Coarse level (skip empty space)", &a->use_coarse);
        demo_ui_bool(ctx, "Cone-trace pre-pass", &a->use_prepass);
        demo_ui_float(ctx, "Over-relaxation", &a->relax, 1.0f, 1.9f);
        demo_ui_float(ctx, "Sun yaw", &a->sun_yaw, -3.14f, 3.14f);
        demo_ui_float(ctx, "Sun height", &a->sun_height, 0.15f, 1.5f);
    }
    nk_end(ctx);
}

#include "check.inc"

int main(int argc, char **argv) {
    static App app;
    static const DemoOption options[] = {
        {"--view", .u32 = &app.view, .lo = 0, .hi = VIEW_COUNT - 1, .help = "0 lit, 1 steps per ray, 2 normal, 3 light only, 4 start depth, 5 light only with the plain shadow estimate"},
        {"--balls", .u32 = &app.ball_count, .lo = 0, .hi = MAX_BALLS, .help = "falling stones and clay"},
        {"--no-coarse", .off = &app.use_coarse, .help = "trace without the empty-space level"},
        {"--no-prepass", .off = &app.use_prepass, .help = "trace without the cone-trace pre-pass"},
        {"--relax", .f32 = &app.relax, .lo = 1, .hi = 1.999, .help = "over-relaxed steps; 1 = plain"},
        {"--still", .flag = &app.still, .help = "no digger, no builder, no balls: the starting world"},
        {0}};
    demo_defaults(&app.demo, "sdfworld", "SDF world - a deformable distance volume");
    app.demo.note = "headless runs 240 frames before the capture; --frames N changes that";
    app.orbit = (DemoOrbit){.target = {0.0f, 3.0f, 0.0f}, .distance = 34.0f, .yaw = 0.65f, .pitch = 0.50f,
                            .fov_deg = 45.0f, .znear = 0.1f, .zfar = 200.0f,
                            .min_distance = 6.0f, .max_distance = 80.0f};
    app.auto_sculpt  = true;
    app.use_coarse   = true;
    app.use_prepass  = true;
    app.ball_count   = 24u;
    app.budget       = 160u;
    app.relax        = 1.2f;
    app.sun_yaw      = -1.1f;
    app.sun_height   = 0.62f;
    app.brush_radius = 4.0f;
    demo_open(&app.demo, options, argc, argv);
    app_init(&app);
    while (demo_running(&app.demo)) {
        if (app.demo.ui) app_ui(&app, app.demo.ui);
        app_frame(&app);
    }
    if (app.demo.check) app_check(&app);
    return demo_close(&app.demo);
}
