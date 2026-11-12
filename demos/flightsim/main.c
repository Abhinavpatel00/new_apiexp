/* flightsim — a flight simulator: procedural terrain and sea, volumetric clouds
 * driven by a dynamic weather field, cockpit instruments, and two flight models
 * sharing one physics core.
 *
 * The frame graph, in order:
 *
 *   BakeVolumes  compute   three static noise volumes holding the cloud shape
 *   BakeWeather  compute   the 512^2 weather map, from the cell table the CPU
 *                          advects; throttled to a few hertz by fs_weather_step
 *   Sky          graphics  fullscreen, no depth: radiance, and 0 for linear
 *                          depth, which is how "nothing in front" is spelled
 *   Terrain      graphics  clipmap rings, reversed Z, no vertex buffer
 *   Water        graphics  one analytic quad at sea level
 *   Aircraft     graphics  one instanced airframe: the player and the traffic
 *   Clouds       compute   the raymarch, at a fraction of the output size
 *   Resolve      graphics  depth-aware upsample, temporal reprojection, composite
 *   Hud          graphics  alpha-blended stroke font and instruments
 *   Present      graphics  tonemap and encode
 *
 * Two frame programs, not one. The cloud history ping-pongs between two
 * accumulation targets, and frame_program_build() copies the pass table at build
 * time, so a single program cannot swap them per frame. The alternative —
 * declaring both targets writable in one pass and letting the shader choose —
 * would transition both every frame and mark one for writing that the same frame
 * then reads as history. app_update points d->program at this frame's parity.
 *
 * The clouds are why the graph is shaped this way. The march runs at a fraction
 * of the output resolution and is reprojected, so its cost is roughly
 * independent of the window size; --clouds selects the variant, and README.md
 * records what each one measured and what was rejected.
 */
#include "demo.h"
#include "flight.h"
#include "hud_font.h"
#include "world.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SHADERS "demos/flightsim/shaders/"

/* Cloud shape volumes and the weather map. The volumes are baked once at
 * startup and never written again; the map is re-baked on a throttle. */
#define WEATHER_DIM 512u
#define BASE_HI_DIM 128u
#define BASE_LO_DIM 32u
#define DETAIL_DIM  32u

#define RECORD_BYTES  80u     /* 16 floats of model matrix, then 4 of colour    */
#define RECORD_FLOATS (RECORD_BYTES / 4u)
#define MAX_RECORDS   (1u + FS_MAX_TRAFFIC)
#define MAX_TEXT      24u     /* display-list items; the HUD writes a fixed set */
#define AIRFRAME_MAX  512u    /* vertices; the builder reports how many it used */

enum { CAM_COCKPIT = 0u, CAM_CHASE = 1u, CAM_ORBIT = 2u };

/* Must match hud.slang's WARN_* bit for bit. */
enum {
    WARN_STALL = 1u << 0, WARN_PULL_UP = 1u << 1, WARN_OVERSPEED = 1u << 2,
    WARN_GROUND = 1u << 3, WARN_FUEL = 1u << 4, WARN_CRASHED = 1u << 5,
};

/* ---- push layouts, one per shader ----------------------------------------- */

typedef struct TerrainPush {
    float vp[16];
    float eye[4], sun[4], params[4];
} TerrainPush;
_Static_assert(sizeof(TerrainPush) == 112, "must match world.slang TerrainPush");

typedef struct WaterPush {
    float vp[16];
    float eye[4], sun[4], params[4];
} WaterPush;
_Static_assert(sizeof(WaterPush) == 112, "must match world.slang WaterPush");

typedef struct SkyPush {
    float eye[4], right_tan[4], up_aspect[4], forward_far[4], sun[4], params[4];
} SkyPush;
_Static_assert(sizeof(SkyPush) == 96, "must match world.slang SkyPush");

typedef struct AircraftPush {
    float    vp[16];
    float    eye[4], sun[4], params[4];
    uint32_t records, stride, vertex_base, vertex_count;
} AircraftPush;
_Static_assert(sizeof(AircraftPush) == 128, "must match world.slang AircraftPush");

typedef struct BakePush {
    uint32_t dims[4], period[4], mode[4];
    float    shape[4];
} BakePush;
_Static_assert(sizeof(BakePush) == 64, "must match bake_volume.slang BakePush");

typedef struct WeatherPush {
    uint32_t dims[4], cells[4];
    float    params[4], lattice[4];
} WeatherPush;
_Static_assert(sizeof(WeatherPush) == 64, "must match bake_weather.slang WeatherPush");

typedef struct CloudPush {
    float    eye[4], right[4], up[4], forward[4], sun[4], sun_color[4];
    float    wind[4], quality[4], shape[4], size[4], scene[4];
    uint32_t idx[4], idx2[4], idx3[4];
} CloudPush;
_Static_assert(sizeof(CloudPush) == 224, "must match clouds.slang CloudPush");

typedef struct ResolvePush {
    float    prev_vp[16];
    float    eye[4], right[4], up[4], forward[4], params[4], size[4];
    uint32_t idx[4], idx2[4];
} ResolvePush;
_Static_assert(sizeof(ResolvePush) == 192, "must match resolve.slang ResolvePush");

typedef struct HudPush {
    float    size[4], speed[4], alt[4], att[4], aero[4], engine[4], warn[4], status[4];
    uint32_t font[4], text[4];
    float    text_style[4], colour[4];
} HudPush;
_Static_assert(sizeof(HudPush) == 192, "must match hud.slang HudPush");

/* Must match hud.slang's HudTextItem: a position, an alpha, a character count,
 * and up to sixteen glyph indices one byte each, little-endian within a word. */
typedef struct HudItem {
    float    box[4];       /* x, y, alpha, character count */
    uint32_t glyphs[4];    /* characters 0..15             */
} HudItem;
_Static_assert(sizeof(HudItem) == 32, "must match hud.slang HudTextItem");

typedef struct App {
    Demo      demo;            /* first: see demo.h */
    Sim       sim;
    Weather   weather;
    GroundHeights ground;

    /* ---- options ---------------------------------------------------------- */
    uint32_t  flight_model;    /* 0 six-dof, 1 arcade                        */
    uint32_t  cloud_mode;      /* FS_CLOUDS_*                                */
    uint32_t  traffic_count;
    uint32_t  camera;
    uint32_t  grid, rings;
    float     budget, refine, cloud_scale;
    uint32_t  seed32;
    float     coverage, turbidity, time_of_day, ring_spacing, history;
    bool      show_hud;

    /* ---- engine state the dynamics core does not own ---------------------- */
    /* rpm is an instrument and fuel is a clock. Thrust comes from the throttle
     * inside flight.c, so neither model can disagree about how much power the
     * airframe has. */
    float     fuel, rpm, speedbrake;

    /* ---- resources -------------------------------------------------------- */
    Target    scene_color, scene_linear, depth, hdr;
    Target    cloud[2], cloud_dist[2];
    Target    base_hi, base_lo, detail, weather_map;
    uint32_t  scene_color_idx, scene_linear_idx;
    uint32_t  cloud_idx[2], cloud_dist_idx[2];
    uint32_t  base_hi_idx, base_lo_idx, detail_idx, weather_idx;
    BufSlice  vertices, records, cells, glyphs, segments, text_list;
    FrameProgram *programs[2];
    uint32_t  flip;            /* which cloud target this frame writes        */
    bool      bake_volumes;    /* one-shot; the record callback clears it      */
    bool      bake_weather;

    /* Host-side staging for the three buffers rewritten every frame. Kept in
     * the App rather than on the stack because the records array is 6 kB and
     * app_update runs once per frame for the life of the process. */
    float     records_host[MAX_RECORDS * RECORD_FLOATS];
    HudItem   hud_items[MAX_TEXT];
    uint32_t  hud_count;

    /* ---- camera ----------------------------------------------------------- */
    float     view[16], view_proj[16], prev_view_proj[16];
    float     eye[3], fwd[3], right[3], up[3];
    float     fov_deg, znear, zfar;
    float     orbit_yaw, orbit_pitch, orbit_dist;
    float     chase_eye[3];    /* damped: see the comment where it is written  */

    /* ---- flight controls -------------------------------------------------- */
    float     stick[3];        /* slew-limited stick position, -1 .. 1        */
    float     look_yaw, look_pitch;  /* mouse look offset, cockpit view only       */

    /* ---- pushes ----------------------------------------------------------- */
    TerrainPush  terrain_push;
    WaterPush    water_push;
    SkyPush      sky_push;
    AircraftPush aircraft_push;
    WeatherPush  weather_push;
    CloudPush    cloud_push;
    ResolvePush  resolve_push;
    HudPush      hud_push;
    BakePush     bake[3];

    uint32_t  vertex_count, record_count, cloud_w, cloud_h;
    uint32_t  warn;
    float     fps;
} App;

