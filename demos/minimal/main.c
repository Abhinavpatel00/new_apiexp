/* minimal.c — the smallest honest thing you can build with vk.h.
 *
 * This demo deliberately uses nothing but two headers:
 *
 *     #include "vk.h"        the backend
 *     #include "platform.h"  the window, the input, the VkSurfaceKHR
 *
 * No demo.h, no mesh.h, no canvas.h, no nuklear, no helper of any kind. There
 * is no "scene" abstraction, no material system, no render graph: one pass,
 * one cleared target, and a triangle whose colour spins.
 *
 * The whole program is the four things a real app does:
 *
 *   1. create a window and a GPU, and attach one to the other
 *   2. compile a pipeline once (never per frame)
 *   3. describe the frame program once (never per frame)
 *   4. loop: frame_begin / record / frame_end
 *
 * Read it top to bottom; it is in that order.
 *
 * Build (see the `minimal` target in CMakeLists.txt):
 *
 *     minimal  demos/minimal/main.c  demos/common/platform.c
 *              src/vk.c  external/volk/volk.c  external/mu_lib/mu/mu_id_pool.c
 *              -I src -I demos/common -I external/mu_lib/mu
 *              -I external/glfw/include -I external/vulkan/include
 *              -I external/volk  glfw -ldl -lm
 *
 * Controls: Escape or Q quits. Space freezes the rotation.
 */

#include "vk.h"
#include "platform.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * 0. THE SHADERS
 *
 * Every other demo in this tree loads .spv from disk, produced by
 * scripts/build_shaders.sh from a .slang source. This file carries its own
 * SPIR-V instead, so that "copy this one file and build it" is true.
 *
 * Two modules:
 *   vs  vertex    gl_VertexIndex -> vec4 gl_Position (screen-covering triangle)
 *   fs  fragment  vec4 push constant -> vec4 out at location 0
 *
 * Each instruction is one word of `wordCount << 16 | opcode`, so `0x00020013`
 * is "2 words, OpTypeVoid". The header is generated and validated:
 *
 *     python3 demos/minimal/gen_spirv.py
 *
 * which checks section order, operand counts against spirv.core.grammar.json,
 * id bounds and id definition before writing anything.
 * ==========================================================================*/

#include "minimal_spirv.h"

/* ============================================================================
 * 1. WHAT THIS APP OWNS
 *
 * Four things: the window, the GPU, the pipeline, and the frame program. That
 * is genuinely all the state this demo has. The push constant payload is the
 * only per-frame data.
 * ==========================================================================*/

/* Must match the shader: vec4 colour at push offset 0. The backend's push
 * range is the full PUSH_MAX bytes, so a small struct is legal and the rest is
 * zero-filled by pass_push(). */
typedef struct Push {
    float colour[4];
} Push;

typedef struct App {
    Platform     *platform;
    Gpu          *gpu;
    Pipe          pipe;
    FrameProgram *program;
    Target        colour;   /* the swapchain pseudo-target */
    Format        out_format; /* what the surface actually gave us */
    float         angle;    /* radians, advanced by time */
    bool          frozen;
} App;

/* ============================================================================
 * 2. THE FRAME PROGRAM
 *
 * A FrameProgram is the resolved pass list: barriers planned across passes,
 * layouts derived, transients allocated. Build it when configuration changes
 * (a resize, a new pipeline), NOT per frame — that is the whole point of the
 * split between build and record.
 *
 * The PassDesc array is copied by frame_program_build(), so a stack array is
 * fine. `record` is left NULL: this demo records through frame_pass(), which
 * is the direct form and needs no callback.
 * ==========================================================================*/

static FrameProgram *build_program(App *a) {
    AttachDesc clear;
    PassDesc   pass;

    clear = attach_clear(a->colour, 0.04f, 0.05f, 0.08f, 1.0f);

    memset(&pass, 0, sizeof pass);
    pass_name_set(pass.name, "Triangle");   /* truncating, not strcpy */
    pass.kind        = PASS_GRAPHICS;
    pass.pipe        = a->pipe;
    pass.state       = draw_state_opaque(SC_1, CULL_NONE);
    pass.colors      = &clear;
    pass.color_count = 1u;

    return frame_program_build(a->gpu, &pass, 1u);
}

/* ============================================================================
 * 3. SETUP
 *
 * Note the order. The window has to exist before the surface, the surface
 * before the swapchain, so: window -> GPU -> surface -> set_surface. A GPU
 * with no surface runs headless and renders into its own colour image, which
 * is what the --check paths in the other demos rely on.
 * ==========================================================================*/

