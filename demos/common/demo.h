/* demo.h — the scaffolding every demo under demos/ shares.
 *
 * A demo is a pass graph plus a few shaders. Everything else — the window, the
 * device, the Nuklear overlay, the frame loop, command-line parsing, the
 * headless capture and the acceptance-test plumbing — is the same in all of
 * them and lives here.
 *
 *     typedef struct App { Demo demo; ...own state... } App;
 *
 *     static void app_init(App *a)  { ...targets, pipes, d->program... }
 *     static void app_ui(App *a, struct nk_context *ctx) { ...sliders... }
 *     static void app_frame(App *a) {
 *         ...per-frame constants and uploads...
 *         if (!demo_frame_begin(d)) return;
 *         pass = demo_pass(d, "Trace");
 *         pass_push(pass, &push, sizeof push);
 *         pass_draw_fullscreen_triangle(pass, d->width, d->height);
 *         demo_present(d);
 *         demo_frame_end(d);
 *     }
 *     static void app_check(App *a) { ...numeric acceptance test... }
 *
 *     int main(int argc, char **argv) {
 *         static App app;
 *         static const DemoOption options[] = {
 *             {"--view", .u32 = &app.view, .lo = 0, .hi = 4, .help = "0 shaded, 1 normals, ..."}, {0}};
 *         demo_defaults(&app.demo, "sdf", "SDF — sphere tracing");
 *         demo_open(&app.demo, options, argc, argv);
 *         app_init(&app);
 *         while (demo_running(&app.demo)) {
 *             if (app.demo.ui) app_ui(&app, app.demo.ui);
 *             app_frame(&app);
 *         }
 *         if (app.demo.check) app_check(&app);
 *         return demo_close(&app.demo);
 *     }
 *
 * The demo owns its loop and its functions take an App *: nothing here calls
 * back into a demo, so nothing is passed through a void pointer.
 *
 * Failure. Nothing in a demo tests the result of a setup call. Every helper
 * here that can fail (a shader that is not there, a target the device cannot
 * make, a readback that never arrives) prints the reason and exits through
 * demo_fatal(). A demo's init is a straight list of what it creates.
 *
 * Output convention. The last pass of a demo writes display-referred colour to
 * `demo->out`: the swapchain in a window, an RGBA8 sRGB target when headless
 * (so `--headless FILE` writes exactly what a window would show). When
 * `demo->out_encode_srgb` is set the attachment is not an sRGB format and the
 * shader must encode; demo_present_pass() handles that.
 *
 * Common command line (demo_open):
 *     --headless [FILE]   no window; write a PPM (default <name>.ppm)
 *     --size W H          output size
 *     --frames N          headless: frames rendered before the capture (default 2)
 *                         windowed: quit after N frames
 *     --time T            freeze the demo clock at T seconds
 *     --check             headless; run the demo's numeric acceptance test,
 *                         exit status 0 = pass
 *     --validate          Khronos validation layers (if installed)
 *     --perf FILE         write the pass timing report after the last frame
 */
#ifndef MU_DEMO_H
#define MU_DEMO_H

#include "nk_vk.h"
#include "platform.h"
#include "vk.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* One command-line option. A table of these (ended by {0}) replaces a parser.
 * The pointers say what the option does, and the compiler checks their types:
 * `flag` is set, `off` is cleared, `u32` or `f32` takes the next argument,
 * which must lie in [lo, hi]. One option may do several of these ("--fog X"
 * stores X and sets a flag). --help prints the table with each default.
 *
 *     {"--view", .u32 = &app.view, .lo = 0, .hi = 4, .help = "0 lit, 1 normal, ..."},
 *     {"--half", .flag = &app.half, .help = "trace at half resolution"},
 */
typedef struct DemoOption {
    const char *name;        /* "--steps"                                       */
    bool       *flag, *off;
    uint32_t   *u32;
    float      *f32;
    double      lo, hi;      /* inclusive                                       */
    const char *help;
} DemoOption;