static const float k_player_colour[4]  = {0.86f, 0.88f, 0.92f, 0.0f};
static const float k_traffic_colour[4] = {0.80f, 0.32f, 0.22f, 0.0f};
static const char *const k_cloud_names[5] = {"OFF", "ANALYTIC", "VOLUME", "ADAPTIVE", "TEMPORAL"};

/* The cloud buffer's size, from the output size and the --scale percentage.
 * A function rather than an inline expression because app_init and app_update
 * both need it and they must agree: the buffers are allocated once at the size
 * app_init computes, and dispatched every frame at the size app_update
 * computes. */
static uint32_t fs_cloud_dim(uint32_t full, float percent)
{
    const uint32_t dim = (uint32_t)((float)full * mu_clamp(percent, 1.0f, 100.0f) / 100.0f);
    return dim > 0u ? dim : 1u;
}

/* ---- the HUD display list -------------------------------------------------- */

/* One display-list item. Every label and every annunciator comes through here,
 * which clears the two-call-site bar AGENTS.md sets for a helper, and it means
 * the character-to-glyph mapping and the byte packing exist in exactly one
 * place. Glyph 0 is blank, so a short string is already terminated. */
static void hud_text(App *a, float x, float y, float alpha, const char *fmt, ...)
{
    char     buf[20];
    va_list  ap;
    HudItem *it;
    int      n;
    uint32_t k;

    /* The list is a fixed array and the HUD writes a fixed set of labels, so
     * this is a boundary check and not a defensive one. MU_ASSERT would be the
     * usual tool but it compiles out under NDEBUG, and a bound that disappears
     * in a release build is not a bound. */
    if (a->hud_count == MAX_TEXT) return;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    n = n < 0 ? 0 : (n > 16 ? 16 : n);

    it = &a->hud_items[a->hud_count++];
    it->box[0] = x; it->box[1] = y; it->box[2] = alpha; it->box[3] = (float)n;
    for (k = 0u; k < 4u; ++k) it->glyphs[k] = 0u;
    for (k = 0u; k < (uint32_t)n; ++k)
        it->glyphs[k >> 2] |= (uint32_t)fs_hud_char_to_glyph[(unsigned char)buf[k] & 0x7F]
                              << ((k & 3u) * 8u);
}

/* ---- per-frame ------------------------------------------------------------ */

