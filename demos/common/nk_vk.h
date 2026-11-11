/* nk_vk.h — Nuklear backend on vk.h (bindless SSBO, no vertex input).
 *
 * Why not upstream nuklear_glfw_vulkan.h? It needs VkPhysicalDevice,
 * VkQueue, queue-family index and swapchain VkImageViews — all hidden by
 * vk.h's abstraction. This backend uses vk.h's intended pattern (§17):
 * Nuklear draw commands are de-indexed on the CPU into a storage-buffer
 * array, uploaded once per frame, and drawn as instanced triangles
 * (pass_draw(3, triangles)) with the vertex shader fetching by
 * gl_InstanceIndex/gl_VertexIndex. One font texture (bindless sampled
 * image), one pipe, one pass. Clip rectangles are honoured: one draw per run
 * of commands sharing a rectangle, tested in the fragment stage.
 *
 * Per-frame flow (galaxy main loop):
 *   nkvk_new_frame(vk, state, dt);          // nk_input_begin/feed/end + dt
 *   ... nk_begin / widgets / nk_end ...     // user UI code
 *   nkvk_prepare(vk, gpu, w, h);            // nk_convert + de-index + upload
 *   frame_begin / frame_run (UI pass calls nkvk_draw) / frame_end
 *
 * Toggle with F1 (or `): nkvk_toggle_visible(). When hidden, prepare()
 * uploads nothing and the UI pass draws 0 triangles.
 */

#ifndef NK_VK_H
#define NK_VK_H

#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_DEFAULT_FONT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#include "nuklear.h"

#include "vk.h"
#include "mu_input.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct NkVk NkVk;

/* GPU vertex (must match NkVertex in shaders/nk.slang, 32 B). */
typedef struct NkVkVertex {
    float pos[2];
    float uv[2];
    float col[4];
} NkVkVertex;

/* Push constants (must match NkPush in shaders/nk.slang, 48 B). */
typedef struct NkVkPush {
    uint32_t vert_offset;    /* byte offset into storage_buffers */
    uint32_t texture_index;  /* font atlas bindless index */
    uint32_t sampler_index;  /* linear sampler bindless index */
    float    width;
    float    height;
    float    pad[3];         /* float4 below sits on a 16-byte boundary in the shader */
    float    clip[4];        /* x0, y0, x1, y1 in pixels; fragments outside are discarded */
} NkVkPush;
_Static_assert(sizeof(NkVkPush) == 48, "NkVkPush must match NkPush in nk.slang");

/* vs_spv / fs_spv: paths to demos/common/shaders/nk.vert.spv / nk.frag.spv, built
 * from shaders/nk.slang (entries vertNk / fragNk).
 * color_format: swapchain format (the UI pass draws to TARGET_SWAPCHAIN).
 * Returns NULL on failure (gpu_last_error() has the reason). */
NkVk *nkvk_create(Gpu *g, const char *vs_spv, const char *fs_spv,
                  Format color_format, uint32_t width, uint32_t height);
void nkvk_destroy(NkVk *vk, Gpu *g);

struct nk_context *nkvk_context(NkVk *vk);
Pipe nkvk_pipe(NkVk *vk);
/* BufUse for the UI pass's buf_reads (max-size range; barrier covers all). */
BufUse nkvk_buf_use(NkVk *vk);
uint32_t nkvk_triangle_count(NkVk *vk);

bool nkvk_visible(NkVk *vk);
void nkvk_set_visible(NkVk *vk, bool visible);
bool nkvk_toggle_visible(NkVk *vk); /* returns new visibility */

/* Start a frame: nk_input_begin + feed mouse/keys/wheel + nk_input_end.
 * Mouse position/buttons come from state; keys from the MuKey map. */
void nkvk_new_frame(NkVk *vk, const MuInputState *state, float dt);

/* Convert draw commands, de-index into NkVkVertex[], upload to the SSBO.
 * Call after the user has built the UI (nk_begin/widgets/nk_end) and
 * before frame_begin(). No-op (0 triangles) when hidden. */
void nkvk_prepare(NkVk *vk, Gpu *g, uint32_t width, uint32_t height);

/* Record the UI draw inside a pass. */
void nkvk_draw(NkVk *vk, PassCtx *p);

#endif /* NK_VK_H */