typedef struct Demo {
    /* ---- identity and options (demo_defaults, then the command line) ------ */
    const char *name;        /* "sdf": log prefix, default output name        */
    const char *title;       /* window title                                   */
    uint32_t    width, height;
    bool        headless;
    const char *out_path;    /* NULL = do not write an image                   */
    bool        check;
    bool        validate;
    uint32_t    frames;      /* 0 = default (2 headless, unlimited windowed)   */
    bool        time_fixed;
    const char *perf_path;

    /* ---- runtime (demo_open) ---------------------------------------------- */
    Gpu          *gpu;
    Platform     *platform;  /* NULL when headless                             */
    NkVk         *nkvk;      /* NULL when headless                             */
    GpuInfo       info;
    FrameProgram *program;   /* what demo_pass() records; set by the demo      */
    struct nk_context *ui;   /* this frame's panel, or NULL (see demo_running) */
    const char   *note;      /* a line for the end of --help, or NULL          */
    const DemoOption *options;
    bool          started;   /* demo_running() has been called                 */
    uint32_t      checks_failed;

    Target  out;             /* final attachment (see "Output convention")     */
    Format  out_format;
    bool    out_encode_srgb;

    Sampler  linear_clamp, linear_repeat, nearest_clamp;
    uint32_t linear_clamp_index, linear_repeat_index, nearest_clamp_index;
    ShaderId fullscreen_vs;  /* demos/common/shaders/fullscreen.vert.spv        */

    float    time, dt;       /* seconds; time is frozen by --time              */
    uint64_t frame;
    bool     ui_hovered;     /* the pointer is over a Nuklear window           */

    /* ---- state owned by the helpers below --------------------------------- */
    AttachDesc ui_color;
    BufUse     ui_use;
    struct {
        Pipe       pipe;
        AttachDesc color;
        Target     read;
        uint32_t   source_index;
        uint32_t   tonemap;      /* DemoTonemap                                */
        float      exposure_ev;
        bool       dither;
    } present;
} Demo;

void demo_defaults(Demo *d, const char *name, const char *title);

/* ---- the run ---------------------------------------------------------------
 *
 * A demo's main() is the whole run, top to bottom:
 *
 *     demo_defaults(d, "sdf", "SDF - sphere tracing");
 *     demo_open(d, options, argc, argv);
 *     app_init(&app);
 *     while (demo_running(d)) {
 *         if (d->ui) app_ui(&app, d->ui);
 *         app_frame(&app);
 *     }
 *     if (d->check) app_check(&app);
 *     return demo_close(d);
 *
 * and a frame is one function that records its passes in order:
 *
 *     if (!demo_frame_begin(d)) return;
 *     pass = demo_pass(d, "Scene");   pass_push(pass, ...); pass_draw(pass, ...);
 *     demo_present(d);
 *     demo_frame_end(d);
 */

/* Read the command line (the common options, then `options`; --help prints
 * both and exits), open the window unless headless, create the device, the
 * samplers, the fullscreen vertex stage and `demo->out`. */
void demo_open(Demo *d, const DemoOption *options, int argc, char **argv);

/* True while there is another frame to draw. Headless: `frames` times, then
 * the image is written. In a window: until it is closed; each call polls the
 * input, advances the clock and starts the Nuklear frame, and sets `demo->ui`
 * to the Nuklear context, or to NULL while the panel is hidden (F1). */
bool demo_running(Demo *d);

/* Open a frame. False when the swapchain refuses it (a resize in progress):
 * record nothing and come back next frame. Headless it cannot refuse. */
bool demo_frame_begin(Demo *d);

/* The next pass of `demo->program`; `name` must be the pass that is due. */
PassCtx *demo_pass(Demo *d, const char *name);

/* Record the pass demo_present_pass() described. */
void demo_present(Demo *d);

/* Record the Nuklear overlay (in a window) and submit the frame. */
void demo_frame_end(Demo *d);

/* --perf report, the acceptance line after a --check, teardown. Returns the
 * process exit status: 1 if a demo_check() failed. */
int demo_close(Demo *d);

/* Print "[name] message: <the library's last error>" and exit(1). */
void demo_fatal(const Demo *d, const char *fmt, ...);

/* ---- resources (all fatal on failure) ------------------------------------- */

/* Load one SPIR-V stage. */
ShaderId demo_shader(Demo *d, const char *path, ShaderStage2 stage);

/* Colour or depth target the size of the output (scale 1) or a fraction of
 * it. Sampled; `storage` adds TGT_STORAGE for compute writes. Optionally
 * returns the bindless index of the sampled view. */