static void app_update(App *a)
{
    Demo           *d = &a->demo;
    const Aircraft *p = &a->sim.player;
    const float     aspect = (float)d->width / (float)d->height;
    const float     tan_y  = tanf(mu_radians(a->fov_deg) * 0.5f);
    float           target[3], up_hint[3], sun[3], sun_color[3], proj[16], R[16];
    uint32_t        rebakes_before = a->weather.rebakes;
    uint32_t        warn = 0u;

    /* ---- controls ---------------------------------------------------------
     * Game-style flight controls:
     *   W/S      pitch up / down   (pull back / push forward on the stick)
     *   A/D      roll left / right (bank left / right)
     *   Q/E      yaw left / right
     *   Z/C      throttle down / up
     *   RMB drag look around the cockpit
     *
     * The stick is slew-rate limited so it has weight and cannot snap from
     * full left to full right in one frame. That is what makes flight controls
     * feel physical instead of like a mouse cursor. The same slewed value
     * drives both flight models.
     */
    if (!d->headless && !d->ui_hovered) {
        const Platform     *pl = d->platform;
        const MuInputState *in = platform_state(pl);

        /* Raw target deflections from the keyboard, -1 .. 1.
         * Note the pitch axis is flipped from the raw aerospace convention so
         * W means "pull up / nose up", which is what every flight game expects. */
        float stick_tgt[3] = {0.0f, 0.0f, 0.0f};
        stick_tgt[0] += (platform_key_down(pl, MU_KEY_W) ?  1.0f : 0.0f)   /* W = pitch up   */
                    + (platform_key_down(pl, MU_KEY_S) ? -1.0f : 0.0f);   /* S = pitch down */
        stick_tgt[1] += (platform_key_down(pl, MU_KEY_D) ?  1.0f : 0.0f)   /* D = roll right */
                    + (platform_key_down(pl, MU_KEY_A) ? -1.0f : 0.0f);   /* A = roll left  */
        stick_tgt[2] += (platform_key_down(pl, MU_KEY_E) ?  1.0f : 0.0f)   /* E = yaw right  */
                    + (platform_key_down(pl, MU_KEY_Q) ? -1.0f : 0.0f);   /* Q = yaw left   */

        /* Slew-rate limit: the stick has mass. 6 rad/s is brisk but not
         * instant, so corrections take a moment and holding a turn is easy. */
        const float slew = 6.0f * d->dt;
        for (int k = 0; k < 3; ++k) {
            float step = stick_tgt[k] - a->stick[k];
            a->stick[k] += mu_clamp(step, -slew, slew);
        }

        /* Throttle: Z down, C up. A lever, not a tap — hold to move it. */
        const float thr = (platform_key_down(pl, MU_KEY_C) ? 0.5f : 0.0f)
                        - (platform_key_down(pl, MU_KEY_Z) ? 0.5f : 0.0f);
        a->sim.player.cmd_throttle = mu_clamp(a->sim.player.cmd_throttle + thr * d->dt, 0.0f, 1.0f);

        /* Mouse look: right-drag looks around the cockpit. The offset decays
         * back to centre when released so the view recentres on its own. */
        if (a->camera == CAM_COCKPIT && mu_input_mouse_down(in, MU_MOUSE_RIGHT)) {
            a->look_yaw   -= in->mouse_dx * 0.003f;
            a->look_pitch += in->mouse_dy * 0.003f;
            a->look_pitch  = mu_clamp(a->look_pitch, -1.3f, 1.3f);
        } else {
            a->look_yaw   *= 1.0f - 8.0f * d->dt;
            a->look_pitch *= 1.0f - 8.0f * d->dt;
        }

        if (platform_key_pressed(pl, MU_KEY_F)) a->flight_model ^= 1u;
        if (platform_key_pressed(pl, MU_KEY_C)) a->cloud_mode = (a->cloud_mode + 1u) % 5u;
        if (platform_key_pressed(pl, MU_KEY_V)) a->camera = (a->camera + 1u) % 3u;
        if (platform_key_pressed(pl, MU_KEY_H)) a->show_hud = !a->show_hud;
        if (platform_key_pressed(pl, MU_KEY_B)) a->speedbrake = a->speedbrake > 0.5f ? 0.0f : 1.0f;
        if (platform_key_pressed(pl, MU_KEY_R)) {
            fs_sim_reset(&a->sim, &a->sim.model, a->traffic_count, (uint64_t)a->seed32);
            a->fuel = 1.0f; a->rpm = 0.6f;
            fs_weather_reset(&a->weather, a->sim.player.pos, a->coverage, (uint64_t)a->seed32);
        }
        /* The orbit camera turns only while a button is held, so moving the
         * pointer across the window to reach the panel does not spin the world. */
        if (a->camera == CAM_ORBIT &&
            (mu_input_mouse_down(in, MU_MOUSE_LEFT) || mu_input_mouse_down(in, MU_MOUSE_RIGHT))) {
            a->orbit_yaw   -= in->mouse_dx * 0.005f;
            a->orbit_pitch += in->mouse_dy * 0.005f;
            a->orbit_pitch  = mu_clamp(a->orbit_pitch, -1.45f, 1.45f);
        }
        if (in->wheel_y != 0.0f)
            a->orbit_dist = mu_clamp(a->orbit_dist * powf(0.9f, in->wheel_y), 25.0f, 4000.0f);
    }
    a->sim.player.cmd[0] = mu_clamp(a->stick[0], -1.0f, 1.0f);
    a->sim.player.cmd[1] = mu_clamp(a->stick[1], -1.0f, 1.0f);
    a->sim.player.cmd[2] = mu_clamp(a->stick[2], -1.0f, 1.0f);
    /* Speedbrake is drag the pilot adds, and the 6-DOF model is the only one
     * that has a drag term to add it to. */
    a->sim.model.cd0 = fs_model_default.cd0 + a->speedbrake * 0.045f;
    a->sim.arcade = a->flight_model != 0u;

    /* ---- physics ---------------------------------------------------------- */
    a->rpm  += (p->throttle - a->rpm) * mu_clamp(2.6f * d->dt, 0.0f, 1.0f);
    a->fuel  = mu_clamp(a->fuel - (0.0009f + 0.0052f * p->throttle) * d->dt, 0.0f, 1.0f);

    fs_world_ground(&a->sim, &a->ground);
    fs_weather_step(&a->weather, d->dt, a->sim.wind);
    /* Sticky: a rebake request raised anywhere (the throttle here, --check, the
     * UI) has to survive until record_bake_weather has dispatched it, and this
     * line runs every frame in between. Overwriting instead of OR-ing drops a
     * request raised after the last update. */
    a->bake_weather = a->bake_weather || a->weather.rebakes != rebakes_before;
    fs_sim_step(&a->sim, d->dt, &a->ground);

    /* ---- sun --------------------------------------------------------------
     * An hour angle about the world's east-west axis, tilted so it never passes
     * exactly overhead: a sun at the zenith lights no vertical surface at all
     * and the terrain goes flat. Direction TO the sun, matching every shader. */
    {
        const float h = mu_radians((a->time_of_day - 6.0f) * 15.0f);
        sun[0] = cosf(h) * 0.35f;
        sun[1] = sinf(h);
        sun[2] = cosf(h) * 0.94f;
        mu_vec3_normalize(sun, sun);
        /* Warm toward the horizon. Colour reaches the clouds separately from
         * intensity because a low sun is dim and red at once, and folding both
         * into one number makes sunset grey. */
        const float warm = 1.0f - mu_clamp(sun[1] * 2.5f, 0.0f, 1.0f);
        sun_color[0] = 1.0f;
        sun_color[1] = 1.0f - warm * 0.42f;
        sun_color[2] = 1.0f - warm * 0.68f;
    }
    const float sun_i = mu_clamp(sun[1] * 3.2f, 0.0f, 1.0f) * 3.4f;

    /* ---- camera ----------------------------------------------------------- */
    mu_mat4_from_quat(R, p->att);
    if (a->camera == CAM_ORBIT) {
        const float cp = cosf(a->orbit_pitch), sp = sinf(a->orbit_pitch);
        a->eye[0] = p->pos[0] + sinf(a->orbit_yaw) * cp * a->orbit_dist;
        a->eye[1] = p->pos[1] + sp * a->orbit_dist;
        a->eye[2] = p->pos[2] + cosf(a->orbit_yaw) * cp * a->orbit_dist;
        memcpy(target, p->pos, sizeof target);
        up_hint[0] = 0.0f; up_hint[1] = 1.0f; up_hint[2] = 0.0f;
    } else {
        /* The cockpit sits at the pilot's eyepoint and looks where the nose
         * points, so the HUD's flight-path marker and the world agree. The
         * offset is in body axes: forward, right, down. */
        const float pilot[3] = {0.55f, 0.0f, -0.72f};
        const float back[3]  = {-26.0f, 0.0f, -7.5f};
        float       off[3], want[3];
        mu_quat_rotate_vec3(off, p->att, pilot);
        for (int k = 0; k < 3; ++k) a->eye[k] = p->pos[k] + off[k];

        /* The chase camera trails behind and above, damped toward its target
         * rather than rigidly attached: a rigid one turns a stall into the
         * horizon stopping, and a damped one turns it into the world dropping
         * away, which is the cue the player actually needs. It is integrated in
         * both views so switching to it never snaps. */
        mu_quat_rotate_vec3(want, p->att, back);
        const float lag = 1.0f - expf(-6.5f * d->dt);
        for (int k = 0; k < 3; ++k) {
            want[k] += p->pos[k];
            a->chase_eye[k] += (want[k] - a->chase_eye[k]) * lag;
        }
        if (a->camera == CAM_CHASE) {
            memcpy(a->eye, a->chase_eye, sizeof a->eye);
            for (int k = 0; k < 3; ++k) target[k] = p->pos[k] + R[k] * 60.0f;
            up_hint[0] = R[4]; up_hint[1] = R[5]; up_hint[2] = R[6];
        } else {
            /* Cockpit view with mouse look: offset the look direction so the
             * player can look around without the aircraft moving. The offset
             * decays back to centre when the mouse button is released. */
            float   ly = a->look_yaw, lp = a->look_pitch;
            float   cp = cosf(lp), sp = sinf(lp);
            float   sy = sinf(ly), cy = cosf(ly);
            float   ldir[3] = {sy * cp, sp, -cy * cp};   /* look offset in world axes */
            for (int k = 0; k < 3; ++k)
                target[k] = a->eye[k] + R[k] * 1000.0f + ldir[k] * 80.0f;
            /* The aircraft's own -Z is up. Using it instead of the world's +Y is
             * what makes the horizon roll in the cockpit view, rather than the
             * camera staying level and the world tilting around it. */
            up_hint[0] = -R[8]; up_hint[1] = -R[9]; up_hint[2] = -R[10];
        }
    }

    mu_mat4_look_at(a->view, a->eye, target, up_hint);
    mu_mat4_perspective_zo(proj, mu_radians(a->fov_deg), aspect, a->znear, a->zfar);
    /* Reversed depth, the same two-line overwrite demos/clustered applies: the
     * stock zero-to-one projection maps near to 0, but every pass here clears
     * depth to 0 and compares greater, so with the stock matrix the FARTHEST
     * surface wins and the 260 km water quad paints over the whole world.
     * Near to 1 and far to 0 also buys the precision where this demo needs it,
     * at the far plane. */
    proj[10] = a->znear / (a->zfar - a->znear);
    proj[14] = a->zfar * a->znear / (a->zfar - a->znear);
    mu_mat4_mul(proj, a->view, a->view_proj);
    /* The view matrix's columns are the camera basis: right, up, and the negated
     * forward. Reading them back out rather than recomputing them keeps the ray
     * reconstruction and the rasterization on one definition of the camera. */
    for (int k = 0; k < 3; ++k) {
        a->right[k] = a->view[k * 4u + 0u];
        a->up[k]    = a->view[k * 4u + 1u];
        a->fwd[k]   = -a->view[k * 4u + 2u];
    }

    /* ---- uploads ---------------------------------------------------------- */
    a->record_count = fs_sim_draw_records(&a->sim, a->records_host, RECORD_BYTES,
                                          k_player_colour, k_traffic_colour);
    demo_upload(d, a->records, a->records_host, (uint64_t)a->record_count * RECORD_BYTES);
    demo_upload(d, a->cells, a->weather.cells, (uint64_t)a->weather.count * sizeof(WeatherCell));

    /* ---- warnings ---------------------------------------------------------
     * Computed once, here, and shared by the annunciator text and the shader's
     * red tint, so a warning cannot appear in one and not the other. */
    if (p->stall_margin < 0.10f)                                 warn |= WARN_STALL;
    if (p->pos[1] - a->ground.player < 120.0f && p->vsi < -3.0f)  warn |= WARN_PULL_UP;
    if (p->airspeed * FS_MS_TO_KT > 210.0f)                       warn |= WARN_OVERSPEED;
    if (p->pos[1] - a->ground.player < 60.0f)                     warn |= WARN_GROUND;
    if (a->fuel < 0.08f)                                          warn |= WARN_FUEL;
    if (a->sim.crashed)                                           warn |= WARN_CRASHED;
    a->warn = warn;

    /* ---- HUD display list -------------------------------------------------
     * Annunciators are text, not a shader special case: the shader draws every
     * mark through one distance field and the only thing the bitmask does there
     * is turn the whole display red. A blink is an alpha the CPU has already
     * computed, so the shader needs no clock for it. */
    a->hud_count = 0u;
    if (a->show_hud) {
        const float blink = fmodf(d->time * 2.0f, 1.0f) < 0.6f ? 1.0f : 0.15f;
        const float ar    = aspect;
        if (warn & WARN_STALL)     hud_text(a, -0.26f, 0.42f, blink, "STALL");
        if (warn & WARN_PULL_UP)   hud_text(a, -0.30f, 0.32f, blink, "PULL UP");
        if (warn & WARN_OVERSPEED) hud_text(a, -0.36f, 0.22f, blink, "OVERSPEED");
        if (warn & WARN_GROUND)    hud_text(a, -0.30f, 0.12f, blink, "TERRAIN");
        if (warn & WARN_FUEL)      hud_text(a, -0.24f, 0.02f, blink, "FUEL");
        if (warn & WARN_CRASHED)   hud_text(a, -0.34f, -0.08f, 1.0f, "AIRCRAFT LOST");

        /* The status line, along the bottom: what the player can change at
         * runtime, and what it is currently costing. */
        hud_text(a, -ar + 0.04f, -0.94f, 0.75f, "%s", a->sim.arcade ? "ARCADE" : "6DOF");
        hud_text(a, -ar + 0.04f, -0.86f, 0.75f, "CLD %s", k_cloud_names[a->cloud_mode]);
        hud_text(a, -ar + 0.04f, -0.78f, 0.75f, "%3.0F FPS", (double)a->fps);
        hud_text(a, -ar + 0.04f, -0.70f, 0.75f, "ALT %5.0F", (double)(p->altitude * FS_M_TO_FT));
        hud_text(a, -ar + 0.04f, -0.62f, 0.75f, "AGL %5.0F",
                 (double)((p->altitude - a->ground.player) * FS_M_TO_FT));
        hud_text(a, -ar + 0.04f, -0.54f, 0.75f, "GS %3.0F KT", (double)(p->groundspeed * FS_MS_TO_KT));
        hud_text(a, -ar + 0.04f, -0.46f, 0.75f, "MACH %.2F", (double)p->mach);
        hud_text(a, -ar + 0.04f, -0.38f, 0.75f, "AOA %4.1F", (double)mu_degrees(p->alpha));
        hud_text(a, -ar + 0.04f, -0.30f, 0.75f, "G %4.1F", (double)p->load_factor);
        hud_text(a, -ar + 0.04f, -0.22f, 0.75f, "THR %3.0F", (double)(p->throttle * 100.0f));
        hud_text(a, -ar + 0.04f, -0.14f, 0.75f, "FUEL %3.0F", (double)(a->fuel * 100.0f));
        hud_text(a, -ar + 0.04f, -0.06f, 0.75f, "TRF %2.0F", (double)a->sim.traffic_count);
        hud_text(a, ar - 0.62f, -0.94f, 0.55f, "WASD FLY  QE YAW  ZC THR  RMB LOOK");
    }

    demo_upload(d, a->text_list, a->hud_items, (uint64_t)a->hud_count * sizeof(HudItem));

    /* ---- pushes ----------------------------------------------------------- */
    memcpy(a->terrain_push.vp, a->view_proj, 64u);
    memcpy(a->water_push.vp, a->view_proj, 64u);
    memcpy(a->aircraft_push.vp, a->view_proj, 64u);

    /* The finest ring is only useful near the ground; from cruise altitude its
     * grid covers a handful of pixels. Quantising the base spacing to an octave
     * of the height above ground hands those vertices to a ring that can use
     * them, and quantising rather than scaling means the grid moves with the
     * ring progression instead of continuously, so it does not swim. */
    const float agl     = fmaxf(a->eye[1] - a->ground.player, 0.0f);
    const float octave  = floorf(log2f(mu_clamp(agl / 900.0f, 1.0f, 64.0f)));
    const float spacing = a->ring_spacing * exp2f(octave);

    memcpy(a->terrain_push.eye, a->eye, 12u); a->terrain_push.eye[3] = d->time;
    memcpy(a->terrain_push.sun, sun, 12u);    a->terrain_push.sun[3] = sun_i;
    a->terrain_push.params[0] = (float)a->grid;
    a->terrain_push.params[1] = (float)a->rings;
    a->terrain_push.params[2] = a->turbidity;
    a->terrain_push.params[3] = spacing;

    memcpy(a->water_push.eye, a->eye, 12u); a->water_push.eye[3] = d->time;
    memcpy(a->water_push.sun, sun, 12u);    a->water_push.sun[3] = sun_i;
    a->water_push.params[0] = 260000.0f;   /* FS_FAR: the sea must reach the horizon */
    a->water_push.params[1] = FS_SEA_LEVEL;
    a->water_push.params[2] = a->turbidity;
    a->water_push.params[3] = 0.55f + 0.45f * a->coverage;   /* chop follows the weather */

    memcpy(a->sky_push.eye, a->eye, 12u);
    memcpy(a->sky_push.right_tan, a->right, 12u);   a->sky_push.right_tan[3] = tan_y;
    memcpy(a->sky_push.up_aspect, a->up, 12u);      a->sky_push.up_aspect[3] = aspect;
    memcpy(a->sky_push.forward_far, a->fwd, 12u);   a->sky_push.forward_far[3] = a->zfar;
    memcpy(a->sky_push.sun, sun, 12u);              a->sky_push.sun[3] = sun_i;
    a->sky_push.params[0] = a->turbidity;
    a->sky_push.params[1] = d->time;

    memcpy(a->aircraft_push.eye, a->eye, 12u); a->aircraft_push.eye[3] = d->time;
    memcpy(a->aircraft_push.sun, sun, 12u);    a->aircraft_push.sun[3] = sun_i;
    a->aircraft_push.params[0]    = a->turbidity;
    a->aircraft_push.records      = (uint32_t)a->records.offset;
    a->aircraft_push.stride       = RECORD_BYTES;
    a->aircraft_push.vertex_base  = (uint32_t)a->vertices.offset;
    a->aircraft_push.vertex_count = a->vertex_count;

    a->weather_push.dims[0]    = WEATHER_DIM;
    a->weather_push.dims[1]    = WEATHER_DIM;
    a->weather_push.dims[2]    = a->weather_idx;
    a->weather_push.dims[3]    = a->weather.count;
    a->weather_push.cells[0]   = (uint32_t)a->cells.offset;
    a->weather_push.cells[1]   = (uint32_t)sizeof(WeatherCell);
    a->weather_push.params[0]  = a->weather.coverage;
    a->weather_push.params[1]  = a->weather.type_bias;
    a->weather_push.params[2]  = a->weather.precip;
    a->weather_push.params[3]  = a->weather.twist;
    a->weather_push.lattice[0] = 64.0f;
    a->weather_push.lattice[1] = 64.0f;
    a->weather_push.lattice[2] = 4.0f;
    a->weather_push.lattice[3] = 0.5f;

    a->cloud_w = fs_cloud_dim(d->width, a->cloud_scale);
    a->cloud_h = fs_cloud_dim(d->height, a->cloud_scale);
    memcpy(a->cloud_push.eye, a->eye, 12u);         a->cloud_push.eye[3] = tan_y;
    memcpy(a->cloud_push.right, a->right, 12u);     a->cloud_push.right[3] = aspect;
    memcpy(a->cloud_push.up, a->up, 12u);
    memcpy(a->cloud_push.forward, a->fwd, 12u);
    memcpy(a->cloud_push.sun, sun, 12u);            a->cloud_push.sun[3] = sun_i;
    memcpy(a->cloud_push.sun_color, sun_color, 12u); a->cloud_push.sun_color[3] = 1.0f;
    a->cloud_push.wind[0]    = a->weather.wind_u;
    a->cloud_push.wind[1]    = a->weather.wind_v;
    a->cloud_push.wind[2]    = d->time;
    a->cloud_push.wind[3]    = 1.0f;          /* light cone spread             */
    a->cloud_push.quality[0] = (float)a->cloud_mode;
    a->cloud_push.quality[1] = a->budget;
    a->cloud_push.quality[2] = a->refine;
    a->cloud_push.quality[3] = 0.5f;          /* jitter, in texels             */
    a->cloud_push.shape[0]   = 0.0f;          /* coverage bias                 */
    a->cloud_push.shape[1]   = 0.055f;        /* extinction per unit density   */
    a->cloud_push.shape[2]   = 0.55f;         /* detail amount                 */
    a->cloud_push.shape[3]   = 0.012f;        /* transmittance cut-off         */
    a->cloud_push.size[0]  = (float)a->cloud_w;  a->cloud_push.size[1] = (float)a->cloud_h;
    a->cloud_push.size[2]  = 1.0f / (float)a->cloud_w; a->cloud_push.size[3] = 1.0f / (float)a->cloud_h;
    a->cloud_push.scene[0] = (float)d->width;  a->cloud_push.scene[1] = (float)d->height;
    a->cloud_push.scene[2] = 1.0f / (float)d->width; a->cloud_push.scene[3] = 1.0f / (float)d->height;
    a->cloud_push.idx[0]   = a->weather_idx;
    a->cloud_push.idx[1]   = a->base_hi_idx;
    a->cloud_push.idx[2]   = a->base_lo_idx;
    a->cloud_push.idx[3]   = a->detail_idx;
    a->cloud_push.idx2[0]  = d->linear_clamp_index;    /* the volume sampler: the
                                                         * march wraps u and w itself
                                                         * with frac(), because a
                                                         * repeating sampler on a 3D
                                                         * image in a compute stage
                                                         * reads zero on llvmpipe */    /* the volume sampler: the
                                                         * horizontal coordinates wrap
                                                         * outside [0,1], so a clamp
                                                         * sampler would freeze the sky
                                                         * to the volume's edge texels
                                                         * beyond one tile */
    a->cloud_push.idx2[1]  = d->nearest_clamp_index;   /* the 2D linear one     */
    a->cloud_push.idx2[2]  = a->scene_linear_idx;
    a->cloud_push.idx2[3]  = a->cloud_idx[a->flip];
    a->cloud_push.idx3[0]  = a->cloud_dist_idx[a->flip];

    memcpy(a->resolve_push.prev_vp, a->prev_view_proj, 64u);
    memcpy(a->resolve_push.eye, a->eye, 12u);
    memcpy(a->resolve_push.right, a->right, 12u); a->resolve_push.right[3] = tan_y;
    memcpy(a->resolve_push.up, a->up, 12u);       a->resolve_push.up[3] = aspect;
    memcpy(a->resolve_push.forward, a->fwd, 12u);
    a->resolve_push.params[0] = a->history;
    a->resolve_push.params[1] = (float)a->cloud_mode;
    a->resolve_push.params[2] = d->time;
    a->resolve_push.size[0] = (float)d->width;   a->resolve_push.size[1] = (float)d->height;
    a->resolve_push.size[2] = (float)a->cloud_w; a->resolve_push.size[3] = (float)a->cloud_h;
    a->resolve_push.idx[0]  = a->cloud_idx[a->flip];
    a->resolve_push.idx[1]  = a->cloud_dist_idx[a->flip];
    a->resolve_push.idx[2]  = a->cloud_idx[a->flip ^ 1u];
    a->resolve_push.idx[3]  = a->cloud_dist_idx[a->flip ^ 1u];
    a->resolve_push.idx2[0] = a->scene_color_idx;
    a->resolve_push.idx2[1] = a->scene_linear_idx;
    a->resolve_push.idx2[2] = d->nearest_clamp_index;
    a->resolve_push.idx2[3] = d->linear_clamp_index;

    a->hud_push.size[0]   = (float)d->width;  a->hud_push.size[1] = (float)d->height;
    a->hud_push.size[2]   = 1.0f / (float)d->width; a->hud_push.size[3] = 1.0f / (float)d->height;
    a->hud_push.speed[0]  = p->airspeed * FS_MS_TO_KT;
    /* True airspeed is what the wing is actually doing; indicated is what the
     * pitot reads. The ratio is the square root of the density ratio, which is
     * the incompressible form and the one an airspeed indicator implements. */
    a->hud_push.speed[1]  = p->airspeed * FS_MS_TO_KT * sqrtf(FS_RHO_0 / fmaxf(p->rho, 0.01f));
    a->hud_push.speed[2]  = p->mach;
    a->hud_push.speed[3]  = p->groundspeed * FS_MS_TO_KT;
    a->hud_push.alt[0]    = p->altitude * FS_M_TO_FT;
    a->hud_push.alt[1]    = (p->altitude - a->ground.player) * FS_M_TO_FT;
    a->hud_push.alt[2]    = p->vsi * FS_M_TO_FT * 60.0f;      /* ft per minute   */
    a->hud_push.alt[3]    = 0.0f;
    a->hud_push.att[0]    = mu_degrees(p->pitch);
    a->hud_push.att[1]    = mu_degrees(p->roll);
    a->hud_push.att[2]    = mu_degrees(p->heading);
    a->hud_push.att[3]    = p->load_factor;
    a->hud_push.aero[0]   = mu_degrees(p->alpha);
    a->hud_push.aero[1]   = mu_degrees(p->beta);
    a->hud_push.aero[2]   = p->stall_margin;
    a->hud_push.aero[3]   = a->speedbrake;
    a->hud_push.engine[0] = p->throttle;
    a->hud_push.engine[1] = a->rpm;
    a->hud_push.engine[2] = a->fuel;
    a->hud_push.engine[3] = a->fuel > 0.02f ? 1.0f : 0.0f;
    a->hud_push.warn[0]   = (float)warn;
    a->hud_push.warn[1]   = d->time;
    a->hud_push.warn[2]   = 2.0f;                            /* blink rate, Hz */
    a->hud_push.status[0] = (float)a->flight_model;
    a->hud_push.status[1] = (float)a->cloud_mode;
    a->hud_push.status[2] = a->fps;
    a->hud_push.status[3] = 0.0f;
    a->hud_push.font[0]   = (uint32_t)a->glyphs.offset;
    a->hud_push.font[1]   = (uint32_t)a->segments.offset;
    a->hud_push.font[2]   = 4u;                              /* bytes per segment */
    a->hud_push.text[0]   = (uint32_t)a->text_list.offset;
    a->hud_push.text[1]   = a->hud_count;
    a->hud_push.text[2]   = (uint32_t)sizeof(HudItem);
    a->hud_push.text_style[0] = 0.042f;                      /* cap height      */
    a->hud_push.text_style[1] = 1.35f;                       /* advance, in widths */
    a->hud_push.colour[0] = 0.30f;
    a->hud_push.colour[1] = 0.95f;
    a->hud_push.colour[2] = 0.45f;
    a->hud_push.colour[3] = 0.95f;

    /* ---- frame bookkeeping ------------------------------------------------ */
    a->fps = d->dt > 0.0f ? 1.0f / d->dt : 0.0f;
    memcpy(a->prev_view_proj, a->view_proj, 64u);
    d->program = a->programs[a->flip];
    a->flip ^= 1u;
    d->present.tonemap = DEMO_TONEMAP_ACES_FITTED;
}