static bool app_start(App *a, uint32_t width, uint32_t height) {
    GpuDesc   desc;
    ShaderId  vs = SHADER_NONE, fs = SHADER_NONE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;

    memset(a, 0, sizeof *a);

    a->platform = platform_create(width, height, "minimal (vk.h + platform.h)");
    if (!a->platform) { fprintf(stderr, "platform_create failed\n"); return false; }

    memset(&desc, 0, sizeof desc);
    desc.app_name   = "minimal";
    desc.width      = width;
    desc.height     = height;
    desc.vsync      = true;
    desc.validation = true;   /* keep it on while reading this file */

    a->gpu = gpu_create(&desc);
    if (!a->gpu) {
        fprintf(stderr, "gpu_create: %s\n", gpu_last_error(NULL));
        return false;
    }

    /* The surface is the window's, handed to the backend's instance. */
    if (!platform_create_surface(a->platform, gpu_raw_instance(a->gpu), &surface) ||
        !set_surface(a->gpu, surface)) {
        fprintf(stderr, "surface: %s\n", gpu_last_error(a->gpu));
        return false;
    }

    /* The pipeline's colour format must be the one the swapchain actually
     * got, and that is the surface's choice, not ours: a surface that does not
     * support BGRA8_SRGB gets RGBA8. gpu_info() reports what was chosen, so ask
     * instead of assuming, and build the pipe AFTER set_surface(). */
    {
        GpuInfo info;
        gpu_info(a->gpu, &info);
        a->out_format = info.swapchain_format;
        printf("minimal: %s, %ux%u, %s\n", info.device_name,
               info.extent_w, info.extent_h,
               info.swapchain_srgb ? "sRGB swapchain" : "linear swapchain");
    }

    /* Shaders first, then the pipeline that names them. A Pipe is interned:
    * asking twice with the same inputs returns the same handle, so this is
    * also where the pipeline count is decided. Never do it in the frame loop. */
    if (!shader_register_spv(a->gpu, k_vs_words, k_vs_words_count,
                             "minimal.vert.spv", "main", STAGE_VERTEX, &vs) ||
        !shader_register_spv(a->gpu, k_fs_words, k_fs_words_count,
                             "minimal.frag.spv", "main", STAGE_FRAGMENT, &fs)) {
        fprintf(stderr, "shaders: %s\n", gpu_last_error(a->gpu));
        return false;
    }
    a->pipe = pipe_intern_graphics(a->gpu, vs, fs,
                                   a->out_format,   /* the swapchain's, above */
                                   FMT_NONE,        /* no depth attachment      */
                                   BLEND_OPAQUE);
    if (!HANDLE_VALID(a->pipe)) {
        fprintf(stderr, "pipe: %s\n", gpu_last_error(a->gpu));
        return false;
    }

    a->colour  = TARGET_SWAPCHAIN;
    a->program = build_program(a);
    if (!a->program) {
        fprintf(stderr, "program: %s\n", gpu_last_error(a->gpu));
        return false;
    }
    a->angle = 0.0f;
    return true;
}

static void app_stop(App *a) {
    if (a->program) frame_program_destroy(a->gpu, a->program);
    /* The swapchain is the surface's to destroy; TARGET_SWAPCHAIN is not a
     * pool entry, so there is no target_destroy() for it. */
    if (a->gpu) gpu_destroy(a->gpu);
    if (a->platform) platform_destroy(a->platform);
}

/* ============================================================================
 * 4. ONE FRAME
 *
 * frame_begin() may return false: the window is minimised, or acquiring the
 * next swapchain image failed because the surface changed size. That is not an
 * error — it means "no frame this time". Present nothing, try again next turn.
 * ==========================================================================*/

static void app_frame(App *a, float dt) {
    PassCtx *pass;
    Push     push;

    if (!a->frozen) a->angle += dt;

    if (!frame_begin(a->gpu)) return;

    /* Pull the pass out of the program by name. The name has to be the one
     * that is due next, so a reordered program fails here rather than drawing
     * into the wrong targets. */
    pass = frame_pass(a->gpu, a->program, "Triangle");
    if (pass) {
        /* Constant colour, spun through the hue wheel so motion is visible
         * without a vertex buffer. */
        push.colour[0] = 0.5f + 0.5f * sinf(a->angle);
        push.colour[1] = 0.5f + 0.5f * sinf(a->angle + 2.0943951f);
        push.colour[2] = 0.5f + 0.5f * sinf(a->angle + 4.1887902f);
        push.colour[3] = 1.0f;
        pass_push(pass, &push, (uint32_t)sizeof push);
        pass_draw(pass, 3u, 1u, 0u, 0u);
    }

    /* Ends the pass, flushes the planned barriers, submits, presents. */
    frame_end(a->gpu);
}

/* ============================================================================
 * 5. MAIN
 * ==========================================================================*/

int main(int argc, char **argv) {
    App            app;
    uint64_t       previous;
    uint32_t       frames = 0u;
    uint32_t       limit  = 0u;   /* 0 = run until the window closes */
    int            i;

    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) limit = (uint32_t)atoi(argv[++i]);
        else if (!strncmp(argv[i], "--frames=", 9))       limit = (uint32_t)atoi(argv[i] + 9);
        else {
            fprintf(stderr, "usage: %s [--frames N]\n", argv[0]);
            return 2;
        }
    }

    if (!app_start(&app, 960u, 540u)) {
        app_stop(&app);
        return 1;
    }

    previous = platform_time_ns(app.platform);
    while (!platform_should_close(app.platform)) {
        uint64_t now;
        float    dt;

        platform_poll(app.platform);
        if (platform_key_pressed(app.platform, MU_KEY_ESCAPE) ||
            platform_key_pressed(app.platform, MU_KEY_Q)) break;
        if (platform_key_pressed(app.platform, MU_KEY_SPACE)) app.frozen = !app.frozen;

        now      = platform_time_ns(app.platform);
        dt       = (float)((double)(now - previous) * 1e-9);
        previous = now;
        /* Clamp: a breakpoint or a resize stall is not a 4-second frame. */
        if (dt > 0.1f) dt = 0.1f;

        app_frame(&app, dt);

        if (limit && ++frames >= limit) break;
    }

    app_stop(&app);
    return 0;
}