Target demo_target(Demo *d, const char *name, Format format, uint32_t width, uint32_t height,
                   bool storage, uint32_t *bindless_index);

/* Allocate from the device arena and upload `bytes` (NULL = allocate only). */
/* A volume: a 3D storage image for compute passes to write and any stage to
 * sample (declare it in tex_reads / tex_writes like a 2D target). Shaders
 * reach it through mu_volume.slang. demo_readback() returns it slice by slice. */
Target demo_volume(Demo *d, const char *name, Format format, uint32_t width, uint32_t height,
                   uint32_t depth, uint32_t *bindless_index);

BufSlice demo_alloc(Demo *d, const void *bytes, uint64_t size);
void     demo_upload(Demo *d, BufSlice dst, const void *bytes, uint64_t size);

/* ---- passes -------------------------------------------------------------- */

typedef enum DemoTonemap {
    DEMO_TONEMAP_NONE = 0, /* clamp                                            */
    DEMO_TONEMAP_REINHARD,
    DEMO_TONEMAP_ACES_FITTED,
    DEMO_TONEMAP_ACES,
    DEMO_TONEMAP_NEUTRAL,
    DEMO_TONEMAP_UCHIMURA,
    DEMO_TONEMAP_HABLE,
    DEMO_TONEMAP_AGX,
    DEMO_TONEMAP_COUNT,
} DemoTonemap;
extern const char *const demo_tonemap_names[DEMO_TONEMAP_COUNT];

/* The final pass: sample `source` (scene-linear), apply exposure and
 * `demo->present.tonemap`, encode, write `demo->out`. */
PassDesc demo_present_pass(Demo *d, Target source);

/* Build a frame program from `passes`, with the Nuklear overlay appended when
 * there is a window (so the array needs room for one more). Fatal if a pass
 * has no pipeline or the build fails. */
FrameProgram *demo_program(Demo *d, PassDesc *passes, uint32_t count);

/* ---- for --check: readback, results --------------------------------------- */

/* Blocking readback of mip 0 of `target` into `dst` (width*height*format
 * bytes). Retries: readback_ready() gives up after a second, which a software
 * rasterizer with a cold shader cache can exceed. */
void demo_readback(Demo *d, Target target, void *dst, size_t bytes);

/* The same for a half-float target, converted: `count` values (pixels times
 * channels) in a malloc'd array the caller frees. */
float *demo_read_floats(Demo *d, Target target, size_t count);

/* The bytes of a storage-buffer slice, as the last submitted frame left them:
 * indirect arguments, counters, anything a shader wrote. `bytes` >= src.size. */
void demo_readback_buffer(Demo *d, BufSlice src, void *dst, size_t bytes);

/* demo->out as a binary PPM. */
void demo_write_ppm(Demo *d, const char *path);

/* Report one acceptance check: prints "[name] check <label>: <value> (<rule>)
 * PASS|FAIL". A failed one fails the run; nothing needs to be accumulated.
 * Returns `ok` for the check that wants to stop early. */
bool demo_check(Demo *d, const char *label, double value, const char *rule, bool ok);

float demo_half_to_float(uint16_t h);

/* ---- camera --------------------------------------------------------------- */

typedef struct DemoOrbit {
    float target[3];
    float distance, yaw, pitch; /* radians                                    */
    float fov_deg, znear, zfar;
    float min_distance, max_distance;
} DemoOrbit;

/* Left-drag orbits, wheel zooms; ignored while the pointer is over the UI. */
void demo_orbit_input(Demo *d, DemoOrbit *o);
/* Column-major (mu_math) view and zero-to-one projection; any may be NULL. */
void demo_orbit_camera(const DemoOrbit *o, float aspect, float view[16], float proj[16],
                       float view_proj[16], float eye[3]);

/* ---- Nuklear rows ---------------------------------------------------------- */

void demo_ui_float(struct nk_context *ctx, const char *label, float *value, float lo, float hi);
void demo_ui_bool(struct nk_context *ctx, const char *label, bool *value);
/* Returns true when the selection changed. */
bool demo_ui_choice(struct nk_context *ctx, const char *label, const char *const *names,
                    uint32_t count, uint32_t *index);
void demo_ui_text(struct nk_context *ctx, const char *fmt, ...);

#endif /* MU_DEMO_H */