/* ---- app_frame -------------------------------------------------------------- */

/* One frame: app_update already did the controls, physics, uploads and pushes
 * and set d->program to this frame's parity. Record every pass in order. */
static void app_frame(App *a)
{
    Demo  *d = &a->demo;
    PassCtx *pass;

    app_update(a);

    if (!demo_frame_begin(d)) return;

    /* BakeVolumes — once at startup, never again. Always begin the pass. */
    pass = demo_pass(d, "BakeVolumes");
    if (a->bake_volumes) {
        for (uint32_t k = 0u; k < 3u; ++k) {
            const uint32_t dim = a->bake[k].dims[0];
            pass_push(pass, &a->bake[k], sizeof(BakePush));
            pass_dispatch(pass, (dim + 3u) / 4u, (dim + 3u) / 4u, (dim + 3u) / 4u);
        }
        a->bake_volumes = false;
    }

    /* BakeWeather — on request, one dispatch. Always begin the pass. */
    pass = demo_pass(d, "BakeWeather");
    if (a->bake_weather) {
        pass_push(pass, &a->weather_push, sizeof(WeatherPush));
        pass_dispatch(pass, WEATHER_DIM / 8u, WEATHER_DIM / 8u, 1u);
        a->bake_weather = false;
    }

    /* Sky — fullscreen, no depth. */
    pass = demo_pass(d, "Sky");
    pass_push(pass, &a->sky_push, sizeof(SkyPush));
    pass_draw_fullscreen_triangle(pass, d->width, d->height);

    /* Terrain — clipmap rings, reversed Z, clears depth and the distance target. */
    pass = demo_pass(d, "Terrain");
    pass_push(pass, &a->terrain_push, sizeof(TerrainPush));
    pass_draw(pass, a->grid * a->grid * 6u, a->rings, 0u, 0u);

    /* Water — one analytic quad at sea level. */
    pass = demo_pass(d, "Water");
    pass_push(pass, &a->water_push, sizeof(WaterPush));
    pass_draw(pass, 6u, 1u, 0u, 0u);

    /* Aircraft — instanced from the record table. */
    pass = demo_pass(d, "Aircraft");
    pass_push(pass, &a->aircraft_push, sizeof(AircraftPush));
    pass_draw(pass, a->vertex_count, a->record_count, 0u, 0u);

    /* Clouds — raymarch at a fraction of the output size. */
    pass = demo_pass(d, "Clouds");
    pass_push(pass, &a->cloud_push, sizeof(CloudPush));
    pass_dispatch(pass, (a->cloud_w + 7u) / 8u, (a->cloud_h + 7u) / 8u, 1u);

    /* Resolve — tone-map, composite clouds, reproject history. */
    pass = demo_pass(d, "Resolve");
    pass_push(pass, &a->resolve_push, sizeof(ResolvePush));
    pass_draw_fullscreen_triangle(pass, d->width, d->height);

    /* Hud — alpha-blended symbology. */
    pass = demo_pass(d, "Hud");
    if (a->show_hud) {
        pass_push(pass, &a->hud_push, sizeof(HudPush));
        pass_draw_fullscreen_triangle(pass, d->width, d->height);
    }

    demo_present(d);
    demo_frame_end(d);
}

/* ---- setup ---------------------------------------------------------------- */

static void app_init(App *a)
{
    Demo     *d = &a->demo;
    Gpu      *gpu = d->gpu;
    static float airframe[AIRFRAME_MAX * 8];
    const uint32_t cw = fs_cloud_dim(d->width, a->cloud_scale);
    const uint32_t ch = fs_cloud_dim(d->height, a->cloud_scale);

    a->vertex_count = fs_airframe_build(airframe, AIRFRAME_MAX);
    a->vertices   = demo_alloc(d, airframe, (uint64_t)a->vertex_count * 32u);
    a->records    = demo_alloc(d, NULL, (uint64_t)MAX_RECORDS * RECORD_BYTES);
    a->cells      = demo_alloc(d, NULL, (uint64_t)FS_MAX_CELLS * sizeof(WeatherCell));
    a->glyphs     = demo_alloc(d, fs_hud_glyphs, (uint64_t)FS_HUD_GLYPHS * sizeof(FsGlyph));
    a->segments   = demo_alloc(d, fs_hud_segments, (uint64_t)FS_HUD_SEGMENTS * 4u);
    a->text_list  = demo_alloc(d, NULL, (uint64_t)MAX_TEXT * sizeof(HudItem));

    /* The scene is rendered in linear radiance with the view distance beside it,
     * because the cloud march has to stop at the terrain and the resolve pass
     * has to know which half-resolution texel is nearest to it. Both are written
     * by the same four draws, so neither costs an extra pass. */
    a->scene_color  = demo_target(d, "fs_scene", FMT_RGBA16F, d->width, d->height, false, &a->scene_color_idx);
    a->scene_linear = demo_target(d, "fs_linear", FMT_R32F, d->width, d->height, false, &a->scene_linear_idx);
    a->depth        = demo_target(d, "fs_depth", FMT_D32F, d->width, d->height, false, NULL);
    a->hdr          = demo_target(d, "fs_hdr", FMT_RGBA16F, d->width, d->height, false, NULL);

    /* The cloud history ping-pongs, so there are two of each. */
    /* TargetDesc stores the name pointer rather than copying it, so these have
     * to be static: a formatted stack buffer here would still be pointed at
     * after app_init returned. */
    static const char *const k_accum_names[2] = {"fs_cloud_0", "fs_cloud_1"};
    static const char *const k_dist_names[2]  = {"fs_cloud_dist_0", "fs_cloud_dist_1"};
    for (uint32_t k = 0u; k < 2u; ++k) {
        a->cloud[k]      = demo_target(d, k_accum_names[k], FMT_RGBA16F, cw, ch, true, &a->cloud_idx[k]);
        a->cloud_dist[k] = demo_target(d, k_dist_names[k], FMT_RGBA16F, cw, ch, true, &a->cloud_dist_idx[k]);
    }

    /* The shape volumes are baked once. base_lo is a coarse second volume rather
     * than a mip of base_hi: this API binds one subresource per storage
     * descriptor and exposes no blit, so a volume cannot get a mip chain, and
     * the light cone wants a low-frequency field it can six-tap cheaply. */
    a->base_hi    = demo_volume(d, "fs_base_hi", FMT_RGBA16F, BASE_HI_DIM, BASE_HI_DIM, BASE_HI_DIM, &a->base_hi_idx);
    a->base_lo    = demo_volume(d, "fs_base_lo", FMT_RGBA16F, BASE_LO_DIM, BASE_LO_DIM, BASE_LO_DIM, &a->base_lo_idx);
    a->detail     = demo_volume(d, "fs_detail", FMT_RGBA16F, DETAIL_DIM, DETAIL_DIM, DETAIL_DIM, &a->detail_idx);
    a->weather_map = demo_target(d, "fs_weather", FMT_RGBA8_UNORM, WEATHER_DIM, WEATHER_DIM, true, &a->weather_idx);

    /* {dim, lattice period, octaves, mode, warp, contrast, bias, 2nd-channel ratio} */
    {
        /* {dim, lattice period, octaves, mode, warp, contrast, bias, 2nd-channel ratio}
         * Contrast and bias are 1 and 0: the volume stores the RAW shape, the
         * same 0..1 quantity the analytic mode hands to the coverage gate.
         * Shaping the field here as well would apply the gate twice, and every
         * mode above 1 would march through a sky whose density sits below the
         * threshold while mode 1 drew clouds from the same weather. */
        const struct { uint32_t dim, idx, per, mode; float contrast, bias, ratio; } k_bake[3] = {
            /* Periods in lattice cells across the volume, and four octaves
             * double them: 16 over 128 texels leaves eight texels per cell and
             * puts the last octave at one texel per cell. The first attempt
             * used 64, which is two texels per cell — above Nyquist — and the
             * bake came out as high-frequency noise whose trilinear mean has
             * none of the variance the march needs to make a cloud. */
            {BASE_HI_DIM, a->base_hi_idx,  16u, 0u, 1.00f, 0.00f, 1.90f},
            {BASE_LO_DIM, a->base_lo_idx,   4u, 0u, 1.00f, 0.00f, 1.90f},
            {DETAIL_DIM,  a->detail_idx,    8u, 1u, 1.00f, 0.00f, 1.90f},
        };
        for (uint32_t k = 0u; k < 3u; ++k) {
            a->bake[k].dims[0] = k_bake[k].dim;  a->bake[k].dims[1] = k_bake[k].dim;
            a->bake[k].dims[2] = k_bake[k].dim;  a->bake[k].dims[3] = k_bake[k].idx;
            a->bake[k].period[0] = k_bake[k].per; a->bake[k].period[1] = k_bake[k].per;
            a->bake[k].period[2] = k_bake[k].per; a->bake[k].period[3] = 4u;
            a->bake[k].mode[0] = k_bake[k].mode;  a->bake[k].mode[1] = 0u;
            a->bake[k].shape[0] = k_bake[k].contrast;
            a->bake[k].shape[1] = k_bake[k].bias;
            a->bake[k].shape[2] = k_bake[k].ratio;
        }
    }
    a->bake_volumes = true;
    a->bake_weather = true;

    /* ---- the sim ---------------------------------------------------------- */
    fs_sim_reset(&a->sim, &fs_model_default, a->traffic_count, (uint64_t)a->seed32);
    fs_weather_reset(&a->weather, a->sim.player.pos, a->coverage, (uint64_t)a->seed32);
    a->weather.coverage = a->coverage;
    a->fuel = 1.0f; a->rpm = 0.6f;
    a->fov_deg = 62.0f; a->znear = 0.35f; a->zfar = 260000.0f;   /* FS_FAR */
    a->orbit_yaw = 0.6f; a->orbit_pitch = 0.22f; a->orbit_dist = 120.0f;
    memcpy(a->chase_eye, a->sim.player.pos, sizeof a->chase_eye);
    a->chase_eye[0] -= 26.0f; a->chase_eye[1] += 7.5f;
    memcpy(a->prev_view_proj, a->view_proj, 64u);

    /* ---- pipelines -------------------------------------------------------- */
    const ShaderId terrain_vs  = demo_shader(d, SHADERS "terrain.vert.spv", STAGE_VERTEX);
    const ShaderId terrain_fs  = demo_shader(d, SHADERS "terrain.frag.spv", STAGE_FRAGMENT);
    const ShaderId water_vs    = demo_shader(d, SHADERS "water.vert.spv", STAGE_VERTEX);
    const ShaderId water_fs    = demo_shader(d, SHADERS "water.frag.spv", STAGE_FRAGMENT);
    const ShaderId sky_fs      = demo_shader(d, SHADERS "sky.frag.spv", STAGE_FRAGMENT);
    const ShaderId aircraft_vs = demo_shader(d, SHADERS "aircraft.vert.spv", STAGE_VERTEX);
    const ShaderId aircraft_fs = demo_shader(d, SHADERS "aircraft.frag.spv", STAGE_FRAGMENT);
    const ShaderId bake_cs     = demo_shader(d, SHADERS "bake_volume.comp.spv", STAGE_COMPUTE);
    const ShaderId weather_cs  = demo_shader(d, SHADERS "bake_weather.comp.spv", STAGE_COMPUTE);
    const ShaderId clouds_cs   = demo_shader(d, SHADERS "clouds.comp.spv", STAGE_COMPUTE);
    const ShaderId resolve_fs  = demo_shader(d, SHADERS "resolve.frag.spv", STAGE_FRAGMENT);
    const ShaderId hud_fs      = demo_shader(d, SHADERS "hud.frag.spv", STAGE_FRAGMENT);

    /* All four world draws write the same two attachments, so they share a
     * pipeline shape and differ only in their stages. */
    const PipeKey world_key = {.vs = terrain_vs, .fs = terrain_fs,
                               .color = {FMT_RGBA16F, FMT_R32F}, .color_count = 2u,
                               .depth = FMT_D32F, .blend_fallback = BLEND_OPAQUE};
    const Pipe terrain_pipe  = pipe_intern(gpu, &world_key);
    const Pipe water_pipe    = pipe_intern(gpu, &(PipeKey){.vs = water_vs, .fs = water_fs,
                                  .color = {FMT_RGBA16F, FMT_R32F}, .color_count = 2u,
                                  .depth = FMT_D32F, .blend_fallback = BLEND_OPAQUE});
    const Pipe aircraft_pipe = pipe_intern(gpu, &(PipeKey){.vs = aircraft_vs, .fs = aircraft_fs,
                                  .color = {FMT_RGBA16F, FMT_R32F}, .color_count = 2u,
                                  .depth = FMT_D32F, .blend_fallback = BLEND_OPAQUE});
    /* The sky has no depth attachment: it is the background, and giving it one
     * would write the far plane over the whole screen for nothing. */
    const Pipe sky_pipe = pipe_intern_graphics(gpu, d->fullscreen_vs, sky_fs,
                                               FMT_RGBA16F, FMT_NONE, BLEND_OPAQUE);
    const Pipe bake_pipe    = pipe_intern_compute(gpu, bake_cs, "main");
    const Pipe weather_pipe = pipe_intern_compute(gpu, weather_cs, "main");
    const Pipe clouds_pipe  = pipe_intern_compute(gpu, clouds_cs, "main");
    const Pipe resolve_pipe = pipe_intern_graphics(gpu, d->fullscreen_vs, resolve_fs,
                                                   FMT_RGBA16F, FMT_NONE, BLEND_OPAQUE);
    const Pipe hud_pipe     = pipe_intern_graphics(gpu, d->fullscreen_vs, hud_fs,
                                                   FMT_RGBA16F, FMT_NONE, BLEND_ALPHA);

    /* stages 0 derives the visibility from the pass kind, which is the narrow
     * mask in every one of these cases and one less thing to get wrong. */
    const BufUse cells_read = {.slice = a->cells, .access = ACC_READ};
    const BufUse hud_reads[3] = {{.slice = a->glyphs, .access = ACC_READ},
                                 {.slice = a->segments, .access = ACC_READ},
                                 {.slice = a->text_list, .access = ACC_READ}};
    const BufUse aircraft_reads[2] = {{.slice = a->vertices, .access = ACC_READ},
                                      {.slice = a->records, .access = ACC_READ}};
    const Target bake_targets[3] = {a->base_hi, a->base_lo, a->detail};

    /* Two programs: the cloud history ping-pongs and the pass table is copied at
     * build time, so each parity needs its own. Everything else is identical. */
    for (uint32_t parity = 0u; parity < 2u; ++parity) {
        const Target accum_now  = a->cloud[parity],      accum_prev = a->cloud[parity ^ 1u];
        const Target dist_now   = a->cloud_dist[parity], dist_prev  = a->cloud_dist[parity ^ 1u];
        const Target cloud_writes[2]  = {accum_now, dist_now};
        /* The march reads the weather map, the three shape volumes and the
         * scene's linear depth (to stop at the terrain), plus last frame's
         * accumulation for the temporal modes. */
        const Target cloud_reads[6]   = {a->weather_map, a->base_hi, a->base_lo, a->detail,
                                         a->scene_linear, accum_prev};
        const Target resolve_reads[6] = {accum_now, dist_now, accum_prev, dist_prev,
                                         a->scene_color, a->scene_linear};
        /* Reversed depth throughout: the projection maps near to 1 and far to 0,
         * which is what keeps precision at the far plane, where a flight
         * simulator spends most of its frames. Terrain clears to 0 and the rest
         * compare greater. */
        AttachDesc depth_clear = attach_depth_clear(a->depth, 0.0f);
        AttachDesc depth_write = attach_color(a->depth, ATT_DEPTH_WRITE);
        /* One AttachDesc array per pass, not one shared array written three
         * times. frame_program_build copies the colours when it builds, which is
         * after this whole loop body has run, so passes sharing an array would
         * all see its last contents: Terrain would load the linear-depth target
         * instead of clearing it, and every pixel the terrain does not cover
         * would keep last frame's distance. */
        AttachDesc scene_first[2], scene_rest[2], scene_color_only, hdr_write, hdr_blend;
        PassDesc   passes[11];
        uint32_t   count = 0u;

        passes[count++] = (PassDesc){
            .name = "BakeVolumes", .kind = PASS_COMPUTE, .pipe = bake_pipe,
            .state = draw_state_opaque(SC_1, CULL_NONE),
            .tex_writes = bake_targets, .tex_write_count = 3u};
        passes[count++] = (PassDesc){
            .name = "BakeWeather", .kind = PASS_COMPUTE, .pipe = weather_pipe,
            .state = draw_state_opaque(SC_1, CULL_NONE),
            .buf_reads = &cells_read, .buf_read_count = 1u,
            .tex_writes = &a->weather_map, .tex_write_count = 1u};

        /* The sky pass has ONE colour attachment because its pipeline declares
         * one format, and a pipeline's blend attachment count has to match the
         * subpass. That leaves scene_linear to be cleared by the next pass,
         * which is where it belongs anyway: Terrain is the first draw that
         * writes a distance, and clearing here means every pixel the terrain
         * does not cover keeps the 0 that says "nothing in front of the
         * clouds". The sky does not need to write a distance at all. */
        scene_color_only = attach_clear(a->scene_color, 0.0f, 0.0f, 0.0f, 0.0f);
        hdr_write = attach_color(a->hdr, ATT_WRITE);
        hdr_blend = attach_color(a->hdr, ATT_WRITE_KEEP);
        passes[count++] = (PassDesc){
            .name = "Sky", .kind = PASS_GRAPHICS, .pipe = sky_pipe,
            .state = draw_state_opaque(SC_1, CULL_NONE),
            .colors = &scene_color_only, .color_count = 1u};

        scene_first[0] = attach_color(a->scene_color, ATT_WRITE_KEEP);
        scene_first[1] = attach_clear(a->scene_linear, 0.0f, 0.0f, 0.0f, 0.0f);
        passes[count++] = (PassDesc){
            .name = "Terrain", .kind = PASS_GRAPHICS, .pipe = terrain_pipe,
            .state = (DrawState){.samples = SC_1, .blend = BLEND_OPAQUE, .depth_cmp = CMP_GREATER,
                                 .depth_write = 1u, .cull = CULL_BACK, .topo = TOPO_TRI,
                                 .write_mask = 0xFFFFu},
            .colors = scene_first, .color_count = 2u, .depth = &depth_clear};

        scene_rest[0] = attach_color(a->scene_color, ATT_WRITE_KEEP);
        scene_rest[1] = attach_color(a->scene_linear, ATT_WRITE_KEEP);
        passes[count++] = (PassDesc){
            .name = "Water", .kind = PASS_GRAPHICS, .pipe = water_pipe,
            .state = (DrawState){.samples = SC_1, .blend = BLEND_OPAQUE, .depth_cmp = CMP_GREATER,
                                 .depth_write = 1u, .cull = CULL_BACK, .topo = TOPO_TRI,
                                 .write_mask = 0xFFFFu},
            .colors = scene_rest, .color_count = 2u, .depth = &depth_write};
        passes[count++] = (PassDesc){
            .name = "Aircraft", .kind = PASS_GRAPHICS, .pipe = aircraft_pipe,
            .state = (DrawState){.samples = SC_1, .blend = BLEND_OPAQUE, .depth_cmp = CMP_GREATER,
                                 .depth_write = 1u, .cull = CULL_BACK, .topo = TOPO_TRI,
                                 .write_mask = 0xFFFFu},
            .colors = scene_rest, .color_count = 2u, .depth = &depth_write,
            .buf_reads = aircraft_reads, .buf_read_count = 2u};

        passes[count++] = (PassDesc){
            .name = "Clouds", .kind = PASS_COMPUTE, .pipe = clouds_pipe,
            .state = draw_state_opaque(SC_1, CULL_NONE),
            .tex_reads = cloud_reads, .tex_read_count = 6u,
            .tex_writes = cloud_writes, .tex_write_count = 2u};
        passes[count++] = (PassDesc){
            .name = "Resolve", .kind = PASS_GRAPHICS, .pipe = resolve_pipe,
            .state = draw_state_opaque(SC_1, CULL_NONE),
            .colors = &hdr_write, .color_count = 1u,
            .tex_reads = resolve_reads, .tex_read_count = 6u};
        passes[count++] = (PassDesc){
            .name = "Hud", .kind = PASS_GRAPHICS, .pipe = hud_pipe,
            .state = draw_state_alpha(SC_1, CULL_NONE),
            .colors = &hdr_blend, .color_count = 1u,
            .buf_reads = hud_reads, .buf_read_count = 3u};

        passes[count++] = demo_present_pass(d, a->hdr);
        a->programs[parity] = demo_program(d, passes, count);
    }
}

static void app_ui(App *a, struct nk_context *ctx)
{
    static const char *const k_models[2] = {"6-DOF aerodynamic", "arcade rate"};
    Demo *d = &a->demo;
    if (nk_begin(ctx, "Flight", nk_rect(10, 10, 300, 420),
                 NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_TITLE)) {
        demo_ui_text(ctx, "%.1f ms   %.0f ft AGL   %.0f kt", (double)d->dt * 1000.0,
                     (double)((a->sim.player.altitude - a->ground.player) * FS_M_TO_FT),
                     (double)(a->sim.player.airspeed * FS_MS_TO_KT));
        demo_ui_choice(ctx, "Flight model", k_models, 2u, &a->flight_model);
        demo_ui_choice(ctx, "Clouds", k_cloud_names, 5u, &a->cloud_mode);
        demo_ui_float(ctx, "Coverage", &a->weather.coverage, 0.0f, 1.0f);
        demo_ui_float(ctx, "Cloud type (-1 stratus, +1 towering)", &a->weather.type_bias, -1.0f, 1.0f);
        demo_ui_float(ctx, "Precipitation", &a->weather.precip, 0.0f, 1.0f);
        demo_ui_float(ctx, "Turbulence", &a->sim.turbulence, 0.0f, 1.0f);
        demo_ui_float(ctx, "Hour of day", &a->time_of_day, 0.0f, 24.0f);
        demo_ui_float(ctx, "Turbidity", &a->turbidity, 1.0f, 12.0f);
        demo_ui_float(ctx, "March budget", &a->budget, 8.0f, 128.0f);
        demo_ui_float(ctx, "Refine steps", &a->refine, 0.0f, 6.0f);
        demo_ui_float(ctx, "Cloud scale, percent", &a->cloud_scale, 10.0f, 100.0f);
        demo_ui_float(ctx, "History weight", &a->history, 0.0f, 0.95f);
        demo_ui_bool(ctx, "HUD", &a->show_hud);
        a->coverage = a->weather.coverage;
    }
    nk_end(ctx);
}

static void app_shutdown(App *a)
{
    Demo *d = &a->demo;
    /* demo.c destroys d->program itself once this hook returns, and d->program is
     * whichever parity ran last, so the one to free here is the other. Freeing
     * both would double-free the current one. */
    for (uint32_t k = 0u; k < 2u; ++k)
        if (a->programs[k] != d->program) frame_program_destroy(d->gpu, a->programs[k]);
}

#include "check.inc"

int main(int argc, char **argv)
{
    static App app;
    static const DemoOption options[] = {
        {"--model",     .u32 = &app.flight_model,     .lo = 0,    .hi = 1,     .help = "0 six-DOF aerodynamic, 1 arcade rate controller"},
        {"--clouds",    .u32 = &app.cloud_mode,       .lo = 0,    .hi = 4,     .help = "0 off, 1 analytic, 2 volume, 3 adaptive, 4 temporal"},
        {"--traffic",   .u32 = &app.traffic_count,    .lo = 0,    .hi = FS_MAX_TRAFFIC, .help = "aircraft flying the pursuit law"},
        {"--camera",    .u32 = &app.camera,           .lo = 0,    .hi = 2,     .help = "0 cockpit, 1 chase, 2 orbit"},
        {"--grid",      .u32 = &app.grid,             .lo = 8,    .hi = 128,   .help = "terrain quads per side, per ring"},
        {"--rings",     .u32 = &app.rings,            .lo = 1,    .hi = 8,     .help = "terrain clipmap rings"},
        {"--spacing",   .f32 = &app.ring_spacing,     .lo = 2.0,  .hi = 128.0, .help = "finest ring spacing, metres"},
        {"--budget",    .f32 = &app.budget,           .lo = 8,    .hi = 128,   .help = "coarse cloud raymarch samples"},
        {"--refine",    .f32 = &app.refine,           .lo = 0,    .hi = 6,     .help = "fine steps between coarse ones"},
        {"--scale",     .f32 = &app.cloud_scale,      .lo = 10,   .hi = 100,   .help = "cloud buffer size, percent of the output"},
        {"--history",   .f32 = &app.history,          .lo = 0.0,  .hi = 0.95,  .help = "temporal blend weight (mode 4)"},
        {"--coverage",  .f32 = &app.coverage,         .lo = 0.0,  .hi = 1.0,   .help = "0 clear sky, 1 overcast"},
        {"--turbidity", .f32 = &app.turbidity,        .lo = 1.0,  .hi = 12.0,  .help = "haze"},
        {"--hour",      .f32 = &app.time_of_day,      .lo = 0.0,  .hi = 24.0,  .help = "local time in decimal hours"},
        {"--seed",      .u32 = &app.seed32,           .lo = 1,    .hi = 999999, .help = "traffic and weather seed"},
        {"--hud",       .off = &app.show_hud,         .help = "hide the cockpit HUD"},
        {0},
    };

    /* Defaults live here and not in app_init: demo_open parses the command line
     * before init runs, so anything init assigned would overwrite what the user
     * asked for, and --help prints these values. */
    app.traffic_count = 12u;
    app.grid          = 64u;
    app.rings         = 8u;
    app.ring_spacing  = 16.0f;
    app.budget        = 48.0f;
    app.refine        = 3.0f;
    app.cloud_scale   = 50.0f;
    app.history       = 0.82f;
    /* Scattered cumulus, not the overcast deck 0.7 and up bakes: the cell area
     * the weather system seeds is roughly the coverage value, so a default
     * above a half hands the player a white ceiling instead of a sky. */
    app.coverage      = 0.35f;
    app.turbidity     = 3.0f;
    app.time_of_day   = 9.5f;
    app.cloud_mode    = 4u;
    app.seed32        = 20260101u;
    app.show_hud      = true;

    demo_defaults(&app.demo, "flightsim", "flightsim — terrain, weather and volumetric clouds");
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
