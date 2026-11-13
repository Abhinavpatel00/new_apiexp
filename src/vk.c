/* vk.c — implementation of the single-header mu_gfx Vulkan library.
 *
 * Volk is included before the public header so VK_NO_PROTOTYPES is active.
 * Resource identity uses fixed tables and generation handles; descriptor slots
 * come from mu_id_pool and are released only after timeline retirement.
 */
#define _POSIX_C_SOURCE 200809L

#include "volk.h"
#include "vk.h"
#include "mu_id_pool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

/* ============================================================================
 * 0. INTERNAL CAPACITIES
 *    Compile-time array bounds, exactly like §1 of the header. Nothing here
 *    grows at runtime: gpu_create() makes one allocation for the whole device
 *    state (calloc), and the only other allocations are the SPIR-V blobs and
 *    the pipeline-cache readback, both off the frame path.
 * ==========================================================================*/
#define SLOT_BUFFERS      512
#define SLOT_TEXTURES     2048
#define SLOT_SAMPLERS     BINDLESS_SAMPLERS
#define SLOT_SHADERS      128
#define SLOT_READBACKS    32
#define SLOT_UPLOADS      8
#define STAGING_CHUNKS    8
#define STAGING_CHUNK_BYTES (4u << 20)
#define DELETE_QUEUE      256
#define ACTIVE_BINDLESS   4096   /* <= BINDLESS_TEXTURES; see below */
#define ARENA_DEVICE_BYTES  (64u << 20)
#define ARENA_TRANSIENT_BYTES (16u << 20)
#define GEOM_PUSH_BYTES   16u
#define SMALL_UPLOAD      65536u
#define ERROR_BYTES       256
#define NAME_BYTES        64

/* Volk owns Vulkan loader dispatch and loads the global/instance/device
 * command pointers. The backend calls the normal vk* names; no per-function
 * PFN table or direct Vulkan-loader link is needed here. */
static bool s_volk_initialized;

/* ---- small helpers -------------------------------------------------------*/
static uint64_t align_up(uint64_t v, uint64_t a) {
    return a ? ((v + a - 1u) / a) * a : v;
}
static uint32_t min_u32(uint32_t a, uint32_t b) { return a < b ? a : b; }
static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}
/* Names are copied byte-by-byte rather than through snprintf: several of them
 * move between fields of the SAME struct (a pass name into its PassStat), and
 * -Wrestrict correctly complains when snprintf's destination and a %s
 * argument may alias. */
static void copy_name(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    if (!src) src = "";
    while (i + 1u < cap && src[i]) { dst[i] = src[i]; ++i; }
    dst[i] = 0;
}

/* ============================================================================
 * 1. INTERNAL TABLES
 * ==========================================================================*/

typedef struct BufferSlot {
    uint32_t    gen;
    bool        live;
    VkBuffer    buffer;
    VkDeviceMemory memory;
    VkDeviceSize mem_offset;   /* offset inside the arena's memory object    */
    uint64_t    size;
    uint64_t    committed;     /* bytes of this buffer a command buffer may touch */
    uint64_t    align;
    uint32_t    usage;         /* BufferUsage                               */
    ResClass    cls;
    Arena       arena;
    bool        host_visible;
    void       *mapped;
    uint64_t    device_address;
    char        name[NAME_BYTES];
} BufferSlot;

typedef struct TextureSlot {
    uint32_t gen;
    bool     live;
    VkImage  image;
    VkImageView view;
    VkImageView storage_view;
    VkDeviceMemory memory;
    VkDeviceSize mem_offset;
    uint32_t width, height, depth, layers, mips;
    Format   format;
    uint32_t usage;
    ResClass cls;
    VkImageLayout layout;
    uint32_t bindless_index;
    char     name[NAME_BYTES];
} TextureSlot;

typedef struct TargetSlot {
    uint32_t gen;
    bool     live;
    TextureSlot *tex;          /* resolved at the boundary, never re-checked */
    /* Layout this target ended the PREVIOUS frame in. A transient target is
     * re-derived every frame and its texture is rewritten at the top of the
     * frame program, so the plan's baked-in oldLayout (UNDEFINED) is right for
     * it; a persistent target keeps its real layout here and the barrier takes
     * it at record time. */
    VkImageLayout frame_layout;
    uint32_t width, height;
    Format   format;
    uint32_t usage;
    SampleCount samples;
    ResClass cls;
    bool     is_swapchain;
    char     name[NAME_BYTES];
} TargetSlot;

typedef struct SamplerSlot {
    uint32_t gen;
    bool     live;
    VkSampler sampler;
    uint32_t bindless_index;
} SamplerSlot;

typedef struct ShaderSlot {
    bool     live;
    VkShaderModule module;
    char     path[128];
    char     entry[64];
    ShaderStage2 stage;
    uint32_t spv_words;
    uint32_t code_bytes;
} ShaderSlot;

typedef struct PipeSlot {
    uint32_t gen;
    bool     live;
    bool     is_compute;
    VkPipeline pipeline;
    PipeKey  key;
    char     vs_entry[64];
    char     fs_entry[64];
    ShaderId vs, fs;
    uint32_t color_count;
    char     name[MAX_PIPE_NAME];
} PipeSlot;

typedef struct ArenaState {
    Buffer   handle;
    VkBuffer buffer;
    VkDeviceMemory memory;
    void    *mapped;
    uint64_t capacity;
    uint64_t used;
    uint64_t peak;
    uint32_t allocations;
    uint64_t base_offset;      /* transient: frame slot's subregion start   */
} ArenaState;

typedef struct StagingChunk {
    VkBuffer buffer;
    VkDeviceMemory memory;
    void    *mapped;
    uint64_t used;
    uint64_t retire_value;     /* 0 = free                                  */
    bool     in_flight;
} StagingChunk;

typedef struct DeleteItem {
    uint64_t retire_value;
    enum { DEL_BUFFER, DEL_MEMORY, DEL_IMAGE, DEL_IMAGE_VIEW, DEL_SAMPLER,
           DEL_PIPELINE, DEL_SHADER_MODULE, DEL_COMMAND_BUFFER,
           DEL_QUERY_POOL, DEL_SWAPCHAIN_IMAGE } kind;
    uint64_t a, b;
    uint32_t c;
} DeleteItem;

typedef struct FrameSlot {
    VkCommandBuffer cmd;
    VkQueryPool     queries;
    VkSemaphore     present_done;
    VkSemaphore     acquire_done;
    FrameProgram   *pending_program;
    uint64_t        submit_value;
    bool            in_flight;
    FrameStats      pending;
} FrameSlot;

typedef struct BarrierBuf {
    uint32_t slot;             /* 1-based Buffer index                      */
    uint64_t offset, size;
    VkAccessFlags src_access, dst_access;
    VkPipelineStageFlags src_stage, dst_stage;
} BarrierBuf;

typedef struct BarrierImg {
    uint32_t slot;             /* 1-based Target index                      */
    VkImageLayout old_layout, new_layout;
    /* The planner chose UNDEFINED because the pass overwrites the whole image
     * (ATT_WRITE / ATT_DEPTH_WRITE) or because the resource is frame-local.
     * Only those barriers may discard; every other one takes its oldLayout
     * from where the image actually is, at RECORD time. A program is planned
     * once and submitted every frame, so a baked-in oldLayout is a guess. */
    bool discard;
    VkAccessFlags src_access, dst_access;
    VkPipelineStageFlags src_stage, dst_stage;
    uint32_t base_mip, mip_count, base_layer, layer_count;
} BarrierImg;

/* Sized by the most DISTINCT slots one pass can name, which is not the same as
 * the number of uses: a pass declares up to MAX_COLOR_ATTACHMENTS colour
 * attachments, one depth attachment, MAX_BINDS_PER_PASS sampled reads and
 * MAX_BINDS_PER_PASS storage writes, and every one of those may name a
 * different target. The table used to end at
 * MAX_COLOR_ATTACHMENTS + 1 + MAX_BINDS_PER_PASS, which is 25 entries for 41
 * possible slots; past 24 distinct images the remaining barriers were dropped
 * silently inside add_img_barrier(). */
#define MAX_IMAGES_PER_PASS (MAX_COLOR_ATTACHMENTS + 1u + \
                             MAX_BINDS_PER_PASS + MAX_BINDS_PER_PASS)

typedef struct BarrierPlan {
    uint32_t buf_count;
    uint32_t img_count;
    BarrierBuf bufs[MAX_BINDS_PER_PASS * 2u];
    BarrierImg imgs[MAX_IMAGES_PER_PASS];
} BarrierPlan;

typedef struct PassPlan {
    PassDesc  desc;
    AttachDesc colors[MAX_COLOR_ATTACHMENTS];
    AttachDesc depth_att;
    Target     tex_reads[MAX_BINDS_PER_PASS];
    Target     tex_writes[MAX_BINDS_PER_PASS];
    BufUse     buf_reads[MAX_BINDS_PER_PASS];
    BufUse     buf_writes[MAX_BINDS_PER_PASS];
    bool       has_depth;
    uint32_t   width, height;
    BarrierPlan barriers;
    PassStat   stat;
    VkImageLayout color_layouts[MAX_COLOR_ATTACHMENTS];
    VkImageLayout depth_layout;
} PassPlan;

struct FrameProgram {
    Gpu      *gpu;
    uint32_t  count;
    PassPlan  passes[MAX_PASSES];
    char      error[ERROR_BYTES];
};

struct PassCtx {
    Gpu         *gpu;
    VkCommandBuffer cmd;
    FrameProgram *program;
    PassPlan    *plan;
    uint32_t     index;
    uint32_t     active_scope;
    DrawState    state;
    bool         state_set;
    bool         pipe_bound;
    bool         scope_open;
};

struct Upload {
    Gpu      *gpu;
    uint32_t  chunk;
    bool      has_staged;
    uint64_t  bytes;
    VkCommandBuffer cmd;
    bool      cmd_begun;
};

struct Readback {
    Gpu      *gpu;
    VkBuffer  buffer;
    VkDeviceMemory memory;
    void     *mapped;
    void     *dst;
    uint64_t  size;
    uint64_t  ready_value;
    bool      copied;
    bool      live;
};

struct Gpu {
    VkInstance  instance;
    VkPhysicalDevice pdev;
    VkDevice    device;
    VkQueue     queue;
    uint32_t    queue_family;
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceMemoryProperties mem_props;
    VkPhysicalDeviceSubgroupProperties subgroup;

    VkCommandPool cmd_pool;
    VkDescriptorPool desc_pool;
    VkDescriptorSetLayout set_layout;
    VkDescriptorSet set;
    VkPipelineLayout pipe_layout;
    VkPipelineCache pipe_cache;
    VkSemaphore timeline;
    uint64_t    timeline_value;
    uint64_t    timeline_done;

    /* extended-dynamic-state-3 availability; entry points are loaded by Volk. */
    bool        eds3;

    /* optional WSI */
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    uint32_t    swapchain_image_count;
    VkImage     swapchain_images[MAX_FRAMES_IN_FLIGHT];
    VkImageView swapchain_views[MAX_FRAMES_IN_FLIGHT];
    uint32_t    swapchain_index;
    uint32_t    extent_w, extent_h;
    Format      swapchain_format;
    bool        swapchain_srgb;
    bool        vsync;
    bool        validation;
    bool        enable_escape;

    /* fixed tables */
    BufferSlot   buffers[SLOT_BUFFERS];
    TextureSlot  textures[SLOT_TEXTURES];
    TargetSlot   targets[MAX_TARGETS];
    SamplerSlot  samplers[SLOT_SAMPLERS];
    ShaderSlot   shaders[SLOT_SHADERS];
    PipeSlot     pipes[MAX_PIPES];
    ArenaState   arenas[3];
    StagingChunk staging[STAGING_CHUNKS];
    Readback     readbacks[SLOT_READBACKS];
    Upload       uploads[SLOT_UPLOADS];
    FrameSlot    frames[MAX_FRAMES_IN_FLIGHT];
    DeleteItem   deletes[DELETE_QUEUE];
    uint32_t     delete_count;
    mu_id_pool   texture_bindless_ids;
    mu_id_pool   sampler_bindless_ids;
    uint32_t     active_bindless;

    /* device facts */
    bool        bda;
    bool        shader_float16;
    bool        storage_16bit;
    bool        push16;
    bool        io16;
    bool        host_visible_device_local;
    uint64_t    device_memory_bytes;
    uint64_t    visible_vram_bytes;

    /* frame state */
    uint32_t    frames_in_flight;
    uint32_t    frame_slot;
    bool        frame_open;
    FrameProgram *active_program;
    PassCtx pass;                 /* the pass frame_pass() has open            */
    bool pass_open;
    uint32_t pass_next;           /* index of the pass frame_pass() begins next */
    FrameStats  active_stats;
    FrameStats  stats;
    double      cpu_start_ms;
    uint32_t    compiled_this_frame;
    uint32_t    descriptor_writes_total;
    uint32_t    escape_uses;
    char        error[ERROR_BYTES];
    char        validation_note[128];

    /* integration hooks */
    compile_fn compiler;
    void           *compiler_user;
};

/* Defined in §7 (frame lifecycle); used by gpu_destroy() in §2. Placed here
 * because it needs Gpu and DeleteItem to be complete types. */
static void delete_apply(Gpu *g, DeleteItem *d);
void gpu_destroy(Gpu *g);   /* gpu_create() unwinds through it on failure */
static void release_texture_bindless_index(Gpu *g, uint32_t index);
static void release_sampler_bindless_index(Gpu *g, uint32_t index);

/* Record that a command buffer this program will submit touches
 * [offset, offset+size) of `slot`. Every BufSlice a pass DECLARES goes through
 * here, which is what makes the rule at write time possible. */
static void commit_range(Gpu *g, uint32_t slot, uint64_t offset, uint64_t size) {
    BufferSlot *bs;
    uint64_t end;
    if (slot == 0u || slot >= SLOT_BUFFERS) return;
    bs = &g->buffers[slot];
    end = offset + size;
    if (end > bs->committed) bs->committed = end;
}

/* VUID-vkQueueSubmit-pCommandBuffers-00070: a device buffer that a command
 * buffer touches may only be destroyed after that command buffer completes.
 * Resources here are already retired by timeline value, so the only question
 * is whether a frame ever touched the buffer at all. A buffer created and
 * destroyed without one command buffer is free immediately; one that a frame
 * used waits for the timeline. */
static bool buffer_touched(const BufferSlot *bs) { return bs->committed != 0u; }

/* ---- error + handle helpers ----------------------------------------------*/
static void fail(Gpu *g, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g->error, sizeof g->error, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[mu_vk] %s\n", g->error);
}
static const char *result_name(VkResult r) {
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_UNKNOWN: return "VK_ERROR_UNKNOWN";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_NOT_READY: return "VK_NOT_READY";
    default: return "VkResult";
    }
}
#define VK_CALL(call, what)                                                       \
    do { VkResult r_ = (call); if (r_ != VK_SUCCESS) {                           \
        fail(g, "%s failed: %s (%d)", (what), result_name(r_), (int)r_);\
        return false; } } while (0)

static BufferSlot *buffer_slot(const Gpu *g, Buffer h) {
    return HANDLE_VALID(h) && h.index < SLOT_BUFFERS &&
           g->buffers[h.index].live && g->buffers[h.index].gen == h.gen
         ? (BufferSlot *)&g->buffers[h.index] : NULL;
}
static TextureSlot *texture_slot(const Gpu *g, Texture h) {
    return HANDLE_VALID(h) && h.index < SLOT_TEXTURES &&
           g->textures[h.index].live && g->textures[h.index].gen == h.gen
         ? (TextureSlot *)&g->textures[h.index] : NULL;
}
static TargetSlot *target_slot(const Gpu *g, Target h) {
    if (h.index == 0xFFFFFFFFu) {
        /* swapchain pseudo-target: the backend owns it, no generation */
        return (TargetSlot *)&g->targets[0];
    }
    return HANDLE_VALID(h) && h.index < MAX_TARGETS &&
           g->targets[h.index].live && g->targets[h.index].gen == h.gen
         ? (TargetSlot *)&g->targets[h.index] : NULL;
}
static SamplerSlot *sampler_slot(const Gpu *g, Sampler h) {
    return HANDLE_VALID(h) && h.index < SLOT_SAMPLERS &&
           g->samplers[h.index].live && g->samplers[h.index].gen == h.gen
         ? (SamplerSlot *)&g->samplers[h.index] : NULL;
}
static PipeSlot *pipe_slot(const Gpu *g, Pipe h) {
    return HANDLE_VALID(h) && h.index < MAX_PIPES &&
           g->pipes[h.index].live && g->pipes[h.index].gen == h.gen
         ? (PipeSlot *)&g->pipes[h.index] : NULL;
}

/* ---- vocabulary mapping (the ONLY place Vulkan spellings appear) ---------*/
static VkFormat to_vk_format(Format f) {
    switch (f) {
    case FMT_RGBA8_UNORM:  return VK_FORMAT_R8G8B8A8_UNORM;
    case FMT_RGBA8_SRGB:   return VK_FORMAT_R8G8B8A8_SRGB;
    case FMT_RGBA8_SNORM:  return VK_FORMAT_R8G8B8A8_SNORM;
    case FMT_BGRA8_UNORM:  return VK_FORMAT_B8G8R8A8_UNORM;
    case FMT_BGRA8_SRGB:   return VK_FORMAT_B8G8R8A8_SRGB;
    case FMT_RGBA16_UNORM: return VK_FORMAT_R16G16B16A16_UNORM;
    case FMT_RGBA16F:      return VK_FORMAT_R16G16B16A16_SFLOAT;
    case FMT_RGBA32F:      return VK_FORMAT_R32G32B32A32_SFLOAT;
    case FMT_RG16F:        return VK_FORMAT_R16G16_SFLOAT;
    case FMT_R32F:         return VK_FORMAT_R32_SFLOAT;
    case FMT_R16F:         return VK_FORMAT_R16_SFLOAT;
    case FMT_R8_UNORM:     return VK_FORMAT_R8_UNORM;
    case FMT_RG8_UNORM:    return VK_FORMAT_R8G8_UNORM;
    case FMT_RGB10A2_UNORM:return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case FMT_R11G11B10F:   return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case FMT_D16_UNORM:    return VK_FORMAT_D16_UNORM;
    case FMT_D24S8:        return VK_FORMAT_D24_UNORM_S8_UINT;
    case FMT_D32F:         return VK_FORMAT_D32_SFLOAT;
    case FMT_R32_UINT:     return VK_FORMAT_R32_UINT;
    case FMT_RGBA8_UINT:   return VK_FORMAT_R8G8B8A8_UINT;
    default:               return VK_FORMAT_UNDEFINED;
    }
}
static VkSampleCountFlagBits to_vk_samples(SampleCount s) {
    switch (s) { case SC_2: return VK_SAMPLE_COUNT_2_BIT;
                 case SC_4: return VK_SAMPLE_COUNT_4_BIT;
                 case SC_8: return VK_SAMPLE_COUNT_8_BIT;
                 default:   return VK_SAMPLE_COUNT_1_BIT; }
}
static VkFilter to_vk_filter(Filter f) {
    return f == FILT_NEAREST ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
}
static VkSamplerMipmapMode to_vk_mip_filter(Filter f) {
    return f == FILT_NEAREST ? VK_SAMPLER_MIPMAP_MODE_NEAREST
                             : VK_SAMPLER_MIPMAP_MODE_LINEAR;
}
static VkSamplerAddressMode to_vk_address(Address a) {
    switch (a) { case ADDR_CLAMP_EDGE: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                 case ADDR_CLAMP_BORDER: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
                 case ADDR_MIRROR: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
                 default: return VK_SAMPLER_ADDRESS_MODE_REPEAT; }
}
static VkCompareOp to_vk_compare(Compare c) {
    switch (c) {
    case CMP_NEVER:   return VK_COMPARE_OP_NEVER;
    case CMP_LESS:    return VK_COMPARE_OP_LESS;
    case CMP_EQUAL:   return VK_COMPARE_OP_EQUAL;
    case CMP_LEQUAL:  return VK_COMPARE_OP_LESS_OR_EQUAL;
    case CMP_GREATER: return VK_COMPARE_OP_GREATER;
    case CMP_GEQUAL:  return VK_COMPARE_OP_GREATER_OR_EQUAL;
    case CMP_ALWAYS:  return VK_COMPARE_OP_ALWAYS;
    default:          return VK_COMPARE_OP_LESS;
    }
}
static VkCullModeFlags to_vk_cull(Cull c) {
    switch (c) { case CULL_NONE: return VK_CULL_MODE_NONE;
                 case CULL_FRONT: return VK_CULL_MODE_FRONT_BIT;
                 default: return VK_CULL_MODE_BACK_BIT; }
}
static VkPrimitiveTopology to_vk_topo(Topo t) {
    switch (t) { case TOPO_TRI_STRIP: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
                 case TOPO_LINE: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
                 case TOPO_POINT: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
                 default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; }
}
static VkImageAspectFlags to_vk_aspect(Format f) {
    if (format_is_depth(f)) {
        VkImageAspectFlags a = VK_IMAGE_ASPECT_DEPTH_BIT;
        if (f == FMT_D24S8) a |= VK_IMAGE_ASPECT_STENCIL_BIT;
        return a;
    }
    return VK_IMAGE_ASPECT_COLOR_BIT;
}
static VkPipelineStageFlags to_vk_stage_for_kind(PassKind k) {
    return k == PASS_COMPUTE ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
           : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
}
/* SH_ALL is a mask of the bits above it, so it is not a separate case: the
 * old `if (s & SH_ALL) v |= ALL_GRAPHICS` matched for EVERY non-zero mask,
 * which turned every narrow read_stages into an all-commands dependency on
 * the source side — the widest possible barrier, from the one API that
 * exists to keep barriers narrow. ACC_TRANSFER had the same shape problem:
 * declaring a copy source added the write access bit too, so the src-side
 * barrier named a writer that was not there. */
static VkPipelineStageFlags to_vk_stage_bits(ShaderBits s) {
    VkPipelineStageFlags v = 0;
    if (s & SH_VERT) v |= VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
    if (s & SH_FRAG) v |= VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    if (s & SH_COMP) v |= VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    if (s & SH_TASK) v |= VK_PIPELINE_STAGE_TASK_SHADER_BIT_EXT;
    if (s & SH_MESH) v |= VK_PIPELINE_STAGE_MESH_SHADER_BIT_EXT;
    return v;
}
/* The consumer, not the caller, decides the stage for these: the command
 * processor reads indirect arguments. It is added to the src side as well, so
 * the pass that WRITES the arguments and the pass that indirectly draws are
 * ordered against each other instead of only against shader stages. */
static VkPipelineStageFlags to_vk_stage_extra(Access2 a) {
    return (a & ACC_INDIRECT) ? VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT : 0u;
}
static VkAccessFlags to_vk_access_bits(Access2 a, bool is_dst) {
    VkAccessFlags v = 0;
    if (a & ACC_READ)     v |= VK_ACCESS_SHADER_READ_BIT;
    if (a & ACC_WRITE)    v |= VK_ACCESS_SHADER_WRITE_BIT;
    if (a & ACC_ATOMIC)   v |= VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    if (a & ACC_INDIRECT) v |= VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
    if (a & ACC_VERTEX)   v |= VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
    if (a & ACC_INDEX)    v |= VK_ACCESS_INDEX_READ_BIT;
    if (a & ACC_UBO)      v |= VK_ACCESS_UNIFORM_READ_BIT;
    if (a & ACC_TRANSFER)
        v |= is_dst ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_TRANSFER_READ_BIT;
    return v;
}
/* ============================================================================
 * 2. DEVICE CREATION — the only place vkCreate* pipelines/queues appear
 * ==========================================================================*/

static uint32_t memory_type(const Gpu *g, uint32_t bits, VkMemoryPropertyFlags want) {
    uint32_t i;
    for (i = 0; i < g->mem_props.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (g->mem_props.memoryTypes[i].propertyFlags & want) == want)
            return i;
    }
    return UINT32_MAX;
}

static bool make_buffer(Gpu *g, uint64_t size, VkBufferUsageFlags usage,
                        VkMemoryPropertyFlags mem_props, Arena arena,
                        VkBuffer *out_buf, VkDeviceMemory *out_mem,
                        void **out_mapped, uint64_t *out_address) {
    VkBufferCreateInfo bci;
    VkMemoryRequirements req;
    VkMemoryAllocateInfo mai;
    VkMemoryAllocateFlagsInfo mafi;
    VkMemoryPropertyFlags relaxed;
    uint32_t type_index;
    VkResult r;

    memset(&bci, 0, sizeof bci);
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size  = (VkDeviceSize)size;
    bci.usage = usage | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (g->bda && !(mem_props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
        bci.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    r = vkCreateBuffer(g->device, &bci, NULL, out_buf);
    if (r != VK_SUCCESS) { fail(g, "vkCreateBuffer: %s", result_name(r)); return false; }

    vkGetBufferMemoryRequirements(g->device, *out_buf, &req);
    type_index = memory_type(g, req.memoryTypeBits, mem_props);
    relaxed = mem_props;
    if (type_index == UINT32_MAX && (mem_props & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
        relaxed &= ~VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        type_index = memory_type(g, req.memoryTypeBits, relaxed);
    }
    if (type_index == UINT32_MAX) {
        vkDestroyBuffer(g->device, *out_buf, NULL);
        fail(g, "no memory type for buffer (bits=%x)", (unsigned)req.memoryTypeBits);
        return false;
    }
    memset(&mai, 0, sizeof mai);
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type_index;
    /* A buffer created with SHADER_DEVICE_ADDRESS needs memory allocated with
     * the matching flag (VUID-vkBindBufferMemory-bufferDeviceAddress-03339). */
    memset(&mafi, 0, sizeof mafi);
    mafi.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    mafi.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    if (bci.usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) mai.pNext = &mafi;
    r = vkAllocateMemory(g->device, &mai, NULL, out_mem);
    if (r != VK_SUCCESS) {
        vkDestroyBuffer(g->device, *out_buf, NULL);
        fail(g, "vkAllocateMemory: %s", result_name(r)); return false;
    }
    r = vkBindBufferMemory(g->device, *out_buf, *out_mem, 0);
    if (r != VK_SUCCESS) {
        vkFreeMemory(g->device, *out_mem, NULL);
        vkDestroyBuffer(g->device, *out_buf, NULL);
        fail(g, "vkBindBufferMemory: %s", result_name(r)); return false;
    }
    if (mem_props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
        r = vkMapMemory(g->device, *out_mem, 0, VK_WHOLE_SIZE, 0, out_mapped);
        if (r != VK_SUCCESS) { fail(g, "vkMapMemory: %s", result_name(r)); return false; }
    } else if (out_mapped) {
        *out_mapped = NULL;
    }
    if (out_address) {
        *out_address = 0;
        if (g->bda && !(mem_props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
            VkBufferDeviceAddressInfo ai;
            memset(&ai, 0, sizeof ai);
            ai.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
            ai.buffer = *out_buf;
            *out_address = (uint64_t)vkGetBufferDeviceAddress(g->device, &ai);
        }
    }
    (void)arena;
    return true;
}

static bool make_image(Gpu *g, uint32_t w, uint32_t h, uint32_t d,
                       uint32_t layers, uint32_t mips, VkFormat fmt,
                       VkImageUsageFlags usage, VkSampleCountFlagBits samples,
                       VkImage *out_image, VkDeviceMemory *out_mem) {
    VkImageCreateInfo ici;
    VkMemoryRequirements req;
    VkMemoryAllocateInfo mai;
    uint32_t type_index;
    VkResult r;

    *out_image = VK_NULL_HANDLE;
    *out_mem = VK_NULL_HANDLE;
    memset(&ici, 0, sizeof ici);
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = d > 1u ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ici.format = fmt;
    ici.extent.width = w; ici.extent.height = h; ici.extent.depth = d;
    ici.mipLevels = mips; ici.arrayLayers = layers;
    ici.samples = samples;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    r = vkCreateImage(g->device, &ici, NULL, out_image);
    if (r != VK_SUCCESS) { fail(g, "vkCreateImage: %s", result_name(r)); return false; }
    vkGetImageMemoryRequirements(g->device, *out_image, &req);
    type_index = memory_type(g, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type_index == UINT32_MAX) {
        vkDestroyImage(g->device, *out_image, NULL);
        *out_image = VK_NULL_HANDLE;
        fail(g, "no device-local memory for image"); return false;
    }
    memset(&mai, 0, sizeof mai);
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type_index;
    r = vkAllocateMemory(g->device, &mai, NULL, out_mem);
    if (r != VK_SUCCESS) {
        vkDestroyImage(g->device, *out_image, NULL);
        *out_image = VK_NULL_HANDLE;
        fail(g, "vkAllocateMemory(image): %s", result_name(r)); return false;
    }
    r = vkBindImageMemory(g->device, *out_image, *out_mem, 0);
    if (r != VK_SUCCESS) {
        vkFreeMemory(g->device, *out_mem, NULL);
        vkDestroyImage(g->device, *out_image, NULL);
        *out_mem = VK_NULL_HANDLE;
        *out_image = VK_NULL_HANDLE;
        fail(g, "vkBindImageMemory: %s", result_name(r)); return false;
    }
    return true;
}

/* depth > 1 makes a 3D view; such an image has exactly one layer. */
static bool make_view(Gpu *g, VkImage image, VkFormat fmt,
                      VkImageAspectFlags aspect, uint32_t mips, uint32_t layers,
                      uint32_t depth, VkImageView *out) {
    VkImageViewCreateInfo vci;
    VkResult r;
    memset(&vci, 0, sizeof vci);
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = image;
    vci.viewType = depth > 1u ? VK_IMAGE_VIEW_TYPE_3D
                 : layers > 1u ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    vci.format = fmt;
    vci.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    vci.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    vci.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    vci.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
    vci.subresourceRange.aspectMask = aspect;
    vci.subresourceRange.baseMipLevel = 0;
    vci.subresourceRange.levelCount = mips;
    vci.subresourceRange.baseArrayLayer = 0;
    vci.subresourceRange.layerCount = layers;
    r = vkCreateImageView(g->device, &vci, NULL, out);
    if (r != VK_SUCCESS) { fail(g, "vkCreateImageView: %s", result_name(r)); return false; }
    return true;
}

static void queue_delete(Gpu *g, DeleteItem item) {
    /* If destruction happens during recording, this frame may still use the
     * resource. Retire no earlier than that frame's submit value. */
    item.retire_value = g->timeline_value + (g->frame_open ? 1u : 0u);
    if (g->delete_count < DELETE_QUEUE) {
        g->deletes[g->delete_count++] = item;
    } else {
        /* Preserve safety if a pathological burst exhausts the fixed queue.
         * The new delete is dropped (and reclaimed at shutdown), so extend the
         * oldest record rather than accidentally retiring it too early. */
        if (g->deletes[0].retire_value < item.retire_value)
            g->deletes[0].retire_value = item.retire_value;
    }
}

static bool init_bindless(Gpu *g) {
    VkDescriptorSetLayoutBinding binds[BIND_COUNT];
    VkDescriptorBindingFlags flags[BIND_COUNT];
    VkDescriptorSetLayoutBindingFlagsCreateInfo flags_ci;
    VkDescriptorSetLayoutCreateInfo lci;
    VkDescriptorPoolSize sizes[BIND_COUNT];
    VkDescriptorPoolCreateInfo pci;
    VkDescriptorSetAllocateInfo dai;
    VkPushConstantRange pcr;
    VkPipelineLayoutCreateInfo plci;
    VkResult r;
    uint32_t i;

    memset(binds, 0, sizeof binds);
    for (i = 0; i < BIND_COUNT; ++i) {
        binds[i].binding = i;
        binds[i].stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT;
    }
    binds[BIND_SAMPLED_IMAGES].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    binds[BIND_SAMPLED_IMAGES].descriptorCount = g->active_bindless;
    binds[BIND_STORAGE_IMAGES].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    binds[BIND_STORAGE_IMAGES].descriptorCount = g->active_bindless;
    binds[BIND_SAMPLERS].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    binds[BIND_SAMPLERS].descriptorCount = BINDLESS_SAMPLERS;
    binds[BIND_STORAGE_BUFFER].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    binds[BIND_STORAGE_BUFFER].descriptorCount = 1u;

    /* PARTIALLY_BOUND is the whole point: 4096 slots exist, only the ones a
     * resource has claimed are ever written. One set, written at create. */
    for (i = 0; i < BIND_COUNT; ++i)
        flags[i] = i == BIND_STORAGE_BUFFER ? 0u
                 : VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
    memset(&flags_ci, 0, sizeof flags_ci);
    flags_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    flags_ci.bindingCount = BIND_COUNT;
    flags_ci.pBindingFlags = flags;

    memset(&lci, 0, sizeof lci);
    lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    lci.pNext = &flags_ci;
    lci.bindingCount = BIND_COUNT;
    lci.pBindings = binds;
    r = vkCreateDescriptorSetLayout(g->device, &lci, NULL, &g->set_layout);
    if (r != VK_SUCCESS) { fail(g, "descriptor set layout: %s", result_name(r)); return false; }

    memset(sizes, 0, sizeof sizes);
    sizes[0].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;   sizes[0].descriptorCount = g->active_bindless;
    sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;   sizes[1].descriptorCount = g->active_bindless;
    sizes[2].type = VK_DESCRIPTOR_TYPE_SAMPLER;         sizes[2].descriptorCount = BINDLESS_SAMPLERS;
    sizes[3].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;  sizes[3].descriptorCount = 1u;
    memset(&pci, 0, sizeof pci);
    pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pci.maxSets = 1u;
    pci.poolSizeCount = BIND_COUNT;
    pci.pPoolSizes = sizes;
    pci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    r = vkCreateDescriptorPool(g->device, &pci, NULL, &g->desc_pool);
    if (r != VK_SUCCESS) { fail(g, "descriptor pool: %s", result_name(r)); return false; }

    memset(&dai, 0, sizeof dai);
    dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dai.descriptorPool = g->desc_pool;
    dai.descriptorSetCount = 1u;
    dai.pSetLayouts = &g->set_layout;
    r = vkAllocateDescriptorSets(g->device, &dai, &g->set);
    if (r != VK_SUCCESS) { fail(g, "descriptor set: %s", result_name(r)); return false; }

    /* One push range for caller-defined graphics/compute payloads. */
    memset(&pcr, 0, sizeof pcr);
    pcr.stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT;
    pcr.offset = 0;
    pcr.size = PUSH_MAX;
    memset(&plci, 0, sizeof plci);
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1u;
    plci.pSetLayouts = &g->set_layout;
    plci.pushConstantRangeCount = 1u;
    plci.pPushConstantRanges = &pcr;
    r = vkCreatePipelineLayout(g->device, &plci, NULL, &g->pipe_layout);
    if (r != VK_SUCCESS) { fail(g, "pipeline layout: %s", result_name(r)); return false; }

    {
        VkPipelineCacheCreateInfo cci;
        memset(&cci, 0, sizeof cci);
        cci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
        r = vkCreatePipelineCache(g->device, &cci, NULL, &g->pipe_cache);
        if (r != VK_SUCCESS) { fail(g, "pipeline cache: %s", result_name(r)); return false; }
    }
    return true;
}

static bool bind_storage_buffer(Gpu *g) {
    VkDescriptorBufferInfo dbi;
    VkWriteDescriptorSet w;
    BufferSlot *bs = &g->buffers[g->arenas[ARENA_DEVICE].handle.index];
    memset(&dbi, 0, sizeof dbi);
    dbi.buffer = bs->buffer;
    dbi.offset = 0;
    dbi.range = bs->size;
    memset(&w, 0, sizeof w);
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = g->set;
    w.dstBinding = BIND_STORAGE_BUFFER;
    w.dstArrayElement = 0;
    w.descriptorCount = 1u;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w.pBufferInfo = &dbi;
    vkUpdateDescriptorSets(g->device, 1u, &w, 0u, NULL);
    ++g->descriptor_writes_total;
    return true;
}

static bool init_arenas(Gpu *g) {
    /* TRANSFER_DST is required so upload_buffer()'s vkCmdUpdateBuffer fast
     * path may target an arena_alloc() slice (the bindless storage buffer). */
    VkBufferUsageFlags device_usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                      VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                      VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                      VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkMemoryPropertyFlags host_flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    ArenaState *dev = &g->arenas[ARENA_DEVICE];
    ArenaState *trans = &g->arenas[ARENA_TRANSIENT];
    uint32_t i;

    /* Slot 1 = device arena, slot 2 = transient arena. Both are real VkBuffers
     * so arena_alloc() can return an honest BufSlice for them. */
    g->buffers[1].live = true; g->buffers[1].gen = 1u;
    g->buffers[1].cls = RES_PERSISTENT; g->buffers[1].arena = ARENA_DEVICE;
    g->buffers[1].size = ARENA_DEVICE_BYTES;
    strcpy(g->buffers[1].name, "arena_device");
    if (!make_buffer(g, ARENA_DEVICE_BYTES, device_usage,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, ARENA_DEVICE,
                     &dev->buffer, &dev->memory, &dev->mapped,
                     &g->buffers[1].device_address)) return false;
    g->buffers[1].buffer = dev->buffer;
    g->buffers[1].memory = dev->memory;
    dev->handle = (Buffer){1u, 1u};
    dev->capacity = ARENA_DEVICE_BYTES;

    g->buffers[2].live = true; g->buffers[2].gen = 1u;
    g->buffers[2].cls = RES_FRAME; g->buffers[2].arena = ARENA_TRANSIENT;
    g->buffers[2].size = ARENA_TRANSIENT_BYTES;
    strcpy(g->buffers[2].name, "arena_transient");
    if (!make_buffer(g, ARENA_TRANSIENT_BYTES, device_usage,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, ARENA_TRANSIENT,
                     &trans->buffer, &trans->memory, &trans->mapped,
                     &g->buffers[2].device_address)) return false;
    g->buffers[2].buffer = trans->buffer;
    g->buffers[2].memory = trans->memory;
    trans->handle = (Buffer){2u, 1u};
    trans->capacity = ARENA_TRANSIENT_BYTES;

    /* Staging: fixed chunks, each persistently mapped and retired by timeline
     * value. Slots 3..3+N-1 are the handles returned by arena_alloc(). */
    for (i = 0; i < STAGING_CHUNKS; ++i) {
        StagingChunk *c = &g->staging[i];
        BufferSlot   *s = &g->buffers[3u + i];
        if (!make_buffer(g, STAGING_CHUNK_BYTES,
                         VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         host_flags, ARENA_STAGING,
                         &c->buffer, &c->memory, &c->mapped, NULL)) return false;
        s->live = true; s->gen = 1u;
        s->buffer = c->buffer; s->memory = c->memory;
        s->size = STAGING_CHUNK_BYTES;
        s->host_visible = true; s->mapped = c->mapped;
        s->arena = ARENA_STAGING; s->cls = RES_FRAME;
        s->usage = BUF_XFER_SRC;
        snprintf(s->name, sizeof s->name, "staging_%u", i);
    }
    return true;
}

static bool init_headless_target(Gpu *g) {
    TextureSlot *t = &g->textures[0];
    TargetSlot  *s = &g->targets[0];
    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                              VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT;
    VkFormat fmt = g->swapchain_srgb ? VK_FORMAT_B8G8R8A8_SRGB
                                     : VK_FORMAT_B8G8R8A8_UNORM;
    /* No implementation supports storage access to an sRGB format. */
    const bool storage = !g->swapchain_srgb;
    if (storage) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    t->live = true; t->gen = 1u;
    t->width = g->extent_w; t->height = g->extent_h;
    t->depth = 1u; t->layers = 1u; t->mips = 1u;
    t->format = g->swapchain_format;
    t->usage = TEX_SAMPLED | TEX_COLOR | TEX_XFER_SRC | TEX_XFER_DST;
    if (storage) t->usage |= TEX_STORAGE;
    t->cls = RES_PERSISTENT;
    t->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    strcpy(t->name, "swapchain");
    if (!make_image(g, g->extent_w, g->extent_h, 1u, 1u, 1u, fmt, usage,
                    VK_SAMPLE_COUNT_1_BIT, &t->image, &t->memory)) return false;
    if (!make_view(g, t->image, fmt, VK_IMAGE_ASPECT_COLOR_BIT, 1u, 1u, 1u, &t->view))
        return false;
    if (!make_view(g, t->image, fmt, VK_IMAGE_ASPECT_COLOR_BIT, 1u, 1u, 1u, &t->storage_view))
        return false;
    s->live = true; s->gen = 0u; s->tex = t;
    s->width = g->extent_w; s->height = g->extent_h;
    s->format = g->swapchain_format;
    s->usage = TGT_COLOR | TGT_SAMPLED;
    if (storage) s->usage |= TGT_STORAGE;
    s->samples = SC_1; s->cls = RES_PERSISTENT;
    s->is_swapchain = true;
    strcpy(s->name, "swapchain");
    return true;
}

static bool init_frames(Gpu *g) {
    VkCommandPoolCreateInfo cpci;
    VkSemaphoreTypeCreateInfo stci;
    VkSemaphoreCreateInfo sci;
    VkQueryPoolCreateInfo qci;
    VkCommandBufferAllocateInfo cai;
    VkResult r;
    uint32_t i;

    memset(&cpci, 0, sizeof cpci);
    cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = g->queue_family;
    r = vkCreateCommandPool(g->device, &cpci, NULL, &g->cmd_pool);
    if (r != VK_SUCCESS) { fail(g, "command pool: %s", result_name(r)); return false; }

    memset(&stci, 0, sizeof stci);
    stci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    stci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    stci.initialValue = 0u;
    memset(&sci, 0, sizeof sci);
    sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    sci.pNext = &stci;
    r = vkCreateSemaphore(g->device, &sci, NULL, &g->timeline);
    if (r != VK_SUCCESS) { fail(g, "timeline semaphore: %s", result_name(r)); return false; }

    for (i = 0; i < g->frames_in_flight; ++i) {
        FrameSlot *f = &g->frames[i];
        memset(&cai, 0, sizeof cai);
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = g->cmd_pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1u;
        r = vkAllocateCommandBuffers(g->device, &cai, &f->cmd);
        if (r != VK_SUCCESS) { fail(g, "command buffer: %s", result_name(r)); return false; }
        memset(&qci, 0, sizeof qci);
        qci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        /* Two timestamps per pass plus two for each optional named scope. */
        qci.queryCount = 2u * (MAX_PASSES + MAX_PROFILE_SCOPES);
        r = vkCreateQueryPool(g->device, &qci, NULL, &f->queries);
        if (r != VK_SUCCESS) { fail(g, "query pool: %s", result_name(r)); return false; }
        memset(&sci, 0, sizeof sci);
        sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        r = vkCreateSemaphore(g->device, &sci, NULL, &f->present_done);
        if (r != VK_SUCCESS) { fail(g, "present semaphore: %s", result_name(r)); return false; }
        r = vkCreateSemaphore(g->device, &sci, NULL, &f->acquire_done);
        if (r != VK_SUCCESS) { fail(g, "acquire semaphore: %s", result_name(r)); return false; }
    }
    return true;
}

Gpu *gpu_create(const GpuDesc *desc) {
    Gpu *g;
    VkApplicationInfo ai;
    VkInstanceCreateInfo ici;
    VkDeviceQueueCreateInfo dqci;
    VkDeviceCreateInfo dci;
    VkPhysicalDeviceFeatures2 f2, f2q;
    VkPhysicalDeviceVulkan11Features v11, v11q;
    VkPhysicalDeviceVulkan12Features v12, v12q;
    VkPhysicalDeviceVulkan13Features v13, v13q;
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT e3, e3q;
    VkPhysicalDeviceProperties2 p2;
    VkResult r;
    uint32_t dev_count = 0, qf_count = 0, ext_count = 0, layer_count = 0;
    uint32_t i, j;
    float queue_priority = 1.0f;
    const char *dev_ext[8];
    uint32_t n_dev_ext = 0;
    const char *inst_ext[8];
    uint32_t n_inst_ext = 0;
    const char *layers[2];
    uint32_t n_layers = 0;
    VkQueueFamilyProperties qf[16];
    VkPhysicalDevice pdev = VK_NULL_HANDLE;
    VkExtensionProperties *inst_exts = NULL;
    VkLayerProperties *layer_props = NULL;

    g = (Gpu *)calloc(1u, sizeof *g);
    if (!g) { fprintf(stderr, "[mu_vk] gpu_create: out of host memory\n"); return NULL; }
    strcpy(g->error, "no error");
    if (!s_volk_initialized) {
        r = volkInitialize();
        if (r != VK_SUCCESS) {
            fail(g, "volkInitialize: %s", result_name(r));
            gpu_destroy(g);
            return NULL;
        }
        s_volk_initialized = true;
    }
    g->extent_w = desc->width ? desc->width : 1280u;
    g->extent_h = desc->height ? desc->height : 720u;
    g->swapchain_format = FMT_BGRA8_SRGB;
    g->swapchain_srgb = true;
    g->vsync = desc->vsync;
    g->validation = desc->validation;
    g->enable_escape = desc->enable_escape;
    g->frames_in_flight = desc->frames_in_flight
                        ? min_u32(desc->frames_in_flight, MAX_FRAMES_IN_FLIGHT)
                        : MAX_FRAMES_IN_FLIGHT;
    g->active_bindless = ACTIVE_BINDLESS;

    /* ---- instance: enable only extensions the loader actually advertises -- */
    vkEnumerateInstanceExtensionProperties(NULL, &ext_count, NULL);
    if (ext_count) {
        inst_exts = (VkExtensionProperties *)calloc(ext_count, sizeof *inst_exts);
        vkEnumerateInstanceExtensionProperties(NULL, &ext_count, inst_exts);
    }
    {
        const char *want[] = { "VK_KHR_surface", "VK_KHR_wayland_surface",
                              "VK_KHR_xcb_surface", "VK_KHR_xlib_surface",
                              "VK_EXT_debug_utils" };
        for (i = 0; i < sizeof want / sizeof *want; ++i) {
            for (j = 0; j < ext_count; ++j) {
                if (!strcmp(inst_exts[j].extensionName, want[i]) &&
                    n_inst_ext < sizeof inst_ext / sizeof *inst_ext)
                    inst_ext[n_inst_ext++] = want[i];
            }
        }
    }
    free(inst_exts);
    if (desc->validation) {
        vkEnumerateInstanceLayerProperties(&layer_count, NULL);
        if (layer_count) {
            layer_props = (VkLayerProperties *)calloc(layer_count, sizeof *layer_props);
            vkEnumerateInstanceLayerProperties(&layer_count, layer_props);
            for (i = 0; i < layer_count; ++i) {
                if (!strcmp(layer_props[i].layerName, "VK_LAYER_KHRONOS_validation"))
                    layers[n_layers++] = "VK_LAYER_KHRONOS_validation";
            }
            free(layer_props);
            if (n_layers == 0)
                snprintf(g->validation_note, sizeof g->validation_note,
                         "validation requested but VK_LAYER_KHRONOS_validation is not installed");
        }
    }

    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.pApplicationName = desc->app_name ? desc->app_name : "mu_gfx";
    ai.applicationVersion = 1u;
    ai.pEngineName = "mu_gfx";
    ai.engineVersion = 1u;
    ai.apiVersion = VK_API_VERSION_1_3;
    memset(&ici, 0, sizeof ici);
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &ai;
    ici.enabledExtensionCount = n_inst_ext;
    ici.ppEnabledExtensionNames = inst_ext;
    ici.enabledLayerCount = n_layers;
    ici.ppEnabledLayerNames = layers;
    r = vkCreateInstance(&ici, NULL, &g->instance);
    if (r != VK_SUCCESS) { fail(g, "vkCreateInstance: %s", result_name(r)); gpu_destroy(g); return NULL; }
    volkLoadInstance(g->instance);

    r = vkEnumeratePhysicalDevices(g->instance, &dev_count, NULL);
    if (r != VK_SUCCESS || dev_count == 0) {
        fail(g, "no Vulkan physical device (%s)", result_name(r));
        gpu_destroy(g); return NULL;
    }
    {
        VkPhysicalDevice *all = (VkPhysicalDevice *)calloc(dev_count, sizeof *all);
        if (!all) { fail(g, "out of host memory"); gpu_destroy(g); return NULL; }
        r = vkEnumeratePhysicalDevices(g->instance, &dev_count, all);
        if (r != VK_SUCCESS || dev_count == 0u) {
            free(all);
            fail(g, "vkEnumeratePhysicalDevices: %s", result_name(r));
            gpu_destroy(g); return NULL;
        }
        /* Prefer a discrete GPU, else take the first device. */
        pdev = all[0];
        for (i = 0; i < dev_count; ++i) {
            VkPhysicalDeviceProperties p;
            vkGetPhysicalDeviceProperties(all[i], &p);
            if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { pdev = all[i]; break; }
        }
        free(all);
    }
    g->pdev = pdev;
    vkGetPhysicalDeviceProperties(pdev, &g->props);
    vkGetPhysicalDeviceMemoryProperties(pdev, &g->mem_props);

    /* ---- feature query, then enable exactly the features this backend uses - */
#define FEATQ(name, stype)                                                       \
    memset(&name, 0, sizeof name); name.sType = stype
    FEATQ(f2q, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
    FEATQ(v11q, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES);
    FEATQ(v12q, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
    FEATQ(v13q, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES);
    FEATQ(e3q, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT);
    f2q.pNext = &v11q; v11q.pNext = &v12q; v12q.pNext = &v13q;
    v13q.pNext = &e3q;
    vkGetPhysicalDeviceFeatures2(pdev, &f2q);
    memset(&p2, 0, sizeof p2);
    p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    memset(&g->subgroup, 0, sizeof g->subgroup);
    g->subgroup.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES;
    p2.pNext = &g->subgroup;
    vkGetPhysicalDeviceProperties2(pdev, &p2);
#undef FEATQ

#define FEATE(name, stype)                                                       \
    memset(&name, 0, sizeof name); name.sType = stype
    FEATE(f2, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
    FEATE(v11, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES);
    FEATE(v12, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
    FEATE(v13, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES);
    FEATE(e3, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT);
    f2.features.samplerAnisotropy = f2q.features.samplerAnisotropy;
    f2.features.shaderStorageImageWriteWithoutFormat =
        f2q.features.shaderStorageImageWriteWithoutFormat;
    v11.shaderDrawParameters = v11q.shaderDrawParameters;   /* Slang SV_VertexID */
    v11.storageBuffer16BitAccess = v11q.storageBuffer16BitAccess;
    v11.uniformAndStorageBuffer16BitAccess = v11q.uniformAndStorageBuffer16BitAccess;
    v11.storagePushConstant16 = v11q.storagePushConstant16;
    v11.storageInputOutput16 = v11q.storageInputOutput16;
    v12.timelineSemaphore = v12q.timelineSemaphore;
    v12.bufferDeviceAddress = v12q.bufferDeviceAddress;
    v12.descriptorBindingPartiallyBound = v12q.descriptorBindingPartiallyBound;
    v12.descriptorBindingVariableDescriptorCount = v12q.descriptorBindingVariableDescriptorCount;
    v12.runtimeDescriptorArray = v12q.runtimeDescriptorArray;
    v12.shaderSampledImageArrayNonUniformIndexing = v12q.shaderSampledImageArrayNonUniformIndexing;
    v12.shaderStorageImageArrayNonUniformIndexing = v12q.shaderStorageImageArrayNonUniformIndexing;
    v13.dynamicRendering = v13q.dynamicRendering;
    /* shaderFloat16 lives in the 1.2 struct; chaining the separate
     * Float16Int8 struct next to it is invalid (VUID-VkDeviceCreateInfo-pNext-02830). */
    v12.shaderFloat16 = v12q.shaderFloat16;
    e3.extendedDynamicState3RasterizationSamples = e3q.extendedDynamicState3RasterizationSamples;
    e3.extendedDynamicState3ColorBlendEnable = e3q.extendedDynamicState3ColorBlendEnable;
    e3.extendedDynamicState3ColorBlendEquation = e3q.extendedDynamicState3ColorBlendEquation;
    e3.extendedDynamicState3ColorWriteMask = e3q.extendedDynamicState3ColorWriteMask;
    f2.pNext = &v11; v11.pNext = &v12; v12.pNext = &v13;
    v13.pNext = &e3;
#undef FEATE

    g->bda = v12q.bufferDeviceAddress == VK_TRUE;
    g->shader_float16 = v12q.shaderFloat16 == VK_TRUE;
    g->storage_16bit = v11q.storageBuffer16BitAccess == VK_TRUE;
    g->push16 = v11q.storagePushConstant16 == VK_TRUE;
    g->io16 = v11q.storageInputOutput16 == VK_TRUE;
    g->eds3 = e3q.extendedDynamicState3RasterizationSamples == VK_TRUE &&
              e3q.extendedDynamicState3ColorBlendEnable == VK_TRUE &&
              e3q.extendedDynamicState3ColorBlendEquation == VK_TRUE &&
              e3q.extendedDynamicState3ColorWriteMask == VK_TRUE;
    for (i = 0; i < g->mem_props.memoryHeapCount; ++i) {
        VkMemoryHeap *h = &g->mem_props.memoryHeaps[i];
        if (h->flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            g->device_memory_bytes += h->size;
            for (j = 0; j < g->mem_props.memoryTypeCount; ++j) {
                VkMemoryType *mt = &g->mem_props.memoryTypes[j];
                if (mt->heapIndex == i &&
                    (mt->propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
                    g->visible_vram_bytes += h->size;
                    g->host_visible_device_local = true;
                    break;
                }
            }
        }
    }

    /* ---- device extensions ---------------------------------------------- */
    vkEnumerateDeviceExtensionProperties(pdev, NULL, &ext_count, NULL);
    if (ext_count) {
        VkExtensionProperties *de = (VkExtensionProperties *)calloc(ext_count, sizeof *de);
        vkEnumerateDeviceExtensionProperties(pdev, NULL, &ext_count, de);
        for (i = 0; i < ext_count; ++i) {
            if (!strcmp(de[i].extensionName, "VK_KHR_swapchain"))
                dev_ext[n_dev_ext++] = "VK_KHR_swapchain";
            if (g->eds3 && !strcmp(de[i].extensionName, "VK_EXT_extended_dynamic_state3"))
                dev_ext[n_dev_ext++] = "VK_EXT_extended_dynamic_state3";
        }
        free(de);
    }
    if (g->eds3) {
        bool found = false;
        for (i = 0; i < n_dev_ext; ++i)
            if (!strcmp(dev_ext[i], "VK_EXT_extended_dynamic_state3")) found = true;
        if (!found) g->eds3 = false;   /* extension absent: static fallback */
    }

    vkGetPhysicalDeviceQueueFamilyProperties(pdev, &qf_count, NULL);
    if (qf_count > sizeof qf / sizeof *qf) qf_count = sizeof qf / sizeof *qf;
    vkGetPhysicalDeviceQueueFamilyProperties(pdev, &qf_count, qf);
    g->queue_family = UINT32_MAX;
    for (i = 0; i < qf_count; ++i) {
        if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
            (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) { g->queue_family = i; break; }
    }
    if (g->queue_family == UINT32_MAX) {
        fail(g, "no graphics+compute queue family");
        gpu_destroy(g); return NULL;
    }

    memset(&dqci, 0, sizeof dqci);
    dqci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    dqci.queueFamilyIndex = g->queue_family;
    dqci.queueCount = 1u;
    dqci.pQueuePriorities = &queue_priority;
    memset(&dci, 0, sizeof dci);
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext = &f2;
    dci.queueCreateInfoCount = 1u;
    dci.pQueueCreateInfos = &dqci;
    dci.enabledExtensionCount = n_dev_ext;
    dci.ppEnabledExtensionNames = dev_ext;
    r = vkCreateDevice(pdev, &dci, NULL, &g->device);
    if (r != VK_SUCCESS) {
        fail(g, "vkCreateDevice: %s", result_name(r));
        gpu_destroy(g); return NULL;
    }
    volkLoadDevice(g->device);
    vkGetDeviceQueue(g->device, g->queue_family, 0u, &g->queue);

    if (g->eds3 && (!vkCmdSetRasterizationSamplesEXT ||
                    !vkCmdSetColorBlendEnableEXT ||
                    !vkCmdSetColorBlendEquationEXT ||
                    !vkCmdSetColorWriteMaskEXT))
        g->eds3 = false;

    g->active_bindless = min_u32(g->active_bindless,
        min_u32(g->props.limits.maxDescriptorSetSampledImages,
                g->props.limits.maxPerStageDescriptorSampledImages));
    if (g->active_bindless < 64u) g->active_bindless = 64u;
    mu_id_pool_init(&g->texture_bindless_ids, g->active_bindless);
    mu_id_pool_init(&g->sampler_bindless_ids, BINDLESS_SAMPLERS);

    if (!init_bindless(g) || !init_arenas(g) ||
        !bind_storage_buffer(g) || !init_frames(g) ||
        !init_headless_target(g)) {
        gpu_destroy(g);
        return NULL;
    }
    if (g->validation_note[0])
        fprintf(stderr, "[mu_vk] note: %s\n", g->validation_note);
    return g;
}

void gpu_destroy(Gpu *g) {
    uint32_t i;
    if (!g) return;
    /* Destruction is allowed to wait: it is shutdown, not a frame. */
    /* Shutdown waits on outstanding timeline work before releasing resources. */
    if (g->device && g->timeline && g->timeline_value > 0u) {
        VkSemaphoreWaitInfo wi;
        memset(&wi, 0, sizeof wi);
        wi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        wi.semaphoreCount = 1u;
        wi.pSemaphores = &g->timeline;
        wi.pValues = &g->timeline_value;
        vkWaitSemaphores(g->device, &wi, 10000000000ull);
    }
    /* gpu_create() unwinds through here, so any handle may still be null:
     * Vulkan takes VK_NULL_HANDLE for an object destroy and ignores it, but
     * the volk entry points are only loaded once an instance/device exists. */
    if (!g->instance) { free(g); return; }
    if (g->device) vkDeviceWaitIdle(g->device);
    if (!g->device) {
        if (g->surface) vkDestroySurfaceKHR(g->instance, g->surface, NULL);
        vkDestroyInstance(g->instance, NULL);
        free(g);
        return;
    }
    for (i = 0; i < g->frames_in_flight; ++i) {
        if (g->frames[i].queries) vkDestroyQueryPool(g->device, g->frames[i].queries, NULL);
        if (g->frames[i].present_done) vkDestroySemaphore(g->device, g->frames[i].present_done, NULL);
        if (g->frames[i].acquire_done) vkDestroySemaphore(g->device, g->frames[i].acquire_done, NULL);
    }
    /* Apply deferred deletes first (device is idle), then destroy what is
     * still live. Staging chunk buffers live in the buffer table (slots 3..),
     * so they are destroyed exactly once below. */
    for (i = 0; i < g->delete_count; ++i) delete_apply(g, &g->deletes[i]);
    g->delete_count = 0u;
    for (i = 0; i < MAX_PIPES; ++i)
        if (g->pipes[i].pipeline) vkDestroyPipeline(g->device, g->pipes[i].pipeline, NULL);
    for (i = 0; i < SLOT_SHADERS; ++i)
        if (g->shaders[i].module) vkDestroyShaderModule(g->device, g->shaders[i].module, NULL);
    for (i = 0; i < SLOT_BUFFERS; ++i) {
        BufferSlot *b = &g->buffers[i];
        if (b->buffer) vkDestroyBuffer(g->device, b->buffer, NULL);
        if (b->memory) vkFreeMemory(g->device, b->memory, NULL);
    }
    for (i = 0; i < SLOT_TEXTURES; ++i) {
        TextureSlot *t = &g->textures[i];
        if (i != 0u && t->live) release_texture_bindless_index(g, t->bindless_index);
        /* Slot 0 borrows a swapchain-owned view once a surface is attached, and
         * frame_begin aliases view and storage_view onto the same handle. Those
         * views are released exactly once below, before the swapchain goes. */
        if (i == 0u && g->swapchain) continue;
        if (t->view) vkDestroyImageView(g->device, t->view, NULL);
        if (t->storage_view) vkDestroyImageView(g->device, t->storage_view, NULL);
        if (t->image) vkDestroyImage(g->device, t->image, NULL);
        if (t->memory) vkFreeMemory(g->device, t->memory, NULL);
    }
    for (i = 0; i < SLOT_SAMPLERS; ++i) {
        if (i != 0u && g->samplers[i].live)
            release_sampler_bindless_index(g, g->samplers[i].bindless_index);
        if (g->samplers[i].sampler) vkDestroySampler(g->device, g->samplers[i].sampler, NULL);
    }
    /* A queue-overflow fallback may have dropped a cold delete record. At
     * shutdown, discard any remaining IDs only after all GPU objects are gone. */
    if (g->texture_bindless_ids.ranges) {
        uint32_t id;
        for (id = 0; id <= g->texture_bindless_ids.max_id; ++id)
            if (mu_id_pool_is_id(&g->texture_bindless_ids, id))
                (void)mu_id_pool_destroy_id(&g->texture_bindless_ids, id);
        mu_id_pool_deinit(&g->texture_bindless_ids);
    }
    if (g->sampler_bindless_ids.ranges) {
        uint32_t id;
        for (id = 0; id <= g->sampler_bindless_ids.max_id; ++id)
            if (mu_id_pool_is_id(&g->sampler_bindless_ids, id))
                (void)mu_id_pool_destroy_id(&g->sampler_bindless_ids, id);
        mu_id_pool_deinit(&g->sampler_bindless_ids);
    }
    for (i = 0; i < g->swapchain_image_count; ++i)
        if (g->swapchain_views[i])
            vkDestroyImageView(g->device, g->swapchain_views[i], NULL);
    if (g->swapchain) vkDestroySwapchainKHR(g->device, g->swapchain, NULL);
    if (g->pipe_cache) vkDestroyPipelineCache(g->device, g->pipe_cache, NULL);
    if (g->pipe_layout) vkDestroyPipelineLayout(g->device, g->pipe_layout, NULL);
    if (g->desc_pool) vkDestroyDescriptorPool(g->device, g->desc_pool, NULL);
    if (g->set_layout) vkDestroyDescriptorSetLayout(g->device, g->set_layout, NULL);
    if (g->cmd_pool) vkDestroyCommandPool(g->device, g->cmd_pool, NULL);
    if (g->timeline) vkDestroySemaphore(g->device, g->timeline, NULL);
    if (g->surface) vkDestroySurfaceKHR(g->instance, g->surface, NULL);
    if (g->device) vkDestroyDevice(g->device, NULL);
    if (g->instance) vkDestroyInstance(g->instance, NULL);
    free(g);
}

void gpu_info(const Gpu *g, GpuInfo *out) {
    memset(out, 0, sizeof *out);
    out->extent_w = g->extent_w;
    out->extent_h = g->extent_h;
    out->swapchain_format = g->swapchain_format;
    out->swapchain_srgb = g->swapchain_srgb;
    out->max_samples = SC_8;
    out->device_memory = g->device_memory_bytes;
    out->frames_in_flight = g->frames_in_flight;
    copy_name(out->device_name, sizeof out->device_name, g->props.deviceName);
    out->subgroup_size = g->subgroup.subgroupSize;
    out->max_compute_workgroup_invocations = g->props.limits.maxComputeWorkGroupInvocations;
    out->max_compute_shared_memory_size = (uint32_t)g->props.limits.maxComputeSharedMemorySize;
    out->non_coherent_atom_size = (uint32_t)g->props.limits.nonCoherentAtomSize;
    out->min_storage_buffer_offset_alignment =
        (uint32_t)g->props.limits.minStorageBufferOffsetAlignment;
    out->max_uniform_buffer_range = g->props.limits.maxUniformBufferRange;
    out->max_storage_buffer_range = g->props.limits.maxStorageBufferRange;
    out->visible_device_memory = g->visible_vram_bytes;
    out->shader_float16 = g->shader_float16;
    out->storage_buffer_16bit = g->storage_16bit;
    out->storage_push_constant16 = g->push16;
    out->storage_input_output16 = g->io16;
    out->host_visible_device_local = g->host_visible_device_local;
}

/* ============================================================================
 * 3. RESOURCES — created outside frames; descriptors written exactly here
 * ==========================================================================*/

static void count_descriptor_write(Gpu *g) {
    ++g->descriptor_writes_total;
    if (g->frame_open) ++g->active_stats.descriptor_writes;
}

static void write_image(Gpu *g, uint32_t binding, uint32_t index,
                        VkImageView view, VkImageLayout layout) {
    VkDescriptorImageInfo dii;
    VkWriteDescriptorSet w;
    memset(&dii, 0, sizeof dii);
    dii.imageView = view;
    dii.imageLayout = layout;
    memset(&w, 0, sizeof w);
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = g->set;
    w.dstBinding = binding;
    w.dstArrayElement = index;
    w.descriptorCount = 1u;
    w.descriptorType = binding == BIND_SAMPLERS ? VK_DESCRIPTOR_TYPE_SAMPLER
                     : binding == BIND_STORAGE_IMAGES ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                     : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w.pImageInfo = &dii;
    vkUpdateDescriptorSets(g->device, 1u, &w, 0u, NULL);
    count_descriptor_write(g);
}

static uint32_t new_buffer_slot(Gpu *g) {
    uint32_t i;
    for (i = 3u + STAGING_CHUNKS; i < SLOT_BUFFERS; ++i)
        if (!g->buffers[i].live) return i;
    return 0u;
}
static uint32_t new_texture_slot(Gpu *g) {
    uint32_t i;
    for (i = 1u; i < SLOT_TEXTURES; ++i)
        if (!g->textures[i].live) return i;
    return 0u;
}
static uint32_t new_target_slot(Gpu *g) {
    uint32_t i;
    for (i = 1u; i < MAX_TARGETS; ++i)
        if (!g->targets[i].live) return i;
    return 0u;
}
static uint32_t new_sampler_slot(Gpu *g) {
    uint32_t i;
    for (i = 1u; i < SLOT_SAMPLERS; ++i)
        if (!g->samplers[i].live) return i;
    return 0u;
}

static VkBufferUsageFlags to_vk_buffer_usage(uint32_t usage) {
    VkBufferUsageFlags v = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (usage & BUF_INDIRECT) v |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    if (usage & BUF_VERTEX)   v |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (usage & BUF_INDEX)    v |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (usage & BUF_XFER_SRC) v |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (usage & BUF_XFER_DST) v |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    return v;
}
static VkImageUsageFlags to_vk_image_usage(uint32_t usage, bool depth) {
    VkImageUsageFlags v = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (usage & TEX_SAMPLED) v |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (usage & TEX_STORAGE) v |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (usage & TEX_COLOR)   v |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (usage & TEX_DEPTH)   v |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (depth)               v |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    return v;
}

Buffer buffer_create(Gpu *g, const BufferDesc *desc) {
    BufferSlot *s;
    uint32_t idx;
    uint64_t align;
    VkMemoryPropertyFlags mem_props;

    if (!desc->size) { fail(g, "buffer_create: zero size"); return BUFFER_NIL; }
    idx = new_buffer_slot(g);
    if (!idx) { fail(g, "buffer_create: buffer table full"); return BUFFER_NIL; }
    s = &g->buffers[idx];
    { uint32_t old_gen = s->gen; memset(s, 0, sizeof *s); s->gen = old_gen ? old_gen : 1u; }
    align = align_up(64u, g->props.limits.minStorageBufferOffsetAlignment);
    s->size = align_up(desc->size, align);
    s->align = align;
    s->usage = desc->usage;
    s->cls = desc->cls;
    s->arena = desc->cls == RES_FRAME ? ARENA_TRANSIENT : ARENA_DEVICE;
    copy_name(s->name, sizeof s->name, desc->name ? desc->name : "buffer");
    mem_props = desc->cls == RES_READBACK
              ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
              : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    s->host_visible = (mem_props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
    if (!make_buffer(g, s->size, to_vk_buffer_usage(desc->usage), mem_props,
                     s->arena, &s->buffer, &s->memory,
                     s->host_visible ? &s->mapped : NULL, &s->device_address)) {
        /* Give the slot back: a failed create must leave nothing claimed, or
         * the table fills up with tombstones over a session. */
        ++s->gen;
        memset(s, 0, sizeof *s);
        s->gen = g->buffers[idx].gen;   /* keep the bumped generation */
        return BUFFER_NIL;
    }
    s->live = true;
    return (Buffer){idx, s->gen};
}

/* Descriptor index 0 is valid. Resource-table slot 0 is reserved for the
 * swapchain pseudo-texture, but bindless descriptor pools may allocate index 0.
 * A descriptor index is returned only when its resource is safely retired. */
static bool alloc_texture_bindless_index(Gpu *g, uint32_t *out_index) {
    if (!mu_id_pool_create_id(&g->texture_bindless_ids, out_index)) {
        fail(g, "texture_create: bindless window full");
        return false;
    }
    return true;
}

static void release_texture_bindless_index(Gpu *g, uint32_t index) {
    if (!mu_id_pool_destroy_id(&g->texture_bindless_ids, index))
        fail(g, "texture bindless index %u was not allocated", index);
}

static bool alloc_sampler_bindless_index(Gpu *g, uint32_t *out_index) {
    if (!mu_id_pool_create_id(&g->sampler_bindless_ids, out_index)) {
        fail(g, "sampler_create: bindless sampler table full");
        return false;
    }
    return true;
}

static void release_sampler_bindless_index(Gpu *g, uint32_t index) {
    if (!mu_id_pool_destroy_id(&g->sampler_bindless_ids, index))
        fail(g, "sampler bindless index %u was not allocated", index);
}

static void abort_texture_init(Gpu *g, TextureSlot *t, uint32_t index) {
    if (t->storage_view) vkDestroyImageView(g->device, t->storage_view, NULL);
    if (t->view) vkDestroyImageView(g->device, t->view, NULL);
    if (t->image) vkDestroyImage(g->device, t->image, NULL);
    if (t->memory) vkFreeMemory(g->device, t->memory, NULL);
    t->storage_view = VK_NULL_HANDLE;
    t->view = VK_NULL_HANDLE;
    t->image = VK_NULL_HANDLE;
    t->memory = VK_NULL_HANDLE;
    release_texture_bindless_index(g, index);
}

static bool init_texture(Gpu *g, TextureSlot *t, const TextureDesc *d) {
    VkFormat fmt = to_vk_format(d->format);
    uint32_t mips = d->mips ? d->mips : 1u;
    uint32_t layers = d->layers ? d->layers : 1u;
    uint32_t depth = d->depth ? d->depth : 1u;
    uint32_t index;

    uint32_t old_gen = t->gen;
    if (fmt == VK_FORMAT_UNDEFINED) { fail(g, "texture_create: bad format"); return false; }
    if (mips > MAX_MIPS) { fail(g, "texture_create: mips > %d", MAX_MIPS); return false; }
    if (depth > 1u && layers > 1u) { fail(g, "texture_create: a 3D texture cannot have layers"); return false; }
    if (depth > 1u && (d->usage & (TEX_COLOR | TEX_DEPTH))) {
        fail(g, "texture_create: a 3D texture cannot be an attachment"); return false;
    }
    if (!alloc_texture_bindless_index(g, &index)) return false;
    memset(t, 0, sizeof *t);
    t->gen = old_gen ? old_gen : 1u;
    t->width = d->width; t->height = d->height; t->depth = depth;
    t->layers = layers; t->mips = mips;
    t->format = d->format; t->usage = d->usage; t->cls = d->cls;
    t->layout = (d->usage & TEX_STORAGE) ? VK_IMAGE_LAYOUT_GENERAL
                                         : VK_IMAGE_LAYOUT_UNDEFINED;
    t->bindless_index = index;
    copy_name(t->name, sizeof t->name, d->name ? d->name : "texture");
    if (!make_image(g, d->width, d->height, depth, layers, mips, fmt,
                    to_vk_image_usage(d->usage, format_is_depth(d->format)),
                    VK_SAMPLE_COUNT_1_BIT, &t->image, &t->memory)) {
        abort_texture_init(g, t, index);
        return false;
    }
    if (!make_view(g, t->image, fmt, to_vk_aspect(d->format), mips, layers, depth, &t->view)) {
        abort_texture_init(g, t, index);
        return false;
    }
    if (d->usage & TEX_STORAGE) {
        if (!make_view(g, t->image, fmt, to_vk_aspect(d->format), mips, layers, depth,
                       &t->storage_view)) {
            abort_texture_init(g, t, index);
            return false;
        }
    }
    t->live = true;
    write_image(g, BIND_SAMPLED_IMAGES, index, t->view,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (d->usage & TEX_STORAGE)
        write_image(g, BIND_STORAGE_IMAGES, index, t->storage_view,
                    VK_IMAGE_LAYOUT_GENERAL);
    /* initial_data is uploaded by texture_create(), which owns the handle.
     * Targets never have initial data. */
    return true;
}

Texture texture_create(Gpu *g, const TextureDesc *desc) {
    uint32_t idx = new_texture_slot(g);
    TextureSlot *t;
    if (!idx) { fail(g, "texture_create: texture table full"); return TEXTURE_NIL; }
    t = &g->textures[idx];
    if (!init_texture(g, t, desc)) {
        uint32_t generation = t->gen;
        memset(t, 0, sizeof *t);
        t->gen = generation;
        return TEXTURE_NIL;
    }
    if (desc->initial_data && desc->initial_data->data && desc->initial_data->size) {
        ImageRegion r;
        Upload *u;
        uint64_t expected = (uint64_t)desc->width * desc->height *
                            (desc->depth ? desc->depth : 1u) * format_bytes(desc->format);
        if (desc->initial_data->size < expected) {
            fail(g, "texture_create: initial_data too small (%llu < %llu)",
                 (unsigned long long)desc->initial_data->size,
                 (unsigned long long)expected);
        } else {
            memset(&r, 0, sizeof r);
            r.width = desc->width; r.height = desc->height;
            r.depth = desc->depth ? desc->depth : 1u;
            u = upload_begin(g);
            if (u) {
                upload_texture(u, (Texture){idx, t->gen}, &r, *desc->initial_data);
                upload_end(g, u);
            }
        }
    }
    return (Texture){idx, t->gen};
}

Target target_create(Gpu *g, const TargetDesc *desc) {
    uint32_t ti = new_texture_slot(g);
    uint32_t si = new_target_slot(g);
    TextureSlot *t;
    TargetSlot *s;
    TextureDesc td;
    if (!ti || !si) { fail(g, "target_create: target table full"); return TARGET_NIL; }
    memset(&td, 0, sizeof td);
    td.name = desc->name; td.width = desc->width; td.height = desc->height;
    td.depth = desc->depth ? desc->depth : 1u;
    td.layers = 1u; td.mips = desc->mips ? desc->mips : 1u;
    td.format = desc->format; td.cls = desc->cls;
    td.usage = TEX_SAMPLED | TEX_XFER_SRC | TEX_XFER_DST;
    if (desc->usage & TGT_COLOR)  td.usage |= TEX_COLOR;
    if (desc->usage & TGT_DEPTH)  td.usage |= TEX_DEPTH;
    if (desc->usage & TGT_STORAGE)td.usage |= TEX_STORAGE;
    t = &g->textures[ti];
    if (!init_texture(g, t, &td)) {
        uint32_t generation = t->gen;
        memset(t, 0, sizeof *t);
        t->gen = generation;
        return TARGET_NIL;
    }
    s = &g->targets[si];
    { uint32_t old_gen = s->gen; memset(s, 0, sizeof *s); s->gen = old_gen ? old_gen : 1u; }
    s->live = true; s->tex = t;
    s->width = desc->width; s->height = desc->height;
    s->format = desc->format; s->usage = desc->usage;
    s->samples = desc->samples ? desc->samples : SC_1;
    s->cls = desc->cls;
    s->frame_layout = t->layout;
    copy_name(s->name, sizeof s->name, desc->name ? desc->name : "target");
    return (Target){si, s->gen};
}

Sampler sampler_create(Gpu *g, const SamplerDesc *desc) {
    VkSamplerCreateInfo sci;
    uint32_t idx = new_sampler_slot(g);
    uint32_t bindless_index;
    SamplerSlot *s;
    VkResult r;
    if (!idx) { fail(g, "sampler_create: sampler table full"); return SAMPLER_NIL; }
    if (!alloc_sampler_bindless_index(g, &bindless_index)) return SAMPLER_NIL;
    s = &g->samplers[idx];
    { uint32_t old_gen = s->gen; memset(s, 0, sizeof *s); s->gen = old_gen ? old_gen : 1u; }

    memset(&sci, 0, sizeof sci);
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = to_vk_filter(desc->mag_filter);
    sci.minFilter = to_vk_filter(desc->min_filter);
    sci.mipmapMode = to_vk_mip_filter(desc->mip_filter);
    sci.addressModeU = to_vk_address(desc->addr_u);
    sci.addressModeV = to_vk_address(desc->addr_v);
    sci.addressModeW = to_vk_address(desc->addr_w);
    sci.mipLodBias = desc->lod_bias;
    sci.anisotropyEnable = desc->max_aniso > 1u && g->props.limits.maxSamplerAnisotropy > 1.0f;
    sci.maxAnisotropy = sci.anisotropyEnable
                      ? min_u32(desc->max_aniso, (uint32_t)g->props.limits.maxSamplerAnisotropy)
                      : 1.0f;
    sci.compareEnable = desc->compare != CMP_NONE;
    sci.compareOp = to_vk_compare(desc->compare);
    sci.minLod = desc->min_lod;
    sci.maxLod = desc->max_lod;
    sci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    r = vkCreateSampler(g->device, &sci, NULL, &s->sampler);
    if (r != VK_SUCCESS) {
        uint32_t gen = s->gen + 1u;
        fail(g, "vkCreateSampler: %s", result_name(r));
        release_sampler_bindless_index(g, bindless_index);
        memset(s, 0, sizeof *s);
        s->gen = gen;
        return SAMPLER_NIL;
    }
    s->live = true;
    s->bindless_index = bindless_index;
    {
        VkDescriptorImageInfo dii;
        VkWriteDescriptorSet w;
        memset(&dii, 0, sizeof dii);
        dii.sampler = s->sampler;
        memset(&w, 0, sizeof w);
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = g->set;
        w.dstBinding = BIND_SAMPLERS;
        w.dstArrayElement = s->bindless_index;
        w.descriptorCount = 1u;
        w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        w.pImageInfo = &dii;
        vkUpdateDescriptorSets(g->device, 1u, &w, 0u, NULL);
        count_descriptor_write(g);
    }
    return (Sampler){idx, s->gen};
}

uint32_t sampler_bindless_index(const Gpu *g, Sampler s) {
    const SamplerSlot *slot = sampler_slot(g, s);
    return slot ? slot->bindless_index : UINT32_MAX;
}

void buffer_destroy(Gpu *g, Buffer b) {
    BufferSlot *s = buffer_slot(g, b);
    /* Three kinds of handle reach this: a buffer_create() buffer (the only one
     * with a Vulkan buffer of its own), an arena_alloc() slice into the device
     * or transient arena (owned by the arena), and a slice into a staging chunk
     * (owned by the chunk). A slice has no independent lifetime to end, so this
     * retires the SLICE: its declared range stops counting as touched, which
     * is what lets the arena's owner be reclaimed without a false stall. The
     * distinction is the handle generation, not the index alone. */
    if (!s) return;
    if (b.index < 3u + STAGING_CHUNKS) {
        s->committed = 0u;
        return;
    }
    if (s->arena != ARENA_DEVICE) {
        fail(g, "buffer_destroy: handle %u is an arena slice, not a created buffer",
             b.index);
        return;
    }
    if (buffer_touched(s)) {
        DeleteItem d;
        memset(&d, 0, sizeof d);
        d.kind = DEL_BUFFER; d.a = (uint64_t)s->buffer; d.b = (uint64_t)s->memory;
        queue_delete(g, d);
    } else {
        vkDestroyBuffer(g->device, s->buffer, NULL);
        vkFreeMemory(g->device, s->memory, NULL);
    }
    s->live = false; ++s->gen;
    s->committed = 0u;
    memset(&s->buffer, 0, sizeof s->buffer);
    memset(&s->memory, 0, sizeof s->memory);
    s->mapped = NULL;
}
/* Resource destruction is deferred to the timeline, so a frame still in
 * flight never sees a freed image. All handle fields are cleared immediately:
 * gpu_destroy() then applies the pending deletes and cannot double-free. */
static void destroy_texture_deferred(Gpu *g, TextureSlot *s) {
    DeleteItem d;
    if (!s || !s->live) return;
    memset(&d, 0, sizeof d);
    d.kind = DEL_IMAGE;
    d.a = (uint64_t)s->image; d.b = (uint64_t)s->view;
    d.c = s->bindless_index;
    queue_delete(g, d);
    if (s->storage_view) {
        memset(&d, 0, sizeof d);
        d.kind = DEL_IMAGE_VIEW; d.a = (uint64_t)s->storage_view;
        queue_delete(g, d);
    }
    if (s->memory) {
        memset(&d, 0, sizeof d);
        d.kind = DEL_MEMORY; d.a = (uint64_t)s->memory;
        queue_delete(g, d);
    }
    s->live = false; ++s->gen;
    memset(&s->image, 0, sizeof s->image);
    memset(&s->view, 0, sizeof s->view);
    memset(&s->storage_view, 0, sizeof s->storage_view);
    memset(&s->memory, 0, sizeof s->memory);
}
void texture_destroy(Gpu *g, Texture t) {
    TextureSlot *s = texture_slot(g, t);
    if (!s || t.index == 0u) return;
    destroy_texture_deferred(g, s);
}
void target_destroy(Gpu *g, Target t) {
    TargetSlot *s = target_slot(g, t);
    if (!s || t.index == 0u || t.index == 0xFFFFFFFFu) return;
    if (s->tex) destroy_texture_deferred(g, s->tex);
    s->live = false; ++s->gen;
    s->tex = NULL;
}
void sampler_destroy(Gpu *g, Sampler h) {
    SamplerSlot *s = sampler_slot(g, h);
    if (!s) return;
    {
        DeleteItem d;
        memset(&d, 0, sizeof d);
        d.kind = DEL_SAMPLER; d.a = (uint64_t)s->sampler;
        d.b = s->bindless_index;
        queue_delete(g, d);
    }
    s->live = false; ++s->gen;
    s->sampler = VK_NULL_HANDLE;
}

bool buffer_info(const Gpu *g, Buffer b, BufferInfo *out) {
    const BufferSlot *s = buffer_slot(g, b);
    if (!s) return false;
    out->device_address = s->device_address;
    out->size = s->size;
    out->mapped = s->mapped;
    return true;
}
bool texture_info(const Gpu *g, Texture t, uint32_t *bindless_index) {
    const TextureSlot *s = texture_slot(g, t);
    if (!s) return false;
    *bindless_index = s->bindless_index;
    return true;
}
void gpu_tables(const Gpu *g, GpuTables *out) {
    out->storage_buffers.buffer = g->arenas[ARENA_DEVICE].handle;
    out->storage_buffers.offset = 0u;
    out->storage_buffers.size = g->arenas[ARENA_DEVICE].capacity;
    out->sampled_image_base = 0u;
    out->sampled_image_count = g->active_bindless;
    out->sampler_base = 0u;
    out->sampler_count = BINDLESS_SAMPLERS;
}

/* ============================================================================
 * 4. ARENAS
 * ==========================================================================*/

BufSlice arena_alloc(Gpu *g, Arena arena, uint64_t size, uint64_t align) {
    ArenaState *a;
    uint64_t base, off;
    if (size == 0u) return BUF_SLICE_NIL;
    if (arena == ARENA_STAGING) {
        /* A chunk is reusable once the timeline has retired it. Prefer one
         * that is already empty; otherwise take the first free one and reset
         * it. Inlined from a one-call-site helper so the two passes read as
         * the one decision they are. */
        uint32_t c = UINT32_MAX;
        StagingChunk *chunk;
        uint64_t real_align = align < 16u ? 16u : align_up(align, 16u);
        for (uint32_t i = 0; i < STAGING_CHUNKS; ++i)
            if (!g->staging[i].in_flight && g->staging[i].used == 0u) { c = i; break; }
        if (c == UINT32_MAX)
            for (uint32_t i = 0; i < STAGING_CHUNKS; ++i)
                if (!g->staging[i].in_flight) { c = i; g->staging[i].used = 0u; break; }
        if (c == UINT32_MAX) { fail(g, "arena_alloc: all staging chunks in flight"); return BUF_SLICE_NIL; }
        chunk = &g->staging[c];
        base = align_up(chunk->used, real_align);
        if (base + size > STAGING_CHUNK_BYTES) {
            fail(g, "arena_alloc: staging chunk full");
            return BUF_SLICE_NIL;
        }
        chunk->used = base + size;
        ++g->arenas[ARENA_STAGING].allocations;
        return (BufSlice){(Buffer){3u + c, 1u}, base, size};
    }
    if (arena != ARENA_DEVICE && arena != ARENA_TRANSIENT) return BUF_SLICE_NIL;
    a = &g->arenas[arena];
    align = align < ALIGN_DEVICE ? ALIGN_DEVICE : align;
    align = align < (uint64_t)g->props.limits.minStorageBufferOffsetAlignment
          ? (uint64_t)g->props.limits.minStorageBufferOffsetAlignment : align;
    if (arena == ARENA_TRANSIENT) {
        uint64_t region = a->capacity / g->frames_in_flight;
        base = a->base_offset + align_up(a->used, align);
        if (base - a->base_offset + size > region) {
            fail(g, "arena_alloc: transient region full");
            return BUF_SLICE_NIL;
        }
        off = base;
        a->used = align_up(a->used, align) + size;
    } else {
        off = align_up(a->used, align);
        if (off + size > a->capacity) {
            fail(g, "arena_alloc: device arena full");
            return BUF_SLICE_NIL;
        }
        a->used = off + size;
    }
    if (a->used > a->peak) a->peak = a->used;
    ++a->allocations;
    return (BufSlice){a->handle, off, size};
}

void arena_stats(const Gpu *g, Arena arena, ArenaStats *out) {
    const ArenaState *a;
    if (arena == ARENA_STAGING) {
        uint64_t used = 0, peak = 0, cap = 0;
        uint32_t i, allocs = 0;
        for (i = 0; i < STAGING_CHUNKS; ++i) {
            used += g->staging[i].used;
            cap += STAGING_CHUNK_BYTES;
            if (g->staging[i].used > peak) peak = g->staging[i].used;
        }
        allocs = g->arenas[ARENA_STAGING].allocations;
        out->reserved_bytes = cap; out->used_bytes = used;
        out->peak_bytes = peak; out->allocations = allocs;
        return;
    }
    a = &g->arenas[arena];
    out->reserved_bytes = a->capacity;
    out->used_bytes = a->used;
    out->peak_bytes = a->peak;
    out->allocations = a->allocations;
}

/* ============================================================================
 * 5. SHADER REGISTRY AND PIPELINES
 * ==========================================================================*/

static uint32_t new_shader_slot(Gpu *g) {
    uint32_t i;
    for (i = 1u; i < SLOT_SHADERS; ++i)
        if (!g->shaders[i].live) return i;
    return 0u;
}

bool shader_register_spv(Gpu *g, const uint32_t *words, uint32_t word_count,
                         const char *path, const char *entry,
                         ShaderStage2 stage, ShaderId *out) {
    VkShaderModuleCreateInfo smci;
    VkShaderModule module = VK_NULL_HANDLE;
    ShaderSlot *s;
    uint32_t idx;
    VkResult r;
    if (!words || word_count < 5u || words[0] != 0x07230203u) {
        fail(g, "shader_register_spv: not a SPIR-V module"); return false;
    }
    idx = new_shader_slot(g);
    if (!idx) { fail(g, "shader_register_spv: shader table full"); return false; }
    memset(&smci, 0, sizeof smci);
    smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = (size_t)word_count * 4u;
    smci.pCode = words;
    r = vkCreateShaderModule(g->device, &smci, NULL, &module);
    if (r != VK_SUCCESS) {
        fail(g, "vkCreateShaderModule: %s", result_name(r)); return false;
    }
    s = &g->shaders[idx];
    memset(s, 0, sizeof *s);
    s->module = module;
    s->live = true;
    s->spv_words = word_count;
    s->code_bytes = word_count * 4u;
    s->stage = stage;
    copy_name(s->path, sizeof s->path, path ? path : "embedded");
    copy_name(s->entry, sizeof s->entry, entry ? entry : "main");
    *out = (ShaderId)idx;
    return true;
}

bool shader_compile_in_process(Gpu *g, const ShaderSource *src, ShaderId *out) {
    const char *dot;
    if (g->frame_open) {
        fail(g, "shader_compile_in_process: compilation inside a frame is forbidden");
        return false;
    }
    if (!src || !src->path || !out) { fail(g, "shader_compile_in_process: bad argument"); return false; }
    dot = strrchr(src->path, '.');
    if (dot && !strcmp(dot, ".spv")) {
        FILE *f = fopen(src->path, "rb");
        uint32_t *words;
        long n;
        bool ok;
        if (!f) { fail(g, "shader_compile_in_process: cannot open %s", src->path); return false; }
        fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
        if (n < 20 || (n % 4) != 0) {
            fclose(f);
            fail(g, "shader_compile_in_process: bad .spv size %ld", n);
            return false;
        }
        words = (uint32_t *)malloc((size_t)n);
        if (!words || fread(words, 1, (size_t)n, f) != (size_t)n) {
            free(words); fclose(f);
            fail(g, "shader_compile_in_process: short read"); return false;
        }
        fclose(f);
        ok = shader_register_spv(g, words, (uint32_t)(n / 4), src->path,
                                 src->entry ? src->entry : "main", src->stage, out);
        free(words);
        return ok;
    }
    if (g->compiler) {
        const uint32_t *words = NULL;
        uint32_t count = 0;
        char err[ERROR_BYTES];
        bool ok;
        err[0] = 0;
        ok = g->compiler(g->compiler_user, src->path,
                         src->entry ? src->entry : "main", src->stage,
                         src->defines, src->define_count, &words, &count,
                         err, sizeof err);
        if (!ok) { fail(g, "in-process compiler: %s", err[0] ? err : "failed"); return false; }
        return shader_register_spv(g, words, count, src->path,
                                   src->entry ? src->entry : "main", src->stage, out);
    }
    fail(g, "shader_compile_in_process: no in-process compiler registered; "
               "pass a .spv path or call set_compiler()");
    return false;
}

void set_compiler(Gpu *g, compile_fn fn, void *user) {
    g->compiler = fn;
    g->compiler_user = user;
}

static const char *pipe_entry(const PipeKey *k, bool fragment) {
    const char *e = fragment ? k->fs_entry : k->vs_entry;
    return e ? e : "main";
}

static VkColorBlendEquationEXT to_vk_blend_equation(Blend b);

static bool build_graphics(Gpu *g, PipeSlot *p) {
    VkPipelineShaderStageCreateInfo stages[2];
    VkDynamicState dyn[16];
    VkPipelineDynamicStateCreateInfo dyn_ci;
    VkPipelineVertexInputStateCreateInfo vii;
    VkPipelineInputAssemblyStateCreateInfo iai;
    VkPipelineViewportStateCreateInfo vpi;
    VkPipelineRasterizationStateCreateInfo rsi;
    VkPipelineMultisampleStateCreateInfo msi;
    VkPipelineDepthStencilStateCreateInfo dsi;
    VkPipelineColorBlendAttachmentState cbas[MAX_COLOR_ATTACHMENTS];
    VkPipelineColorBlendStateCreateInfo cbi;
    VkPipelineRenderingCreateInfoKHR rdci;
    VkGraphicsPipelineCreateInfo gpci;
    VkFormat color_fmts[MAX_COLOR_ATTACHMENTS];
    VkResult r;
    uint32_t n_stages = 0, n_dyn = 0, i;
    ShaderSlot *vs = p->vs ? &g->shaders[p->vs] : NULL;
    ShaderSlot *fs = p->fs ? &g->shaders[p->fs] : NULL;

    if (vs && vs->live) {
        memset(&stages[n_stages], 0, sizeof stages[n_stages]);
        stages[n_stages].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[n_stages].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[n_stages].module = vs->module;
        stages[n_stages].pName = p->vs_entry;
        ++n_stages;
    }
    if (fs && fs->live) {
        memset(&stages[n_stages], 0, sizeof stages[n_stages]);
        stages[n_stages].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[n_stages].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[n_stages].module = fs->module;
        stages[n_stages].pName = p->fs_entry;
        ++n_stages;
    }
    if (n_stages == 0u) { fail(g, "pipe_intern: no shader stages"); return false; }

    /* Everything that used to be a pipeline variant is dynamic. No vertex
     * input state at all: geometry is fetched by SV_VertexID (§19). */
    dyn[n_dyn++] = VK_DYNAMIC_STATE_VIEWPORT;
    dyn[n_dyn++] = VK_DYNAMIC_STATE_SCISSOR;
    dyn[n_dyn++] = VK_DYNAMIC_STATE_CULL_MODE;
    dyn[n_dyn++] = VK_DYNAMIC_STATE_FRONT_FACE;
    dyn[n_dyn++] = VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY;
    dyn[n_dyn++] = VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE;
    dyn[n_dyn++] = VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE;
    dyn[n_dyn++] = VK_DYNAMIC_STATE_DEPTH_COMPARE_OP;
    if (g->eds3) {
        dyn[n_dyn++] = VK_DYNAMIC_STATE_RASTERIZATION_SAMPLES_EXT;
        dyn[n_dyn++] = VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT;
        dyn[n_dyn++] = VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT;
        dyn[n_dyn++] = VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT;
    }
    memset(&dyn_ci, 0, sizeof dyn_ci);
    dyn_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn_ci.dynamicStateCount = n_dyn;
    dyn_ci.pDynamicStates = dyn;

    memset(&vii, 0, sizeof vii);
    vii.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    memset(&iai, 0, sizeof iai);
    iai.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    iai.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    memset(&vpi, 0, sizeof vpi);
    vpi.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vpi.viewportCount = 1u; vpi.scissorCount = 1u;
    memset(&rsi, 0, sizeof rsi);
    rsi.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rsi.polygonMode = VK_POLYGON_MODE_FILL;
    rsi.cullMode = VK_CULL_MODE_NONE;
    rsi.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rsi.lineWidth = 1.0f;
    memset(&msi, 0, sizeof msi);
    msi.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    msi.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    /* Depth state that the key fixes. depthWriteEnable is NOT a dynamic state,
     * and a read-only depth attachment (ATT_DEPTH_READ) is illegal unless the
     * pipeline was created with depthWriteEnable = VK_FALSE, so this has to
     * come from the key rather than be overwritten per draw. */
    memset(&dsi, 0, sizeof dsi);
    dsi.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    dsi.depthTestEnable = p->key.depth != FMT_NONE && p->key.depth_cmp != CMP_NONE
                        ? VK_TRUE : VK_FALSE;
    dsi.depthWriteEnable = p->key.depth != FMT_NONE && p->key.depth_write
                         ? VK_TRUE : VK_FALSE;
    dsi.depthCompareOp = p->key.depth_cmp == CMP_NONE ? VK_COMPARE_OP_LESS
                                                      : to_vk_compare(p->key.depth_cmp);
    dsi.depthBoundsTestEnable = VK_FALSE;
    dsi.stencilTestEnable = VK_FALSE;

    memset(cbas, 0, sizeof cbas);
    for (i = 0; i < p->color_count; ++i) {
        Blend fallback = p->key.blend_static_off ? BLEND_OPAQUE
                                                  : p->key.blend_fallback;
        VkColorBlendEquationEXT eq = to_vk_blend_equation(fallback);
        cbas[i].blendEnable = fallback == BLEND_OPAQUE ? VK_FALSE : VK_TRUE;
        cbas[i].srcColorBlendFactor = eq.srcColorBlendFactor;
        cbas[i].dstColorBlendFactor = eq.dstColorBlendFactor;
        cbas[i].colorBlendOp = eq.colorBlendOp;
        cbas[i].srcAlphaBlendFactor = eq.srcAlphaBlendFactor;
        cbas[i].dstAlphaBlendFactor = eq.dstAlphaBlendFactor;
        cbas[i].alphaBlendOp = eq.alphaBlendOp;
        cbas[i].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                 VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    }
    memset(&cbi, 0, sizeof cbi);
    cbi.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cbi.attachmentCount = p->color_count;
    cbi.pAttachments = p->color_count ? cbas : NULL;

    for (i = 0; i < p->color_count; ++i)
        color_fmts[i] = to_vk_format(p->key.color[i]);
    memset(&rdci, 0, sizeof rdci);
    rdci.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
    rdci.colorAttachmentCount = p->color_count;
    rdci.pColorAttachmentFormats = p->color_count ? color_fmts : NULL;
    rdci.depthAttachmentFormat = p->key.depth == FMT_NONE ? VK_FORMAT_UNDEFINED
                                                          : to_vk_format(p->key.depth);
    rdci.stencilAttachmentFormat = p->key.depth == FMT_D24S8
                                 ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_UNDEFINED;
    if (p->key.view_mask) rdci.viewMask = p->key.view_mask;

    memset(&gpci, 0, sizeof gpci);
    gpci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpci.pNext = &rdci;
    gpci.stageCount = n_stages;
    gpci.pStages = stages;
    gpci.pVertexInputState = &vii;
    gpci.pInputAssemblyState = &iai;
    gpci.pViewportState = &vpi;
    gpci.pRasterizationState = &rsi;
    gpci.pMultisampleState = &msi;
    gpci.pDepthStencilState = &dsi;
    gpci.pColorBlendState = &cbi;
    gpci.pDynamicState = &dyn_ci;
    gpci.layout = g->pipe_layout;
    r = vkCreateGraphicsPipelines(g->device, g->pipe_cache, 1u, &gpci, NULL, &p->pipeline);
    if (r != VK_SUCCESS) { fail(g, "vkCreateGraphicsPipelines: %s", result_name(r)); return false; }
    return true;
}

static bool build_compute(Gpu *g, PipeSlot *p) {
    VkComputePipelineCreateInfo cpci;
    VkPipelineShaderStageCreateInfo stage;
    ShaderSlot *cs;
    VkResult r;
    if (!p->vs || !g->shaders[p->vs].live) {
        fail(g, "compute pipe: shader not registered"); return false;
    }
    cs = &g->shaders[p->vs];
    memset(&stage, 0, sizeof stage);
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = cs->module;
    stage.pName = p->vs_entry;
    memset(&cpci, 0, sizeof cpci);
    cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpci.stage = stage;
    cpci.layout = g->pipe_layout;
    r = vkCreateComputePipelines(g->device, g->pipe_cache, 1u, &cpci, NULL, &p->pipeline);
    if (r != VK_SUCCESS) { fail(g, "vkCreateComputePipelines: %s", result_name(r)); return false; }
    return true;
}

static bool key_equal(const PipeKey *a, const PipeKey *b) {
    uint32_t i;
    if (a->vs != b->vs || a->fs != b->fs || a->color_count != b->color_count ||
        a->depth != b->depth || a->view_mask != b->view_mask ||
        a->blend_fallback != b->blend_fallback ||
        a->blend_static_off != b->blend_static_off ||
        a->depth_cmp != b->depth_cmp || a->depth_write != b->depth_write) return false;
    if (strcmp(pipe_entry(a, false), pipe_entry(b, false)) ||
        strcmp(pipe_entry(a, true), pipe_entry(b, true))) return false;
    for (i = 0; i < a->color_count; ++i)
        if (a->color[i] != b->color[i]) return false;
    return true;
}

static uint32_t new_pipe_slot(Gpu *g) {
    uint32_t i;
    for (i = 1u; i < MAX_PIPES; ++i)
        if (!g->pipes[i].live) return i;
    return 0u;
}

/* `entry` is the compute entry point. Graphics entry points are part of the
 * key (PipeKey.vs_entry / .fs_entry), so they cannot be passed separately
 * without the two disagreeing. */
static Pipe intern_key(Gpu *g, const PipeKey *key, bool compute, const char *entry) {
    PipeSlot *p;
    uint32_t i, idx;
    for (i = 1u; i < MAX_PIPES; ++i) {
        if (!g->pipes[i].live || g->pipes[i].is_compute != compute) continue;
        if (key_equal(&g->pipes[i].key, key))
            return (Pipe){i, g->pipes[i].gen};
    }
    if (g->frame_open) {
        /* A pipeline is built by compiling SPIR-V and asking the driver for a
         * PSO, which can cost milliseconds. Doing that between frame_begin()
         * and frame_end() turns one slow frame into a visible hitch, so it is
         * a programming error: intern every pipe before the first frame. */
        ++g->active_stats.pipeline_creations;
        if (++g->compiled_this_frame > MAX_PIPES_IN_FRAME) {
            fail(g, "pipe_intern: %u pipelines compiled inside one frame; "
                    "intern pipes before frame_begin()", g->compiled_this_frame);
            return PIPE_NIL;
        }
    }
    idx = new_pipe_slot(g);
    if (!idx) {
        fail(g, "pipe_intern: MAX_PIPES (%d) exceeded", MAX_PIPES);
        return PIPE_NIL;
    }
    p = &g->pipes[idx];
    { uint32_t old_gen = p->gen; memset(p, 0, sizeof *p); p->gen = old_gen ? old_gen : 1u; }
    p->key = *key;
    p->vs = key->vs; p->fs = key->fs;
    p->color_count = key->color_count;
    p->is_compute = compute;
    /* The entry-point names belong to the KEY. build_graphics() reads
     * p->vs_entry / p->fs_entry, so taking them from the intern_key()
     * parameters instead made every pipe_intern() call build with whatever
     * the caller happened to pass ("main") while key_equal() compared the
     * names the caller actually asked for: two pipes that differ only in
     * entry point collided in the table, and a custom entry point was never
     * used. pipe_intern_compute() is the one caller that supplies an entry
     * name that is not in the key. */
    if (compute) {
        copy_name(p->vs_entry, sizeof p->vs_entry, entry ? entry : "main");
        copy_name(p->fs_entry, sizeof p->fs_entry, "main");
    } else {
        copy_name(p->vs_entry, sizeof p->vs_entry,
                  key->vs_entry ? key->vs_entry : "main");
        copy_name(p->fs_entry, sizeof p->fs_entry,
                  key->fs_entry ? key->fs_entry : "main");
    }
    {
        /* Through locals: the destinations live inside `p`, which is inside
         * `g`, and snprintf's %s argument may alias its own output. */
        char ve[64], fe[64];
        copy_name(ve, sizeof ve, p->vs_entry);
        copy_name(fe, sizeof fe, key->fs ? p->fs_entry : "depth");
        snprintf(p->name, sizeof p->name, "%s_%.16s_%.16s%s",
                 compute ? "cs" : "gfx", ve, fe,
                 key->blend_static_off ? "_opaque" : "");
    }
    if (compute ? !build_compute(g, p) : !build_graphics(g, p)) {
        p->live = false;
        return PIPE_NIL;
    }
    p->live = true;
    return (Pipe){idx, p->gen};
}

Pipe pipe_intern(Gpu *g, const PipeKey *key) {
    if (!key || key->color_count > MAX_COLOR_ATTACHMENTS) {
        fail(g, "pipe_intern: bad key"); return PIPE_NIL;
    }
    return intern_key(g, key, false, NULL);
}
Pipe pipe_find(const Gpu *g, const PipeKey *key) {
    uint32_t i;
    if (!key) return PIPE_NIL;
    for (i = 1u; i < MAX_PIPES; ++i)
        if (g->pipes[i].live && !g->pipes[i].is_compute &&
            key_equal(&g->pipes[i].key, key))
            return (Pipe){i, g->pipes[i].gen};
    return PIPE_NIL;
}
Pipe pipe_intern_compute(Gpu *g, ShaderId cs, const char *entry) {
    PipeKey k;
    memset(&k, 0, sizeof k);
    k.vs = cs;
    k.vs_entry = NULL;
    return intern_key(g, &k, true, entry);
}
uint32_t pipe_count(const Gpu *g) {
    uint32_t i, n = 0;
    for (i = 1u; i < MAX_PIPES; ++i) if (g->pipes[i].live) ++n;
    return n;
}
void pipe_stats(const Gpu *g, uint32_t *pipelines, uint32_t *compiled_this_frame) {
    if (pipelines) *pipelines = pipe_count(g);
    if (compiled_this_frame) *compiled_this_frame = g->compiled_this_frame;
}

DrawState draw_state_opaque(uint8_t samples, uint8_t cull) {
    DrawState s;
    memset(&s, 0, sizeof s);
    s.samples = samples; s.blend = BLEND_OPAQUE;
    s.depth_cmp = CMP_LESS; s.depth_write = 1u;
    s.cull = cull; s.topo = TOPO_TRI; s.write_mask = 0xFFFFu;
    return s;
}
DrawState draw_state_alpha(uint8_t samples, uint8_t cull) {
    DrawState s = draw_state_opaque(samples, cull);
    s.blend = BLEND_ALPHA; s.depth_write = 0u;
    return s;
}
DrawState draw_state_depth_only(uint8_t samples) {
    DrawState s = draw_state_opaque(samples, CULL_BACK);
    s.blend = BLEND_OPAQUE; s.write_mask = 0u;
    return s;
}

/* ============================================================================
 * 6. FRAME PROGRAM — declaration, validation, barrier plan, cost preview
 * ==========================================================================*/

typedef struct TrackBuf { VkAccessFlags access; VkPipelineStageFlags stage; uint32_t pass; } TrackBuf;
typedef struct TrackImg { VkImageLayout layout; uint32_t pass; } TrackImg;

static void add_buf_barrier(BarrierPlan *plan, const BarrierBuf *b) {
    uint32_t i;
    for (i = 0; i < plan->buf_count; ++i) {
        BarrierBuf *e = &plan->bufs[i];
        if (e->slot != b->slot) continue;
        /* Same buffer, one pass: merge into ONE range and one access set
         * instead of paying for two barrier entries. */
        uint64_t end0 = e->offset + e->size;
        uint64_t end1 = b->offset + b->size;
        uint64_t begin = e->offset < b->offset ? e->offset : b->offset;
        uint64_t end = end0 > end1 ? end0 : end1;
        e->offset = begin; e->size = end - begin;
        e->src_access |= b->src_access; e->dst_access |= b->dst_access;
        e->src_stage |= b->src_stage;  e->dst_stage |= b->dst_stage;
        return;
    }
    if (plan->buf_count == MU_ARRAY_COUNT(plan->bufs)) {
        /* Dropping a buffer barrier is a silent data race between passes:
         * assert rather than continue. */
        MU_ASSERT(plan->buf_count < MU_ARRAY_COUNT(plan->bufs));
        return;
    }
    plan->bufs[plan->buf_count++] = *b;
}
/* One entry per image per pass, because Vulkan needs one transition between
 * distinct layouts and the entry can carry both ends of it. When the same
 * image is named twice in a pass (a colour attachment that is also sampled),
 * the SECOND use's layouts would silently replace the first and the pass would
 * begin rendering into a layout nothing transitioned to. Merging keeps the
 * first entry's old_layout and the last use's new_layout, which is the single
 * transition the pass actually needs. */
static void add_img_barrier(BarrierPlan *plan, const BarrierImg *b) {
    uint32_t i;
    for (i = 0; i < plan->img_count; ++i) {
        BarrierImg *e = &plan->imgs[i];
        if (e->slot != b->slot) continue;
        e->new_layout = b->new_layout;
        e->src_access |= b->src_access; e->dst_access |= b->dst_access;
        e->src_stage |= b->src_stage;   e->dst_stage |= b->dst_stage;
        /* Discarding needs BOTH uses to be safe: an entry that keeps the
         * previous contents cannot also declare them undefined. */
        e->discard = e->discard && b->discard;
        /* The transition covers the union of the subresources named. */
        if (b->base_mip < e->base_mip) e->base_mip = b->base_mip;
        if (b->base_mip + b->mip_count > e->base_mip + e->mip_count)
            e->mip_count = b->base_mip + b->mip_count - e->base_mip;
        if (b->base_layer < e->base_layer) e->base_layer = b->base_layer;
        if (b->base_layer + b->layer_count > e->base_layer + e->layer_count)
            e->layer_count = b->base_layer + b->layer_count - e->base_layer;
        return;
    }
    if (plan->img_count == MU_ARRAY_COUNT(plan->imgs)) {
        /* Dropping an image barrier leaves the next pass reading a layout the
         * GPU never transitioned to. */
        MU_ASSERT(plan->img_count < MU_ARRAY_COUNT(plan->imgs));
        return;
    }
    plan->imgs[plan->img_count++] = *b;
}

static uint32_t target_index(Target t) {
    return t.index == 0xFFFFFFFFu ? 0u : t.index;
}
static VkImageLayout to_vk_layout_for_use(AttUse use, bool depth) {
    switch (use) {
    case ATT_STORAGE_RW:   return VK_IMAGE_LAYOUT_GENERAL;
    case ATT_STORAGE_READ: return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    case ATT_DEPTH_WRITE:
    case ATT_DEPTH_CLEAR:  return VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    case ATT_DEPTH_READ:   return VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
    default:
        (void)depth;
        return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
}
static VkAccessFlags to_vk_layout_access(VkImageLayout l, bool dst) {
    switch (l) {
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        return dst ? (VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT)
                   : VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:
    case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL:
        return dst ? (VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT)
                   : VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        return dst ? VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_SHADER_WRITE_BIT;
    case VK_IMAGE_LAYOUT_GENERAL:
        return VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    default: return 0u;
    }
}
static VkPipelineStageFlags to_vk_layout_stage(VkImageLayout l) {
    switch (l) {
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: return VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:
    case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL:
        return VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
               VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    default: return VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    }
}

static bool plan_barriers(Gpu *g, FrameProgram *prog) {
    TrackBuf bufs[SLOT_BUFFERS];
    TrackImg imgs[MAX_TARGETS];
    uint32_t i, j;

    memset(bufs, 0, sizeof bufs);
    /* A persistent image is NOT in UNDEFINED when the frame begins: the last
     * frame left it in whatever layout its final transition produced, and
     * apply_barriers()/pass_close() recorded that in TextureSlot.layout.
     * Assuming UNDEFINED made every `oldLayout = UNDEFINED` transition both a
     * validation error (oldLayout must match the real layout) and a logical
     * discard: a --check path that renders a target in one program and then
     * samples it from another lost the pixels. Transient and readback
     * resources are the ones that really start undefined. */
    for (i = 0; i < MAX_TARGETS; ++i) {
        TargetSlot *ts = &g->targets[i];
        imgs[i].layout = (ts->tex && ts->live && ts->cls == RES_PERSISTENT)
                       ? ts->tex->layout : VK_IMAGE_LAYOUT_UNDEFINED;
        imgs[i].pass = UINT32_MAX;
    }
    for (i = 0; i < prog->count; ++i) {
        PassPlan *pp = &prog->passes[i];
        PassDesc *d = &pp->desc;
        VkPipelineStageFlags dst_stage = to_vk_stage_for_kind(d->kind);
        VkPipelineStageFlags explicit_stages = d->read_stages ? to_vk_stage_bits(d->read_stages) : 0u;
        BarrierPlan *plan = &pp->barriers;

        for (j = 0; j < d->buf_read_count; ++j) {
            const BufUse *u = &pp->buf_reads[j];
            BufferSlot *bs = buffer_slot(g, u->slice.buffer);
            uint32_t si;
            TrackBuf *tr;
            BarrierBuf b;
            if (!bs) { fail(g, "pass '%s': invalid buffer read handle", d->name); return false; }
            if (u->slice.size == 0u) {
                fail(g, "pass '%s': buf_read %u has size 0 (no whole-buffer shorthand)", d->name, j);
                return false;
            }
            if (u->slice.offset + u->slice.size > bs->size) {
                fail(g, "pass '%s': buf_read %u out of bounds", d->name, j); return false;
            }
            si = u->slice.buffer.index;
            tr = &bufs[si];
            memset(&b, 0, sizeof b);
            b.slot = si; b.offset = u->slice.offset; b.size = u->slice.size;
            b.dst_access = to_vk_access_bits(u->access, true);
            b.dst_stage = (u->stages ? to_vk_stage_bits(u->stages)
                                     : (explicit_stages ? explicit_stages : dst_stage))
                        | to_vk_stage_extra(u->access);
            if (tr->pass != UINT32_MAX && (tr->access & (VK_ACCESS_SHADER_WRITE_BIT |
                                                         VK_ACCESS_TRANSFER_WRITE_BIT))) {
                b.src_access = tr->access;
                b.src_stage = tr->stage;
                add_buf_barrier(plan, &b);
            }
            /* A read leaves the range readable; it does not clear a pending
             * write, so the next pass must still see the producer's stage. */
            tr->access |= b.dst_access;
            tr->stage |= b.dst_stage;
            tr->pass = i;
            commit_range(g, si, u->slice.offset, u->slice.size);
        }
        for (j = 0; j < d->buf_write_count; ++j) {
            const BufUse *u = &pp->buf_writes[j];
            BufferSlot *bs = buffer_slot(g, u->slice.buffer);
            uint32_t si;
            TrackBuf *tr;
            BarrierBuf b;
            if (!bs) { fail(g, "pass '%s': invalid buffer write handle", d->name); return false; }
            if (u->slice.size == 0u) {
                fail(g, "pass '%s': buf_write %u has size 0", d->name, j); return false;
            }
            if (u->slice.offset + u->slice.size > bs->size) {
                fail(g, "pass '%s': buf_write %u out of bounds", d->name, j); return false;
            }
            si = u->slice.buffer.index;
            tr = &bufs[si];
            memset(&b, 0, sizeof b);
            b.slot = si; b.offset = u->slice.offset; b.size = u->slice.size;
            b.dst_access = to_vk_access_bits(u->access, true);
            /* A write to indirect arguments is consumed by the command
             * processor: the barrier's src side has to name that stage or the
             * next indirect draw has no ordering against the writer. */
            b.dst_stage = (u->stages ? to_vk_stage_bits(u->stages) : dst_stage)
                        | to_vk_stage_extra(u->access);
            if (tr->pass != UINT32_MAX && tr->pass != i) {
                b.src_access = tr->access;
                b.src_stage = tr->stage;
                add_buf_barrier(plan, &b);
            }
            tr->access = b.dst_access; tr->stage = b.dst_stage; tr->pass = i;
            commit_range(g, si, u->slice.offset, u->slice.size);
        }
        /* Attachments and sampled/storage targets: one image barrier per
         * target per pass, derived from intent, never from a caller layout. */
        for (j = 0; j < d->color_count; ++j) {
            AttachDesc *a = &pp->colors[j];
            TargetSlot *ts = target_slot(g, a->target);
            uint32_t ti = target_index(a->target);
            VkImageLayout want = to_vk_layout_for_use(a->use, false);
            BarrierImg b;
            if (!ts) { fail(g, "pass '%s': invalid color target", d->name); return false; }
            pp->color_layouts[j] = want;
            memset(&b, 0, sizeof b);
            b.slot = ti; b.old_layout = imgs[ti].layout; b.new_layout = want;
            b.base_mip = a->mip; b.mip_count = 1u; b.layer_count = 1u;
            b.src_access = to_vk_layout_access(imgs[ti].layout, false);
            b.dst_access = to_vk_layout_access(want, true);
            b.src_stage = imgs[ti].pass == UINT32_MAX ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                                                      : to_vk_layout_stage(imgs[ti].layout);
            b.dst_stage = to_vk_layout_stage(want);
            if (a->use == ATT_WRITE || a->use == ATT_CLEAR) {
                /* Fully overwritten: UNDEFINED is legal and skips the fetch
                 * and decompress of last frame's contents (02 §3). */
                b.old_layout = VK_IMAGE_LAYOUT_UNDEFINED;
                b.src_access = 0u;
                b.src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
                b.discard = true;
            }
            if (b.old_layout != b.new_layout) add_img_barrier(plan, &b);
            imgs[ti].layout = want; imgs[ti].pass = i;
        }
        if (d->depth) {
            AttachDesc *a = &pp->depth_att;
            TargetSlot *ts = target_slot(g, a->target);
            uint32_t ti = target_index(a->target);
            VkImageLayout want = to_vk_layout_for_use(a->use, true);
            BarrierImg b;
            if (!ts) { fail(g, "pass '%s': invalid depth target", d->name); return false; }
            pp->depth_layout = want;
            memset(&b, 0, sizeof b);
            b.slot = ti; b.old_layout = imgs[ti].layout; b.new_layout = want;
            b.base_mip = a->mip; b.mip_count = 1u; b.layer_count = 1u;
            b.src_access = to_vk_layout_access(imgs[ti].layout, false);
            b.dst_access = to_vk_layout_access(want, true);
            b.src_stage = imgs[ti].pass == UINT32_MAX ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                                                      : to_vk_layout_stage(imgs[ti].layout);
            b.dst_stage = to_vk_layout_stage(want);
            if (a->use == ATT_DEPTH_WRITE || a->use == ATT_DEPTH_CLEAR) {
                b.old_layout = VK_IMAGE_LAYOUT_UNDEFINED;
                b.src_access = 0u;
                b.src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
                b.discard = true;
            }
            if (b.old_layout != b.new_layout) add_img_barrier(plan, &b);
            imgs[ti].layout = want; imgs[ti].pass = i;
        }
        for (j = 0; j < d->tex_read_count; ++j) {
            TargetSlot *ts = target_slot(g, pp->tex_reads[j]);
            uint32_t ti = target_index(pp->tex_reads[j]);
            BarrierImg b;
            if (!ts) { fail(g, "pass '%s': invalid tex_read target", d->name); return false; }
            /* A frame-local image nobody wrote is a build error, not a
             * validation-layer message at runtime (api-v2-design.md §3.2). */
            if (ts->cls == RES_FRAME && imgs[ti].pass == UINT32_MAX) {
                fail(g, "pass '%s': reads transient '%s' that no pass writes",
                     d->name, ts->name); return false;
            }
            for (uint32_t k = 0; k < d->tex_write_count; ++k) {
                if (pp->tex_writes[k].index == pp->tex_reads[j].index &&
                    pp->tex_writes[k].gen == pp->tex_reads[j].gen) {
                    fail(g, "pass '%s': feedback loop on '%s'", d->name, ts->name);
                    return false;
                }
            }
            memset(&b, 0, sizeof b);
            b.slot = ti; b.old_layout = imgs[ti].layout;
            b.new_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            b.mip_count = 1u; b.layer_count = 1u;
            b.src_access = to_vk_layout_access(imgs[ti].layout, false);
            b.dst_access = VK_ACCESS_SHADER_READ_BIT;
            b.src_stage = imgs[ti].pass == UINT32_MAX ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                                                      : to_vk_layout_stage(imgs[ti].layout);
            b.dst_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
            if (b.old_layout != b.new_layout) add_img_barrier(plan, &b);
            imgs[ti].layout = b.new_layout; imgs[ti].pass = i;
        }
        for (j = 0; j < d->tex_write_count; ++j) {
            TargetSlot *ts = target_slot(g, pp->tex_writes[j]);
            uint32_t ti = target_index(pp->tex_writes[j]);
            BarrierImg b;
            if (!ts) { fail(g, "pass '%s': invalid tex_write target", d->name); return false; }
            memset(&b, 0, sizeof b);
            b.slot = ti; b.old_layout = imgs[ti].layout;
            b.new_layout = VK_IMAGE_LAYOUT_GENERAL;
            b.mip_count = 1u; b.layer_count = 1u;
            b.src_access = to_vk_layout_access(imgs[ti].layout, false);
            b.dst_access = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
            b.src_stage = imgs[ti].pass == UINT32_MAX ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                                                      : to_vk_layout_stage(imgs[ti].layout);
            b.dst_stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
            if (b.old_layout != b.new_layout) add_img_barrier(plan, &b);
            imgs[ti].layout = b.new_layout; imgs[ti].pass = i;
        }
    }
    return true;
}

static uint64_t add_sat_u64(uint64_t a, uint64_t b) {
    return UINT64_MAX - a < b ? UINT64_MAX : a + b;
}

static uint64_t mul_sat_u64(uint64_t a, uint64_t b) {
    return b && a > UINT64_MAX / b ? UINT64_MAX : a * b;
}

/* Logical bytes named by the pass declarations, not a prediction of DRAM/L2
 * traffic. It assumes full-resource texture access and ignores caches,
 * compression, shader-dependent access patterns, and overdraw. */
static uint64_t target_level_bytes(const TargetSlot *ts, uint32_t mip,
                                  bool include_samples) {
    uint64_t w, h, bpp, bytes;
    if (!ts) return 0u;
    w = ts->width >> mip; h = ts->height >> mip;
    if (!w) w = 1u;
    if (!h) h = 1u;
    bpp = format_bytes(ts->format);
    bytes = mul_sat_u64(mul_sat_u64(w, h), bpp);
    if (include_samples) bytes = mul_sat_u64(bytes, (uint64_t)ts->samples);
    return bytes;
}

static uint64_t target_all_mips_bytes(const TargetSlot *ts) {
    uint64_t bytes = 0u;
    uint32_t mip;
    if (!ts || !ts->tex) return 0u;
    for (mip = 0; mip < ts->tex->mips; ++mip) {
        uint64_t depth = ts->tex->depth >> mip;
        if (!depth) depth = 1u;
        bytes = add_sat_u64(bytes, mul_sat_u64(
            target_level_bytes(ts, mip, true), depth));
    }
    return mul_sat_u64(bytes, ts->tex->layers ? ts->tex->layers : 1u);
}

static uint64_t pass_declared_bytes(const Gpu *g, const PassPlan *pp) {
    const PassDesc *d = &pp->desc;
    uint64_t total = 0u;
    uint32_t i;
    for (i = 0; i < d->color_count; ++i) {
        const AttachDesc *a = &pp->colors[i];
        const TargetSlot *ts = target_slot(g, a->target);
        uint64_t bytes = target_level_bytes(ts, a->mip, true);
        uint32_t touches = (a->use == ATT_WRITE_KEEP || a->use == ATT_STORAGE_RW)
                         ? 2u : 1u;
        total = add_sat_u64(total, mul_sat_u64(bytes, touches));
        if (HANDLE_VALID(a->resolve))
            total = add_sat_u64(total,
                target_level_bytes(target_slot(g, a->resolve), a->mip, true));
    }
    if (pp->has_depth) {
        const AttachDesc *a = &pp->depth_att;
        total = add_sat_u64(total, target_level_bytes(target_slot(g, a->target),
                                                      a->mip, true));
    }
    for (i = 0; i < d->tex_read_count; ++i)
        total = add_sat_u64(total, target_all_mips_bytes(target_slot(g, pp->tex_reads[i])));
    for (i = 0; i < d->tex_write_count; ++i)
        total = add_sat_u64(total, target_all_mips_bytes(target_slot(g, pp->tex_writes[i])));
    for (i = 0; i < d->buf_read_count; ++i)
        total = add_sat_u64(total, pp->buf_reads[i].slice.size);
    for (i = 0; i < d->buf_write_count; ++i)
        total = add_sat_u64(total, pp->buf_writes[i].slice.size);
    return total;
}

static void init_pass_stats(FrameProgram *prog) {
    uint32_t i;
    for (i = 0; i < prog->count; ++i) {
        PassStat *stat = &prog->passes[i].stat;
        memset(stat, 0, sizeof *stat);
        copy_name(stat->name, sizeof stat->name, prog->passes[i].desc.name);
        stat->declared_bytes = pass_declared_bytes(prog->gpu, &prog->passes[i]);
    }
}

FrameProgram *frame_program_build(Gpu *g, const PassDesc *passes, uint32_t count) {
    FrameProgram *prog;
    uint32_t i, j;
    if (!passes || count == 0u || count > MAX_PASSES) {
        fail(g, "frame_program_build: pass count %u (max %d)", count, MAX_PASSES);
        return NULL;
    }
    prog = (FrameProgram *)calloc(1u, sizeof *prog);
    if (!prog) { fail(g, "frame_program_build: out of host memory"); return NULL; }
    prog->gpu = g;
    prog->count = count;
    for (i = 0; i < count; ++i) {
        const PassDesc *src = &passes[i];
        PassPlan *pp = &prog->passes[i];
        PassDesc *d = &pp->desc;
        *d = *src;
        if (!d->name[0]) snprintf(d->name, sizeof d->name, "pass_%u", i);
        if (d->color_count > MAX_COLOR_ATTACHMENTS ||
            d->buf_read_count > MAX_BINDS_PER_PASS ||
            d->buf_write_count > MAX_BINDS_PER_PASS ||
            d->tex_read_count > MAX_BINDS_PER_PASS ||
            d->tex_write_count > MAX_BINDS_PER_PASS) {
            fail(g, "pass '%s': exceeds MAX_BINDS_PER_PASS / attachment cap", d->name);
            goto fail;
        }
        if (d->color_count + d->tex_read_count + d->tex_write_count +
            d->buf_read_count + d->buf_write_count > MAX_BINDS_PER_PASS * 2u) {
            fail(g, "pass '%s': too many declared bindings for one pass", d->name);
            goto fail;
        }
        /* The barrier tables are sized by the most DISTINCT slots a pass can
         * name, and the check above is what makes that bound reachable-but-not-
         * exceedable. Stated as an assert so the reasoning is testable rather
         * than implied by two constants that have to be kept in step. */
        MU_ASSERT(d->color_count + (d->depth ? 1u : 0u) +
                  d->tex_read_count + d->tex_write_count <= MAX_IMAGES_PER_PASS);
        MU_ASSERT(d->buf_read_count + d->buf_write_count <=
                  MU_ARRAY_COUNT(((BarrierPlan *)0)->bufs));
        for (j = 0; j < d->color_count; ++j) pp->colors[j] = src->colors[j];
        d->colors = pp->colors;
        if (d->depth) {
            pp->depth_att = *src->depth;
            d->depth = &pp->depth_att;
            pp->has_depth = true;
        }
        for (j = 0; j < d->tex_read_count; ++j) pp->tex_reads[j] = src->tex_reads[j];
        for (j = 0; j < d->tex_write_count; ++j) pp->tex_writes[j] = src->tex_writes[j];
        for (j = 0; j < d->buf_read_count; ++j) pp->buf_reads[j] = src->buf_reads[j];
        for (j = 0; j < d->buf_write_count; ++j) pp->buf_writes[j] = src->buf_writes[j];
        d->tex_reads = pp->tex_reads; d->tex_writes = pp->tex_writes;
        d->buf_reads = pp->buf_reads; d->buf_writes = pp->buf_writes;

        pp->width = g->extent_w; pp->height = g->extent_h;
        if (d->color_count) {
            TargetSlot *ts = target_slot(g, pp->colors[0].target);
            if (ts) { pp->width = ts->width; pp->height = ts->height; }
        } else if (pp->has_depth) {
            TargetSlot *ts = target_slot(g, pp->depth_att.target);
            if (ts) { pp->width = ts->width; pp->height = ts->height; }
        }
        /* A pass's pipe must agree with its attachments, otherwise the driver
         * rejects it at draw time instead of at build time. */
        if (HANDLE_VALID(d->pipe)) {
            PipeSlot *ps = pipe_slot(g, d->pipe);
            if (!ps) { fail(g, "pass '%s': stale pipe handle", d->name); goto fail; }
            if (!ps->is_compute && ps->key.color_count != d->color_count) {
                fail(g, "pass '%s': pipe has %u colour formats, pass has %u",
                     d->name, ps->key.color_count, d->color_count); goto fail;
            }
            if (ps->is_compute && d->kind != PASS_COMPUTE) {
                fail(g, "'%s': compute pipe in a %s pass", d->name,
                     d->kind == PASS_GRAPHICS ? "graphics" : "unknown"); goto fail;
            }
            if (!ps->is_compute) {
                if (d->kind != PASS_GRAPHICS) {
                    fail(g, "'%s': graphics pipe in a compute pass", d->name); goto fail;
                }
                for (j = 0; j < d->color_count; ++j) {
                    TargetSlot *ts = target_slot(g, pp->colors[j].target);
                    if (ts && ts->format != ps->key.color[j]) {
                        fail(g, "pass '%s': colour %u format mismatch", d->name, j);
                        goto fail;
                    }
                }
                /* The depth format is part of the pipeline, so a pass that
                 * attaches a different depth format draws with a pipeline the
                 * driver rejects. R8/R16 depth prepasses vs D32F shadow maps
                 * is exactly where this shows up. */
                {
                    TargetSlot *zs = pp->has_depth ? target_slot(g, pp->depth_att.target) : NULL;
                    Format want = zs ? zs->format : FMT_NONE;
                    if (want != ps->key.depth) {
                        fail(g, "pass '%s': depth format does not match the pipe", d->name);
                        goto fail;
                    }
                }
            }
        }
        if (pp->has_depth && d->color_count) {
            TargetSlot *c = target_slot(g, pp->colors[0].target);
            TargetSlot *z = target_slot(g, pp->depth_att.target);
            if (c && z && (c->width != z->width || c->height != z->height)) {
                fail(g, "pass '%s': depth extent differs from colour", d->name); goto fail;
            }
        }
    }
    if (!plan_barriers(g, prog)) goto fail;
    init_pass_stats(prog);
    return prog;
fail:
    free(prog);
    return NULL;
}

void frame_program_destroy(Gpu *g, FrameProgram *p) {
    (void)g;
    free(p);
}

void frame_program_report(const Gpu *g, const FrameProgram *p, const char *path) {
    FILE *f = path ? fopen(path, "w") : stdout;
    uint32_t i;
    (void)g;
    if (!f) return;
    fprintf(f, "# frame program: %u passes\n", p ? p->count : 0u);
    fprintf(f, "# declared_B is a logical estimate from resource declarations, not GPU traffic\n");
    fprintf(f, "# pass                            gpu_ms  declared_B    N N_eff steps/pixel workgroup tile\n");
    if (p) for (i = 0; i < p->count; ++i) {
        const PassStat *s = &p->passes[i].stat;
        fprintf(f, "%-32s %7.3f %11llu %4u %5u %11.3f %u,%u,%u %4u\n",
                s->name, s->gpu_ms, (unsigned long long)s->declared_bytes,
                s->samples, s->effective_samples, s->steps_per_pixel,
                s->workgroup_x, s->workgroup_y, s->workgroup_z, s->tile_size);
    }
    if (f != stdout) fclose(f);
}

/* ============================================================================
 * 7. FRAME LIFECYCLE — one command buffer, one submit, one present
 * ==========================================================================*/

static void delete_apply(Gpu *g, DeleteItem *d) {
    switch (d->kind) {
    case DEL_BUFFER:
        vkDestroyBuffer(g->device, (VkBuffer)d->a, NULL);
        vkFreeMemory(g->device, (VkDeviceMemory)d->b, NULL);
        break;
    case DEL_IMAGE:
        vkDestroyImageView(g->device, (VkImageView)d->b, NULL);
        vkDestroyImage(g->device, (VkImage)d->a, NULL);
        release_texture_bindless_index(g, d->c);
        break;
    case DEL_IMAGE_VIEW:
        vkDestroyImageView(g->device, (VkImageView)d->a, NULL);
        break;
    case DEL_SAMPLER:
        vkDestroySampler(g->device, (VkSampler)d->a, NULL);
        release_sampler_bindless_index(g, (uint32_t)d->b);
        break;
    case DEL_PIPELINE:
        vkDestroyPipeline(g->device, (VkPipeline)d->a, NULL);
        break;
    case DEL_SHADER_MODULE:
        vkDestroyShaderModule(g->device, (VkShaderModule)d->a, NULL);
        break;
    case DEL_COMMAND_BUFFER:
        vkFreeCommandBuffers(g->device, g->cmd_pool, 1u, (VkCommandBuffer *)&d->a);
        break;
    case DEL_MEMORY:
        vkFreeMemory(g->device, (VkDeviceMemory)d->a, NULL);
        break;
    case DEL_QUERY_POOL:
        vkDestroyQueryPool(g->device, (VkQueryPool)d->a, NULL);
        break;
    case DEL_SWAPCHAIN_IMAGE:
        break;
    }
}

static void resolve_frame(Gpu *g, FrameSlot *f) {
    uint64_t pass_stamps[2u * MAX_PASSES];
    uint64_t scope_stamps[2u * MAX_PROFILE_SCOPES];
    uint32_t i;
    VkResult r;
    if (!f->in_flight) return;

    if (f->pending.pass_count) {
        uint32_t count = 2u * f->pending.pass_count;
        memset(pass_stamps, 0, sizeof pass_stamps);
        r = vkGetQueryPoolResults(g->device, f->queries, 0u, count,
                                  (size_t)count * sizeof(uint64_t), pass_stamps,
                                  sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
        if (r == VK_SUCCESS) {
            double period = (double)g->props.limits.timestampPeriod;
            for (i = 0; i < f->pending.pass_count; ++i) {
                uint64_t a = pass_stamps[2u * i], b = pass_stamps[2u * i + 1u];
                double ms = b >= a ? (double)(b - a) * period / 1e6 : 0.0;
                f->pending.passes[i].gpu_ms = ms;
                f->pending.gpu_ms += ms;
            }
            if (f->pending_program)
                for (i = 0; i < f->pending_program->count && i < f->pending.pass_count; ++i)
                    f->pending_program->passes[i].stat.gpu_ms = f->pending.passes[i].gpu_ms;
        }
    }

    if (f->pending.scope_count) {
        uint32_t count = 2u * f->pending.scope_count;
        uint32_t first = 2u * MAX_PASSES;
        memset(scope_stamps, 0, sizeof scope_stamps);
        r = vkGetQueryPoolResults(g->device, f->queries, first, count,
                                  (size_t)count * sizeof(uint64_t), scope_stamps,
                                  sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
        if (r == VK_SUCCESS) {
            double period = (double)g->props.limits.timestampPeriod;
            for (i = 0; i < f->pending.scope_count; ++i) {
                uint64_t a = scope_stamps[2u * i], b = scope_stamps[2u * i + 1u];
                if (f->pending.scopes[i].complete) {
                    f->pending.scopes[i].gpu_ms = b >= a
                        ? (double)(b - a) * period / 1e6 : 0.0;
                    f->pending.scopes[i].timing_available = true;
                }
            }
        }
    }

    g->stats = f->pending;
    f->in_flight = false;
}
static void retire(Gpu *g) {
    uint32_t i;
    VkResult r = vkGetSemaphoreCounterValue(g->device, g->timeline, &g->timeline_done);
    if (r != VK_SUCCESS) return;
    for (i = 0; i < g->frames_in_flight; ++i) {
        FrameSlot *f = &g->frames[i];
        if (f->in_flight && f->submit_value <= g->timeline_done)
            resolve_frame(g, f);
    }
    for (i = 0; i < STAGING_CHUNKS; ++i) {
        StagingChunk *c = &g->staging[i];
        if (c->in_flight && c->retire_value <= g->timeline_done) {
            c->in_flight = false;
            c->used = 0u;
            c->retire_value = 0u;
        }
    }
    {
        uint32_t write = 0;
        for (i = 0; i < g->delete_count; ++i) {
            DeleteItem *d = &g->deletes[i];
            if (d->retire_value <= g->timeline_done) delete_apply(g, d);
            else g->deletes[write++] = *d;
        }
        g->delete_count = write;
    }
}

static bool wait_oldest_frame(Gpu *g) {
    uint32_t i, best = 0;
    uint64_t value = UINT64_MAX;
    VkSemaphoreWaitInfo wi;
    for (i = 0; i < g->frames_in_flight; ++i)
        if (g->frames[i].in_flight && g->frames[i].submit_value < value) {
            value = g->frames[i].submit_value; best = i;
        }
    if (value == UINT64_MAX) return true;
    memset(&wi, 0, sizeof wi);
    wi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    wi.semaphoreCount = 1u;
    wi.pSemaphores = &g->timeline;
    wi.pValues = &value;
    /* A host wait on the timeline for frame-slot reuse. A frame that takes
     * longer than a second is slow, not lost (a software rasterizer running
     * a brute-force reference pass gets there easily): keep waiting while
     * the answer is "not yet", and give up only after a minute. */
    {
        VkResult r = VK_TIMEOUT;
        uint32_t tries;
        for (tries = 0u; tries < 60u && r == VK_TIMEOUT; ++tries)
            r = vkWaitSemaphores(g->device, &wi, 1000000000ull);
        if (r != VK_SUCCESS) {
            fail(g, r == VK_TIMEOUT ? "timeline wait timed out" : "timeline wait failed"); return false;
        }
    }
    g->timeline_done = value;
    resolve_frame(g, &g->frames[best]);
    retire(g);
    return true;
}

static void set_viewport_scissor(Gpu *g, VkCommandBuffer cmd,
                                 uint32_t width, uint32_t height) {
    VkViewport vp;
    (void)g;
    VkRect2D sc;
    /* README.md: VkViewport{.y=height,.height=-height}. Do not "fix" this by
     * editing the viewport helper — it is the coordinate convention. */
    memset(&vp, 0, sizeof vp);
    vp.x = 0.0f; vp.y = (float)height;
    vp.width = (float)width; vp.height = -(float)height;
    vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
    memset(&sc, 0, sizeof sc);
    sc.extent.width = width; sc.extent.height = height;
    vkCmdSetViewport(cmd, 0u, 1u, &vp);
    vkCmdSetScissor(cmd, 0u, 1u, &sc);
}

bool frame_begin(Gpu *g) {
    VkCommandBufferBeginInfo bi;
    FrameSlot *f;
    uint32_t i;
    VkResult r;

    if (g->frame_open) { fail(g, "frame_begin: a frame is already open"); return false; }
    retire(g);
    for (i = 0; i < g->frames_in_flight; ++i)
        if (!g->frames[i].in_flight) break;
    if (i == g->frames_in_flight) {
        if (!wait_oldest_frame(g)) return false;
        for (i = 0; i < g->frames_in_flight; ++i)
            if (!g->frames[i].in_flight) break;
        if (i == g->frames_in_flight) { fail(g, "frame_begin: no free frame slot"); return false; }
    }
    g->frame_slot = i;
    f = &g->frames[i];
    memset(&f->pending, 0, sizeof f->pending);
    vkResetCommandBuffer(f->cmd, 0);
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    r = vkBeginCommandBuffer(f->cmd, &bi);
    if (r != VK_SUCCESS) { fail(g, "vkBeginCommandBuffer: %s", result_name(r)); return false; }
    vkCmdResetQueryPool(f->cmd, f->queries, 0u,
                        2u * (MAX_PASSES + MAX_PROFILE_SCOPES));

    /* The one descriptor set, bound once per frame for graphics AND compute.
     * No other binding happens anywhere in the backend. */
    vkCmdBindDescriptorSets(f->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            g->pipe_layout, 0u, 1u, &g->set, 0u, NULL);
    vkCmdBindDescriptorSets(f->cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            g->pipe_layout, 0u, 1u, &g->set, 0u, NULL);

    /* Each frame slot gets its own disjoint transient subregion, so aliasing
     * never overlaps an in-flight frame (api-v2-design.md §2.2). */
    g->arenas[ARENA_TRANSIENT].base_offset =
        (uint64_t)i * (ARENA_TRANSIENT_BYTES / g->frames_in_flight);
    g->arenas[ARENA_TRANSIENT].used = 0u;
    /* This arena's high-water mark is per frame: it is reported as
     * FrameStats.transient_peak_bytes, and "the most transient memory any
     * frame ever used" is not a per-frame number. The device arena's peak is
     * session-wide and is deliberately not reset here. */
    g->arenas[ARENA_TRANSIENT].peak = 0u;
    /* A frame-local target is re-derived this frame, so it starts UNDEFINED
     * again; a persistent one keeps the layout the last frame left it in and
     * apply_barriers() uses that as the transition's source. Doing this here,
     * and not at plan time, is what makes one program correct for every
     * frame instead of only for the frame it was built in. */
    for (uint32_t t = 0; t < MAX_TARGETS; ++t)
        if (g->targets[t].live && g->targets[t].cls == RES_FRAME)
            g->targets[t].frame_layout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (g->swapchain) {
        uint32_t image_index = 0u;
        VkResult ar = vkAcquireNextImageKHR(g->device, g->swapchain, UINT64_MAX,
                                            f->acquire_done, VK_NULL_HANDLE,
                                            &image_index);
        /* VK_SUBOPTIMAL_KHR is a usable image that is merely the wrong size
         * for the surface: present it, and let the next resize fix it.
         * Anything else (out of date, surface lost) means there is no image,
         * so there is no frame: the caller skips it. Previously the error was
         * only recorded and the frame continued with the PREVIOUS index, which
         * renders into an image that is still owned by the presentation
         * engine. */
        if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) {
            fail(g, "acquire swapchain image: %s (recreate the swapchain with set_surface)",
                 result_name(ar));
            /* The command buffer is already recording: close it so the next
             * frame_begin() can reset it without a validation complaint. */
            vkEndCommandBuffer(f->cmd);
            g->frame_open = false;
            return false;
        }
        if (ar == VK_SUBOPTIMAL_KHR)
            snprintf(g->validation_note, sizeof g->validation_note,
                     "swapchain is suboptimal; recreate it with set_surface");
        /* The swapchain may hand back more images than frames_in_flight; the
         * view arrays are sized for the framebuffer count, so clamp to what
         * was actually created rather than index past the end. */
        if (image_index >= g->swapchain_image_count) {
            fail(g, "acquire returned image %u of %u", image_index,
                 g->swapchain_image_count);
            vkEndCommandBuffer(f->cmd);
            g->frame_open = false;
            return false;
        }
        g->swapchain_index = image_index;
        /* Slot 0 stands for the acquired image: without the VkImage here
         * the planned barriers for TARGET_SWAPCHAIN are skipped and the
         * image is rendered to and presented in UNDEFINED layout. */
        g->textures[0].image = g->swapchain_images[g->swapchain_index];
        g->textures[0].view = g->swapchain_views[g->swapchain_index];
        g->textures[0].storage_view = g->swapchain_views[g->swapchain_index];
        g->textures[0].layout = VK_IMAGE_LAYOUT_UNDEFINED;
    }

    memset(&g->active_stats, 0, sizeof g->active_stats);
    g->active_stats.cpu_ms = 0.0;
    g->compiled_this_frame = 0u;
    g->escape_uses = 0u;
    g->frame_open = true;
    g->cpu_start_ms = now_ms();
    return true;
}

/* The layout an image barrier starts from, decided when the frame is
 * RECORDED rather than when the program was built. A plan is built once and
 * replayed every frame: a persistent image is in whatever layout the previous
 * frame left it in, so the build-time oldLayout is a guess. `b->discard` marks
 * the one case where UNDEFINED is both correct and wanted (the pass overwrites
 * the whole subresource). Frame-local images are re-derived each frame, so the
 * build-time value is right for them too.
 *
 * Returns the old layout and, through the out parameters, the access and stage
 * that go with it: a barrier whose srcAccessMask names a writer that is not
 * there serialises more than the dependency needs. */
static VkImageLayout barrier_src_layout(const TargetSlot *ts, const BarrierImg *b,
                                        VkAccessFlags *src_access,
                                        VkPipelineStageFlags *src_stage) {
    VkImageLayout old;
    if (b->discard || ts->cls == RES_FRAME) {
        *src_access = b->src_access;
        *src_stage = b->src_stage;
        return b->old_layout;
    }
    old = ts->frame_layout;
    *src_access = to_vk_layout_access(old, false);
    *src_stage = old == VK_IMAGE_LAYOUT_UNDEFINED
               ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
               : to_vk_layout_stage(old);
    return old;
}

static void apply_barriers(Gpu *g, VkCommandBuffer cmd, PassPlan *pp) {
    VkBufferMemoryBarrier bb[MAX_BINDS_PER_PASS * 2u];
    VkImageMemoryBarrier ib[MAX_COLOR_ATTACHMENTS + 1u + MAX_BINDS_PER_PASS];
    VkPipelineStageFlags src_stage = 0, dst_stage = 0;
    uint32_t i, nb = 0, ni = 0;
    BarrierPlan *plan = &pp->barriers;

    for (i = 0; i < plan->buf_count; ++i) {
        BarrierBuf *b = &plan->bufs[i];
        BufferSlot *bs = &g->buffers[b->slot];
        VkBufferMemoryBarrier *v = &bb[nb];
        if (!bs->buffer) continue;
        memset(v, 0, sizeof *v);
        v->sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        v->srcAccessMask = b->src_access; v->dstAccessMask = b->dst_access;
        v->buffer = bs->buffer; v->offset = b->offset; v->size = b->size;
        v->srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        v->dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        src_stage |= b->src_stage; dst_stage |= b->dst_stage;
        ++nb;
    }
    for (i = 0; i < plan->img_count; ++i) {
        BarrierImg *b = &plan->imgs[i];
        TargetSlot *ts = &g->targets[b->slot];
        VkImageMemoryBarrier *v = &ib[ni];
        VkImageLayout old_layout;
        VkAccessFlags src_access = b->src_access;
        VkPipelineStageFlags img_src_stage = b->src_stage;
        if (!ts->tex || !ts->tex->image) continue;
        old_layout = barrier_src_layout(ts, b, &src_access, &img_src_stage);
        memset(v, 0, sizeof *v);
        v->sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        v->srcAccessMask = src_access; v->dstAccessMask = b->dst_access;
        v->oldLayout = old_layout; v->newLayout = b->new_layout;
        v->image = ts->tex->image;
        v->subresourceRange.aspectMask = to_vk_aspect(ts->format);
        v->subresourceRange.baseMipLevel = b->base_mip;
        v->subresourceRange.levelCount = b->mip_count;
        v->subresourceRange.baseArrayLayer = b->base_layer;
        v->subresourceRange.layerCount = b->layer_count;
        v->srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        v->dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        src_stage |= img_src_stage;   /* resolved at record time, not at build */
        dst_stage |= b->dst_stage;
        /* Track the layout a whole-image transition leaves behind, so a later
         * readback_request() starts from where the image really is (a target
         * that was rendered and then sampled is in SHADER_READ_ONLY, not in
         * the attachment layout recorded after its rendering pass). */
        if (b->base_mip == 0u && b->mip_count >= ts->tex->mips)
            ts->tex->layout = b->new_layout;
        ts->frame_layout = b->new_layout;
        ++ni;
    }
    if (nb == 0u && ni == 0u) return;
    if (!src_stage) src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    if (!dst_stage) dst_stage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0u, 0u, NULL, nb, bb, ni, ib);
    ++g->active_stats.barriers;
    g->active_stats.layout_transitions += ni;
}

static VkResolveModeFlagBits to_vk_resolve_mode(ResolveMode m) {
    return m == RESOLVE_SAMPLE_ZERO ? VK_RESOLVE_MODE_SAMPLE_ZERO_BIT
         : m == RESOLVE_AVERAGE     ? VK_RESOLVE_MODE_AVERAGE_BIT
                                    : VK_RESOLVE_MODE_NONE;
}
static VkAttachmentLoadOp to_vk_load_op(AttUse use) {
    if (use == ATT_CLEAR || use == ATT_DEPTH_CLEAR) return VK_ATTACHMENT_LOAD_OP_CLEAR;
    if (use == ATT_WRITE || use == ATT_DEPTH_WRITE)   return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    return VK_ATTACHMENT_LOAD_OP_LOAD;
}
static VkAttachmentStoreOp to_vk_store_op(AttUse use) {
    return (use == ATT_DEPTH_READ || use == ATT_STORAGE_READ)
         ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
}

static void begin_rendering(Gpu *g, VkCommandBuffer cmd, PassPlan *pp) {
    VkRenderingAttachmentInfo colors[MAX_COLOR_ATTACHMENTS];
    VkRenderingAttachmentInfo depth_ai;
    VkRenderingInfo ri;
    uint32_t j;
    PassDesc *d = &pp->desc;
    if (d->color_count == 0u && !pp->has_depth) return;
    memset(colors, 0, sizeof colors);
    memset(&depth_ai, 0, sizeof depth_ai);
    memset(&ri, 0, sizeof ri);
    for (j = 0; j < d->color_count; ++j) {
        AttachDesc *a = &pp->colors[j];
        TargetSlot *ts = target_slot(g, a->target);
        VkRenderingAttachmentInfo *c = &colors[j];
        c->sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        c->imageView = ts->tex->view;
        c->imageLayout = pp->color_layouts[j];
        c->resolveMode = to_vk_resolve_mode(a->resolve_mode);
        if (HANDLE_VALID(a->resolve)) {
            TargetSlot *rs = target_slot(g, a->resolve);
            if (rs) c->resolveImageView = rs->tex->view;
        }
        c->loadOp = to_vk_load_op(a->use);
        c->storeOp = to_vk_store_op(a->use);
        c->clearValue.color.float32[0] = a->clear[0];
        c->clearValue.color.float32[1] = a->clear[1];
        c->clearValue.color.float32[2] = a->clear[2];
        c->clearValue.color.float32[3] = a->clear[3];
    }
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea.extent.width = pp->width;
    ri.renderArea.extent.height = pp->height;
    ri.layerCount = 1u;
    ri.colorAttachmentCount = d->color_count;
    ri.pColorAttachments = colors;
    if (pp->has_depth) {
        AttachDesc *a = &pp->depth_att;
        TargetSlot *ts = target_slot(g, a->target);
        depth_ai.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depth_ai.imageView = ts->tex->view;
        depth_ai.imageLayout = pp->depth_layout;
        depth_ai.loadOp = to_vk_load_op(a->use);
        depth_ai.storeOp = to_vk_store_op(a->use);
        depth_ai.clearValue.depthStencil.depth = a->clear[0];
        ri.pDepthAttachment = &depth_ai;
    }
    vkCmdBeginRendering(cmd, &ri);
    set_viewport_scissor(g, cmd, pp->width, pp->height);
}

static bool is_graphics_kind(PassKind k) {
    return k == PASS_GRAPHICS;
}

/* Ends the pass frame_pass() opened. */
static void pass_close(Gpu *g) {
    PassCtx *ctx = &g->pass;
    PassPlan *pp = ctx->plan;
    FrameSlot *f = &g->frames[g->frame_slot];
    VkCommandBuffer cmd = f->cmd;
    const uint32_t i = ctx->index;
    if (ctx->scope_open) {
        ScopeStat *sc = &g->active_stats.scopes[ctx->active_scope];
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                            f->queries,
                            2u * MAX_PASSES + 2u * ctx->active_scope + 1u);
        fail(g, "pass '%s': profiling scope '%s' was not ended",
             pp->desc.name, sc->name);
        ctx->scope_open = false;
    }
    if (is_graphics_kind(pp->desc.kind) &&
        (pp->desc.color_count || pp->has_depth)) vkCmdEndRendering(cmd);
    /* Keep TextureSlot.layout in sync with the last rendering layout so
     * readback_request() transitions from the correct layout instead of
     * UNDEFINED (which would logically discard the just-rendered image). */
    if (is_graphics_kind(pp->desc.kind)) {
        for (uint32_t c = 0u; c < pp->desc.color_count; ++c) {
            TargetSlot *ts = target_slot(g, pp->colors[c].target);
            if (ts && ts->tex) ts->tex->layout = pp->color_layouts[c];
        }
        if (pp->has_depth) {
            TargetSlot *ts = target_slot(g, pp->depth_att.target);
            if (ts && ts->tex) ts->tex->layout = pp->depth_layout;
        }
    }
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                        f->queries, 2u * i + 1u);
    memcpy(&pp->stat, &g->active_stats.passes[i], sizeof(PassStat));
    g->pass_open = false;
}

/* Ends the last pass of the program being recorded and totals its stats. */
static void program_close(Gpu *g) {
    uint32_t i;
    if (g->pass_open) pass_close(g);
    g->active_stats.declared_bytes = 0u;
    for (i = 0; i < g->active_stats.pass_count; ++i)
        g->active_stats.declared_bytes = add_sat_u64(
            g->active_stats.declared_bytes,
            g->active_stats.passes[i].declared_bytes);
    g->active_stats.transient_peak_bytes = g->arenas[ARENA_TRANSIENT].peak;
    g->active_stats.escape_uses = g->escape_uses;
    g->pass_next = 0u;
}

PassCtx *frame_pass(Gpu *g, FrameProgram *p, const char *name) {
    VkCommandBuffer cmd;
    FrameSlot *f;
    PassPlan *pp;
    PassCtx *ctx = &g->pass;
    uint32_t i;
    if (!g->frame_open || !p || p->gpu != g) {
        fail(g, "frame_pass: no open frame or program belongs to another Gpu");
        return NULL;
    }
    f = &g->frames[g->frame_slot];
    cmd = f->cmd;
    if (g->pass_open) pass_close(g);
    /* another program, or this one a second time in the frame */
    if (g->pass_next && (g->active_program != p || g->pass_next >= p->count)) program_close(g);
    if (!g->pass_next) {
        g->active_program = p;
        g->active_stats.pass_count = p->count > MAX_PASSES ? MAX_PASSES : p->count;
    }
    i = g->pass_next;
    pp = &p->passes[i];
    if (name && strcmp(name, pp->desc.name) != 0) {
        fail(g, "frame_pass: '%s' asked for, but pass %u of the program is '%s'",
             name, i, pp->desc.name);
        return NULL;
    }
    memset(ctx, 0, sizeof *ctx);
    ctx->gpu = g; ctx->cmd = cmd; ctx->program = p;
    ctx->plan = pp; ctx->index = i;
    ctx->state = pp->desc.state;
    memcpy(&g->active_stats.passes[i], &pp->stat, sizeof(PassStat));
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, f->queries, 2u * i);
    apply_barriers(g, cmd, pp);
    if (is_graphics_kind(pp->desc.kind)) begin_rendering(g, cmd, pp);
    if (HANDLE_VALID(pp->desc.pipe)) {
        PipeSlot *ps = pipe_slot(g, pp->desc.pipe);
        if (ps) {
            vkCmdBindPipeline(cmd, ps->is_compute ? VK_PIPELINE_BIND_POINT_COMPUTE
                                                  : VK_PIPELINE_BIND_POINT_GRAPHICS,
                              ps->pipeline);
            ctx->pipe_bound = true;
        }
    }
    if (is_graphics_kind(pp->desc.kind)) pass_state(ctx, pp->desc.state);
    g->pass_open = true;
    g->pass_next = i + 1u;
    return ctx;
}

void frame_run(Gpu *g, FrameProgram *p, void *user) {
    uint32_t i;
    if (!g->frame_open || !p || p->gpu != g) {
        fail(g, "frame_run: no open frame or program belongs to another Gpu");
        return;
    }
    if (g->pass_open || g->pass_next) program_close(g);
    for (i = 0; i < p->count; ++i) {
        PassCtx *ctx = frame_pass(g, p, NULL);
        if (p->passes[i].desc.record) p->passes[i].desc.record(user, ctx);
    }
    program_close(g);
}

void frame_end(Gpu *g) {
    VkTimelineSemaphoreSubmitInfo tsi;
    VkSubmitInfo si;
    VkPresentInfoKHR pi;
    VkSemaphore signals[2];
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    FrameSlot *f;
    VkResult r;
    uint64_t signal_value;
    uint64_t signal_values[2];
    const uint64_t wait_value = 0u;
    uint32_t signal_count;
    if (!g->frame_open) { fail(g, "frame_end: no open frame"); return; }
    if (g->pass_open || g->pass_next) program_close(g);
    f = &g->frames[g->frame_slot];
    if (g->swapchain && g->textures[0].image) {
        /* Presentation requires PRESENT_SRC_KHR
         * (VUID-VkPresentInfoKHR-pImageIndices-01430). */
        VkImageMemoryBarrier b;
        const VkImageLayout from = g->textures[0].layout;
        memset(&b, 0, sizeof b);
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = from;
        b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        b.srcAccessMask = from == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                        ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : 0u;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = g->textures[0].image;
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.subresourceRange.levelCount = 1u;
        b.subresourceRange.layerCount = 1u;
        vkCmdPipelineBarrier(f->cmd,
                             from == VK_IMAGE_LAYOUT_UNDEFINED
                                 ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                                 : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0u,
                             0u, NULL, 0u, NULL, 1u, &b);
        g->textures[0].layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    }
    r = vkEndCommandBuffer(f->cmd);
    if (r != VK_SUCCESS) { fail(g, "vkEndCommandBuffer: %s", result_name(r)); return; }

    g->active_stats.cpu_ms = now_ms() - g->cpu_start_ms;
    f->pending = g->active_stats;
    f->pending.pass_count = g->active_stats.pass_count;
    f->pending_program = g->active_program;
    ++g->timeline_value;
    signal_value = g->timeline_value;
    f->submit_value = signal_value;
    f->in_flight = true;

    /* Value arrays match the full semaphore lists, including binary entries.
     * Values corresponding to binary semaphores are ignored by Vulkan. */
    signals[0] = g->timeline;
    signal_values[0] = signal_value;
    signal_values[1] = 0u;
    signal_count = 1u;
    if (g->swapchain) { signals[signal_count++] = f->present_done; }
    memset(&tsi, 0, sizeof tsi);
    tsi.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    tsi.signalSemaphoreValueCount = signal_count;
    tsi.pSignalSemaphoreValues = signal_values;
    if (g->swapchain) {
        tsi.waitSemaphoreValueCount = 1u;
        tsi.pWaitSemaphoreValues = &wait_value;
    }

    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.pNext = &tsi;
    si.commandBufferCount = 1u;
    si.pCommandBuffers = &f->cmd;
    si.signalSemaphoreCount = signal_count;
    si.pSignalSemaphores = signals;
    if (g->swapchain) {
        si.waitSemaphoreCount = 1u;
        si.pWaitSemaphores = &f->acquire_done;
        si.pWaitDstStageMask = &wait_stage;
    }
    r = vkQueueSubmit(g->queue, 1u, &si, VK_NULL_HANDLE);
    if (r != VK_SUCCESS) {
        fail(g, "vkQueueSubmit: %s", result_name(r));
        f->in_flight = false;
        g->frame_open = false;
        return;
    }
    if (g->swapchain) {
        memset(&pi, 0, sizeof pi);
        pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        pi.waitSemaphoreCount = 1u;
        pi.pWaitSemaphores = &f->present_done;
        pi.swapchainCount = 1u;
        pi.pSwapchains = &g->swapchain;
        pi.pImageIndices = &g->swapchain_index;
        vkQueuePresentKHR(g->queue, &pi);
    }
    g->frame_open = false;
    retire(g);
}

/* ============================================================================
 * 8. PASS RECORDING VERBS
 * ==========================================================================*/

static VkCommandBuffer pass_cmd(PassCtx *p) { return p->cmd; }

/* A draw is only legal once a pipeline is bound, and "a pipeline got bound"
 * is exactly what PassCtx.pipe_bound records — it was being set in two places
 * and read in none. The check is one load on a path that is already recorded
 * command by command; it turns a validation-layer message at draw time into
 * the reason, at the line. */
static bool draw_ready(PassCtx *p, const char *what) {
    if (!p->pipe_bound) {
        fail(p->gpu, "%s: no pipeline bound in pass '%s' (PassDesc.pipe was "
                     "PIPE_NIL and pass_bind_pipe() was never called)",
             what, p->plan->desc.name);
        return false;
    }
    return true;
}


void pass_push(PassCtx *p, const void *data, uint32_t size) {
    Gpu *g = p->gpu;
    /* PUSH() makes the payload exactly PUSH_MAX bytes, so a caller passing
     * sizeof(Push) is always legal and a caller passing something smaller
     * gets a shader reading stale constants past `size`. Push the whole
     * range from the caller's buffer, zero-filled to PUSH_MAX, so the bytes
     * a shader reads are never a previous draw's. */
    uint8_t buf[PUSH_MAX];
    if (!data || size == 0u || size > PUSH_MAX) {
        fail(g, "pass_push: size %u outside 1..%d", size, PUSH_MAX);
        return;
    }
    if (size < PUSH_MAX) {
        memset(buf, 0, sizeof buf);
        memcpy(buf, data, size);
        data = buf;
        size = PUSH_MAX;
    }
    /* stageFlags must name every stage of the layout's one push range
     * (VUID-vkCmdPushConstants-offset-01796), whatever kind the pass is. */
    vkCmdPushConstants(pass_cmd(p), g->pipe_layout,
                       VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT,
                       0u, size, data);
    ++g->active_stats.pushes;
}

void pass_bind_pipe(PassCtx *p, Pipe pipe) {
    PipeSlot *ps = pipe_slot(p->gpu, pipe);
    if (!ps) { fail(p->gpu, "pass_bind_pipe: stale pipe handle"); return; }
    vkCmdBindPipeline(pass_cmd(p), ps->is_compute ? VK_PIPELINE_BIND_POINT_COMPUTE
                                                 : VK_PIPELINE_BIND_POINT_GRAPHICS,
                      ps->pipeline);
    p->pipe_bound = true;
}

static VkColorBlendEquationEXT to_vk_blend_equation(Blend b) {
    VkColorBlendEquationEXT e;
    memset(&e, 0, sizeof e);
    switch (b) {
    case BLEND_PREMUL:
        e.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        e.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        e.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        e.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        break;
    case BLEND_ADD:
        e.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        e.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        e.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        e.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        break;
    case BLEND_ALPHA:
    default:
        e.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        e.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        e.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        e.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        break;
    }
    e.colorBlendOp = VK_BLEND_OP_ADD;
    e.alphaBlendOp = VK_BLEND_OP_ADD;
    return e;
}

void pass_state(PassCtx *p, DrawState state) {
    Gpu *g = p->gpu;
    VkCommandBuffer cmd = pass_cmd(p);
    uint32_t n_col = p->plan->desc.color_count;
    if (p->state_set && memcmp(&p->state, &state, sizeof state) == 0)
        return;   /* shadow-compare: identical state => zero emission */
    p->state = state;
    p->state_set = true;
    ++g->active_stats.state_changes;
    ++g->active_stats.state_runs;
    vkCmdSetCullMode(cmd, to_vk_cull((Cull)state.cull));
    vkCmdSetFrontFace(cmd, VK_FRONT_FACE_COUNTER_CLOCKWISE);
    vkCmdSetPrimitiveTopology(cmd, to_vk_topo((Topo)state.topo));
    vkCmdSetDepthTestEnable(cmd, state.depth_cmp != CMP_NONE ? VK_TRUE : VK_FALSE);
    vkCmdSetDepthWriteEnable(cmd, state.depth_write ? VK_TRUE : VK_FALSE);
    vkCmdSetDepthCompareOp(cmd, to_vk_compare((Compare)state.depth_cmp));
    if (g->eds3) {
        VkBool32 blends[MAX_COLOR_ATTACHMENTS];
        VkColorBlendEquationEXT eqs[MAX_COLOR_ATTACHMENTS];
        VkColorComponentFlags masks[MAX_COLOR_ATTACHMENTS];
        uint32_t i, n = n_col ? n_col : 1u;
        /* write_mask is one 4-bit RGBA mask applied to every attachment, and
         * its only two values are "all" and "none" (draw_state_depth_only).
         * The old loop read a nibble per attachment, so it looked like a
         * per-attachment partition that the struct does not have, and the
         * "all" case only worked because 0xFFFF happens to fill every nibble. */
        const VkColorComponentFlags rgba = state.write_mask
            ? (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT)
            : 0u;
        for (i = 0; i < n; ++i) {
            blends[i] = state.blend != BLEND_OPAQUE ? VK_TRUE : VK_FALSE;
            eqs[i] = to_vk_blend_equation((Blend)state.blend);
            masks[i] = rgba;
        }
        vkCmdSetRasterizationSamplesEXT(cmd, to_vk_samples((SampleCount)state.samples));
        vkCmdSetColorBlendEnableEXT(cmd, 0u, n, blends);
        vkCmdSetColorBlendEquationEXT(cmd, 0u, n, eqs);
        vkCmdSetColorWriteMaskEXT(cmd, 0u, n, masks);
    }
    /* Without EDS3 the pipeline's static blend/sample state is the fallback;
     * every other DrawState axis is core-dynamic on Vulkan 1.3. */
}

/* The indirect verbs take a range the GPU reads itself, so nothing else in the
 * API checks it: plan_barriers() only sees what a pass DECLARES, and a mistyped
 * stride is a validation message at draw time at best. Both live here because
 * both need the same three facts (the buffer, the range, the stride). */
static bool indirect_range_ok(Gpu *g, const char *what, BufferSlot *b,
                              BufSlice args, uint64_t stride, uint32_t max_draws) {
    uint64_t need;
    if ((uint64_t)args.offset % 4u != 0u) {
        fail(g, "%s: args offset %llu is not a multiple of 4", what,
             (unsigned long long)args.offset);
        return false;
    }
    if (stride % 4u != 0u) {
        fail(g, "%s: stride %llu is not a multiple of 4", what,
             (unsigned long long)stride);
        return false;
    }
    need = stride * (uint64_t)max_draws;
    if (args.offset > b->size || need > b->size - args.offset) {
        fail(g, "%s: %u commands of %llu bytes at offset %llu exceed the buffer (%llu bytes)",
             what, max_draws, (unsigned long long)stride,
             (unsigned long long)args.offset, (unsigned long long)b->size);
        return false;
    }
    return true;
}

void pass_draw_indirect(PassCtx *p, BufSlice args, uint32_t max_draws) {
    BufferSlot *b = buffer_slot(p->gpu, args.buffer);
    const uint64_t stride = sizeof(VkDrawIndirectCommand);
    if (!b) { fail(p->gpu, "pass_draw_indirect: bad args buffer"); return; }
    if (max_draws == 0u) return;   /* "no indirect draws this frame" */
    if (!draw_ready(p, "pass_draw_indirect")) return;
    if (!indirect_range_ok(p->gpu, "pass_draw_indirect", b, args, stride, max_draws))
        return;
    vkCmdDrawIndirect(pass_cmd(p), b->buffer, args.offset, max_draws,
                      (uint32_t)stride);
    /* max_draws is a ceiling: the GPU may execute fewer. Counting the ceiling
     * keeps the statistic honest about work that was POSSIBLE this frame. */
    p->gpu->active_stats.draws += max_draws;
}
void pass_draw_indirect_count(PassCtx *p, BufSlice args, BufSlice count,
                              uint32_t max_draws, uint32_t stride) {
    BufferSlot *b = buffer_slot(p->gpu, args.buffer);
    BufferSlot *c = buffer_slot(p->gpu, count.buffer);
    const uint64_t real_stride = stride ? stride : sizeof(VkDrawIndirectCommand);
    if (!b || !c) { fail(p->gpu, "pass_draw_indirect_count: bad buffer"); return; }
    if (max_draws == 0u) return;
    if (!draw_ready(p, "pass_draw_indirect_count")) return;
    if (!indirect_range_ok(p->gpu, "pass_draw_indirect_count", b, args,
                           real_stride, max_draws))
        return;
    vkCmdDrawIndirectCount(pass_cmd(p), b->buffer, args.offset,
                           c->buffer, count.offset, max_draws,
                           (uint32_t)real_stride);
    p->gpu->active_stats.draws += max_draws;
}
void pass_dispatch(PassCtx *p, uint32_t x, uint32_t y, uint32_t z) {
    /* The header says "any count 0 runs nothing, which is how a GPU-side
     * decision costs no work". vkCmdDispatch(0,0,0) is legal and does exactly
     * that; substituting 1 launched a workgroup for every caller that meant
     * "nothing". */
    if (x == 0u || y == 0u || z == 0u) return;
    if (!draw_ready(p, "pass_dispatch")) return;
    vkCmdDispatch(pass_cmd(p), x, y, z);
    ++p->gpu->active_stats.dispatches;
}
void pass_dispatch_indirect(PassCtx *p, BufSlice args) {
    BufferSlot *b = buffer_slot(p->gpu, args.buffer);
    if (!b) { fail(p->gpu, "pass_dispatch_indirect: bad args buffer"); return; }
    if (!draw_ready(p, "pass_dispatch_indirect")) return;
    vkCmdDispatchIndirect(pass_cmd(p), b->buffer, args.offset);
    ++p->gpu->active_stats.dispatches;
}
void pass_draw(PassCtx *p, uint32_t verts, uint32_t instances,
               uint32_t first_vert, uint32_t first_instance) {
    if (verts == 0u || instances == 0u) return;   /* "draw nothing" */
    if (!draw_ready(p, "pass_draw")) return;
    vkCmdDraw(pass_cmd(p), verts, instances, first_vert, first_instance);
    ++p->gpu->active_stats.draws;   /* counted: fixed-count draws are the slow path */
}

void pass_draw_fullscreen_triangle(PassCtx *p, uint32_t width, uint32_t height) {
    if (width == 0u || height == 0u) return;
    if (!draw_ready(p, "pass_draw_fullscreen_triangle")) return;
    set_viewport_scissor(p->gpu, pass_cmd(p), width, height);
    vkCmdDraw(pass_cmd(p), 3u, 1u, 0u, 0u);
    ++p->gpu->active_stats.draws;
}

void pass_note_samples(PassCtx *p, uint32_t samples, uint32_t effective_samples,
                       double steps_per_pixel) {
    PassStat *stat;
    if (!p || p->index >= MAX_PASSES) return;
    stat = &p->gpu->active_stats.passes[p->index];
    stat->samples = samples;
    stat->effective_samples = effective_samples;
    stat->steps_per_pixel = steps_per_pixel;
    p->plan->stat.samples = samples;
    p->plan->stat.effective_samples = effective_samples;
    p->plan->stat.steps_per_pixel = steps_per_pixel;
}

void pass_note_dispatch(PassCtx *p, uint32_t workgroup_x, uint32_t workgroup_y,
                        uint32_t workgroup_z, uint32_t tile_size) {
    PassStat *stat;
    if (!p || p->index >= MAX_PASSES) return;
    stat = &p->gpu->active_stats.passes[p->index];
    stat->workgroup_x = workgroup_x;
    stat->workgroup_y = workgroup_y;
    stat->workgroup_z = workgroup_z;
    stat->tile_size = tile_size;
    p->plan->stat.workgroup_x = workgroup_x;
    p->plan->stat.workgroup_y = workgroup_y;
    p->plan->stat.workgroup_z = workgroup_z;
    p->plan->stat.tile_size = tile_size;
}

bool pass_scope_begin(PassCtx *p, const char *name, ShaderBits stage) {
    Gpu *g;
    ScopeStat *scope;
    uint32_t slot;
    if (!p || !name || !name[0]) return false;
    g = p->gpu;
    if (p->scope_open) {
        fail(g, "pass '%s': profiling scopes cannot be nested", p->plan->desc.name);
        return false;
    }
    if (g->active_stats.scope_count >= MAX_PROFILE_SCOPES) {
        fail(g, "pass '%s': MAX_PROFILE_SCOPES (%u) exceeded",
             p->plan->desc.name, MAX_PROFILE_SCOPES);
        return false;
    }
    slot = g->active_stats.scope_count++;
    scope = &g->active_stats.scopes[slot];
    memset(scope, 0, sizeof *scope);
    copy_name(scope->pass_name, sizeof scope->pass_name, p->plan->desc.name);
    copy_name(scope->name, sizeof scope->name, name);
    scope->stage = stage;
    p->active_scope = slot;
    p->scope_open = true;
    vkCmdWriteTimestamp(pass_cmd(p), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        g->frames[g->frame_slot].queries,
                        2u * MAX_PASSES + 2u * slot);
    return true;
}

bool pass_scope_end(PassCtx *p) {
    ScopeStat *scope;
    if (!p || !p->scope_open) {
        if (p) fail(p->gpu, "pass '%s': profiling scope end without begin",
                    p->plan->desc.name);
        return false;
    }
    scope = &p->gpu->active_stats.scopes[p->active_scope];
    vkCmdWriteTimestamp(pass_cmd(p), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                        p->gpu->frames[p->gpu->frame_slot].queries,
                        2u * MAX_PASSES + 2u * p->active_scope + 1u);
    scope->complete = true;
    p->scope_open = false;
    return true;
}

/* ============================================================================
 * 9. UPLOADS — one batch, one submit, no wait
 * ==========================================================================*/

static bool staging_bump(Gpu *g, uint32_t chunk, uint64_t size,
                         uint64_t align, uint64_t *out_offset,
                         void **out_host) {
    StagingChunk *c = &g->staging[chunk];
    uint64_t a = align < 16u ? 16u : align_up(align, 16u);
    uint64_t off = align_up(c->used, a);
    if (off + size > STAGING_CHUNK_BYTES) {
        fail(g, "upload: staging chunk %u full", chunk); return false;
    }
    c->used = off + size;
    *out_offset = off;
    *out_host = (uint8_t *)c->mapped + off;
    return true;
}

Upload *upload_begin(Gpu *g) {
    Upload *u = NULL;
    VkCommandBufferAllocateInfo cai;
    VkCommandBufferBeginInfo bi;
    uint32_t i, chunk = UINT32_MAX;
    VkResult r;
    for (i = 0; i < SLOT_UPLOADS; ++i)
        if (!g->uploads[i].gpu) { u = &g->uploads[i]; break; }
    if (!u) { fail(g, "upload_begin: too many concurrent upload batches"); return NULL; }
    for (i = 0; i < STAGING_CHUNKS; ++i)
        if (!g->staging[i].in_flight && g->staging[i].used == 0u) { chunk = i; break; }
    if (chunk == UINT32_MAX) {
        for (i = 0; i < STAGING_CHUNKS; ++i)
            if (!g->staging[i].in_flight) {
                chunk = i; g->staging[i].used = 0u; break;
            }
    }
    if (chunk == UINT32_MAX) {
        /* The GPU still reads every chunk. Wait for the oldest upload rather
         * than fail: a caller that ignores the failure draws with stale data.
         * The cursor of the chunk we waited on MUST be reset after the wait:
         * retire() clears `used` for it, but this upload then writes into it
         * and the next upload_begin() must not treat a stale offset as live. */
        VkSemaphoreWaitInfo wi;
        uint64_t oldest = UINT64_MAX;
        for (i = 0; i < STAGING_CHUNKS; ++i)
            if (g->staging[i].retire_value < oldest) { oldest = g->staging[i].retire_value; chunk = i; }
        memset(&wi, 0, sizeof wi);
        wi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        wi.semaphoreCount = 1u;
        wi.pSemaphores = &g->timeline;
        wi.pValues = &oldest;
        if (vkWaitSemaphores(g->device, &wi, 60000000000ull) != VK_SUCCESS) {
            fail(g, "upload_begin: all staging chunks in flight"); return NULL;
        }
        retire(g);
        g->staging[chunk].used = 0u;
    }
    memset(&cai, 0, sizeof cai);
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = g->cmd_pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1u;
    r = vkAllocateCommandBuffers(g->device, &cai, &u->cmd);
    if (r != VK_SUCCESS) { fail(g, "upload command buffer: %s", result_name(r)); return NULL; }
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    r = vkBeginCommandBuffer(u->cmd, &bi);
    if (r != VK_SUCCESS) { fail(g, "upload begin: %s", result_name(r)); return NULL; }
    u->gpu = g; u->chunk = chunk; u->bytes = 0u;
    u->has_staged = false; u->cmd_begun = true;
    return u;
}

void upload_buffer(Upload *u, BufSlice dst, DataSpan src) {
    Gpu *g = u->gpu;
    BufferSlot *b;
    if (!g || !src.data || src.size == 0u) return;
    b = buffer_slot(g, dst.buffer);
    if (!b) { fail(g, "upload_buffer: bad destination handle"); return; }
    if (dst.offset + src.size > b->size) {
        fail(g, "upload_buffer: destination range out of bounds"); return;
    }
    if (src.size <= SMALL_UPLOAD && (src.size % 4u) == 0u &&
        (dst.offset % 4u) == 0u) {
        /* <= 64 KB: vkCmdUpdateBuffer touches no staging memory at all
         * (api-v2-design.md §4.2). */
        vkCmdUpdateBuffer(u->cmd, b->buffer, dst.offset, src.size, src.data);
    } else {
        uint64_t off;
        void *host;
        VkBufferCopy copy;
        if (!staging_bump(g, u->chunk, src.size, 16u, &off, &host)) return;
        memcpy(host, src.data, (size_t)src.size);
        memset(&copy, 0, sizeof copy);
        copy.srcOffset = off; copy.dstOffset = dst.offset; copy.size = src.size;
        vkCmdCopyBuffer(u->cmd, g->staging[u->chunk].buffer, b->buffer, 1u, &copy);
        u->has_staged = true;
    }
    u->bytes += src.size;
}

/* The layout a texture rests in when no pass is using it. A sampled-only
 * texture is SHADER_READ_ONLY; one that shaders also write (TGT_STORAGE) is
 * GENERAL, because that is the layout every storage-image descriptor over it
 * is written with (see write_image()). */
static VkImageLayout idle_layout_for(const TextureSlot *t) {
    return (t->usage & TEX_STORAGE) ? VK_IMAGE_LAYOUT_GENERAL
                                    : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

void upload_texture(Upload *u, Texture dst, const ImageRegion *r, DataSpan src) {
    Gpu *g = u->gpu;
    TextureSlot *t;
    VkImageMemoryBarrier before, after;
    VkBufferImageCopy copy;
    uint64_t off;
    void *host;
    uint32_t bpp;
    if (!g || !r || !src.data || src.size == 0u) return;
    t = texture_slot(g, dst);
    if (!t) { fail(g, "upload_texture: bad texture handle"); return; }
    bpp = format_bytes(t->format);
    if (bpp == 0u) { fail(g, "upload_texture: unknown format"); return; }
    /* The copy reads exactly this many bytes out of the upload span. A short
     * span is a caller bug that would otherwise read past the end of the host
     * buffer in vkCmdCopyBufferToImage. */
    {
        uint64_t depth = r->depth ? r->depth : 1u;
        uint64_t need = (uint64_t)r->width * r->height * depth * bpp;
        if (src.size < need) {
            fail(g, "upload_texture: %llu-byte span for a %llu-byte region",
                 (unsigned long long)src.size, (unsigned long long)need);
            return;
        }
        if (r->width == 0u || r->height == 0u) {
            fail(g, "upload_texture: empty region"); return;
        }
        if ((uint64_t)r->x + r->width > t->width ||
            (uint64_t)r->y + r->height > t->height ||
            (uint64_t)r->z + depth > t->depth ||
            r->mip >= t->mips || r->layer >= t->layers) {
            fail(g, "upload_texture: region outside the texture"); return;
        }
    }
    if (!staging_bump(g, u->chunk, src.size, 16u, &off, &host)) return;
    memcpy(host, src.data, (size_t)src.size);

    memset(&before, 0, sizeof before);
    before.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    /* Start from where the image really is, not from UNDEFINED. UNDEFINED is
     * the one layout that permits the GPU to discard the contents, so a second
     * upload into a region of a texture whose mip 0 is already in
     * SHADER_READ_ONLY (or a partial-region upload of a texture other regions
     * of which are live) wiped the rest of the image on tilers. The first
     * upload of a fresh image still comes from UNDEFINED, which is correct and
     * cheap there. */
    before.oldLayout = t->layout;
    before.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    before.srcAccessMask = before.oldLayout == VK_IMAGE_LAYOUT_UNDEFINED
                         ? 0u
                         : (VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                            VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    before.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    before.image = t->image;
    before.subresourceRange.aspectMask = to_vk_aspect(t->format);
    before.subresourceRange.baseMipLevel = r->mip;
    before.subresourceRange.levelCount = 1u;
    before.subresourceRange.baseArrayLayer = r->layer;
    before.subresourceRange.layerCount = 1u;
    vkCmdPipelineBarrier(u->cmd,
                         before.oldLayout == VK_IMAGE_LAYOUT_UNDEFINED
                             ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                             : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0u,
                         0u, NULL, 0u, NULL, 1u, &before);

    memset(&copy, 0, sizeof copy);
    copy.bufferOffset = off;
    copy.bufferRowLength = r->width;
    copy.bufferImageHeight = r->height;
    copy.imageSubresource.aspectMask = to_vk_aspect(t->format);
    copy.imageSubresource.mipLevel = r->mip;
    copy.imageSubresource.baseArrayLayer = r->layer;
    copy.imageSubresource.layerCount = 1u;
    copy.imageOffset.x = (int32_t)r->x;
    copy.imageOffset.y = (int32_t)r->y;
    copy.imageOffset.z = (int32_t)r->z;
    copy.imageExtent.width = r->width;
    copy.imageExtent.height = r->height;
    copy.imageExtent.depth = r->depth ? r->depth : 1u;
    vkCmdCopyBufferToImage(u->cmd, g->staging[u->chunk].buffer, t->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &copy);

    /* Back to the layout the texture uses when nobody is uploading into it.
     * A texture the shaders also write as a storage image lives in GENERAL, so
     * forcing SHADER_READ_ONLY here left the descriptor written as GENERAL
     * while the image sat in the wrong layout: a validation error and an
     * undefined read. */
    after = before;
    after.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    after.newLayout = idle_layout_for(t);
    after.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    after.dstAccessMask = after.newLayout == VK_IMAGE_LAYOUT_GENERAL
                        ? (VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT)
                        : VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(u->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u,
                         0u, NULL, 0u, NULL, 1u, &after);
    t->layout = after.newLayout;
    u->bytes += src.size;
    u->has_staged = true;
}

void upload_end(Gpu *g, Upload *u) {
    VkTimelineSemaphoreSubmitInfo tsi;
    VkSubmitInfo si;
    VkResult r;
    uint64_t value;
    DeleteItem d;
    if (!u || u->gpu != g) { fail(g, "upload_end: invalid batch"); return; }
    r = vkEndCommandBuffer(u->cmd);
    if (r != VK_SUCCESS) { fail(g, "upload_end: %s", result_name(r)); goto abandoned; }
    ++g->timeline_value;
    value = g->timeline_value;
    memset(&tsi, 0, sizeof tsi);
    tsi.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    tsi.signalSemaphoreValueCount = 1u;
    tsi.pSignalSemaphoreValues = &value;
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.pNext = &tsi;
    si.commandBufferCount = 1u;
    si.pCommandBuffers = &u->cmd;
    si.signalSemaphoreCount = 1u;
    si.pSignalSemaphores = &g->timeline;
    r = vkQueueSubmit(g->queue, 1u, &si, VK_NULL_HANDLE);
    if (r != VK_SUCCESS) {
        fail(g, "upload submit: %s", result_name(r));
        --g->timeline_value;   /* the value was never signalled */
        goto abandoned;
    }
    g->staging[u->chunk].in_flight = true;
    g->staging[u->chunk].retire_value = value;
    memset(&d, 0, sizeof d);
    d.kind = DEL_COMMAND_BUFFER;
    d.a = (uint64_t)u->cmd;
    queue_delete(g, d);
    if (g->frame_open) g->active_stats.upload_bytes += u->bytes;
    u->gpu = NULL;
    u->cmd = VK_NULL_HANDLE;
    u->cmd_begun = false;
    retire(g);
    return;

abandoned:
    /* The batch never reached the queue, so nothing will retire its command
     * buffer or release its staging chunk. Do both here: a failed upload must
     * not strand a chunk that every later upload_begin() then waits on. */
    {
        DeleteItem cb;
        memset(&cb, 0, sizeof cb);
        cb.kind = DEL_COMMAND_BUFFER;
        cb.a = (uint64_t)u->cmd;
        queue_delete(g, cb);
    }
    g->staging[u->chunk].used = 0u;
    g->staging[u->chunk].in_flight = false;
    g->staging[u->chunk].retire_value = 0u;
    u->gpu = NULL;
    u->cmd = VK_NULL_HANDLE;
    u->cmd_begun = false;
    retire(g);
}

/* ============================================================================
 * 10. READBACK
 * ==========================================================================*/

/* Both readbacks share this: claim a slot, make a host-visible buffer of
 * `size` bytes, and open a one-shot command buffer. On failure nothing is
 * left allocated. */
static Readback *readback_open(Gpu *g, uint64_t size, VkCommandBuffer *cmd) {
    Readback *rb = NULL;
    VkCommandBufferAllocateInfo cai;
    VkCommandBufferBeginInfo bi;
    VkResult r;
    uint32_t i;
    for (i = 0; i < SLOT_READBACKS; ++i)
        if (!g->readbacks[i].live) { rb = &g->readbacks[i]; break; }
    if (!rb) { fail(g, "readback: table full (%u in flight); release finished ones", SLOT_READBACKS); return NULL; }
    memset(rb, 0, sizeof *rb);
    if (!make_buffer(g, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     ARENA_STAGING, &rb->buffer, &rb->memory, &rb->mapped, NULL))
        return NULL;
    memset(&cai, 0, sizeof cai);
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = g->cmd_pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1u;
    r = vkAllocateCommandBuffers(g->device, &cai, cmd);
    if (r != VK_SUCCESS) {
        fail(g, "readback cmd: %s", result_name(r));
        vkDestroyBuffer(g->device, rb->buffer, NULL);
        vkFreeMemory(g->device, rb->memory, NULL);
        memset(rb, 0, sizeof *rb);
        return NULL;
    }
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(*cmd, &bi);
    return rb;
}

/* Submit the copy behind everything already queued and record the timeline
 * value that marks it done. `bytes` is how much readback_ready() copies out. */
static void readback_submit(Gpu *g, Readback *rb, VkCommandBuffer cmd, void *dst, uint64_t bytes) {
    VkTimelineSemaphoreSubmitInfo tsi;
    VkSubmitInfo si;
    DeleteItem d;
    uint64_t value;
    vkEndCommandBuffer(cmd);
    ++g->timeline_value;
    value = g->timeline_value;
    memset(&tsi, 0, sizeof tsi);
    tsi.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    tsi.signalSemaphoreValueCount = 1u;
    tsi.pSignalSemaphoreValues = &value;
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.pNext = &tsi;
    si.commandBufferCount = 1u;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1u;
    si.pSignalSemaphores = &g->timeline;
    vkQueueSubmit(g->queue, 1u, &si, VK_NULL_HANDLE);
    memset(&d, 0, sizeof d);
    d.kind = DEL_COMMAND_BUFFER; d.a = (uint64_t)cmd;
    queue_delete(g, d);
    rb->gpu = g;
    rb->dst = dst;
    rb->size = bytes;
    rb->ready_value = value;
    rb->live = true;
    rb->copied = false;
}

Readback *readback_request(Gpu *g, Texture src, DataSpan dst) {
    TextureSlot *t = texture_slot(g, src);
    Readback *rb;
    VkCommandBuffer cmd;
    VkImageMemoryBarrier before, after;
    VkBufferImageCopy copy;
    VkImageLayout restore;
    if (!t || !dst.data || dst.size == 0u) { fail(g, "readback_request: bad argument"); return NULL; }
    rb = readback_open(g, dst.size, &cmd);
    if (!rb) return NULL;

    memset(&before, 0, sizeof before);
    before.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    before.oldLayout = t->layout;
    before.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    /* Scope to the real producers (a compute storage write or a colour
     * attachment), never whole-memory read/write. */
    before.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT |
                           VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    before.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    before.image = t->image;
    before.subresourceRange.aspectMask = to_vk_aspect(t->format);
    before.subresourceRange.levelCount = 1u;
    before.subresourceRange.layerCount = 1u;
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0u,
                         0u, NULL, 0u, NULL, 1u, &before);
    memset(&copy, 0, sizeof copy);
    copy.bufferOffset = 0u;
    copy.imageSubresource.aspectMask = to_vk_aspect(t->format);
    copy.imageSubresource.layerCount = 1u;
    copy.imageExtent.width = t->width;
    copy.imageExtent.height = t->height;
    copy.imageExtent.depth = t->depth ? t->depth : 1u;   /* a 3D texture: every slice */
    vkCmdCopyImageToBuffer(cmd, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           rb->buffer, 1u, &copy);
    /* Put the image back where it was: the frame program's barriers are
     * planned once and assume each frame starts in the layout the previous
     * one ended in. */
    restore = before.oldLayout != VK_IMAGE_LAYOUT_UNDEFINED
            ? before.oldLayout
            : (t->usage & TEX_STORAGE)
            ? VK_IMAGE_LAYOUT_GENERAL
            : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    after = before;
    after.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    after.newLayout = restore;
    after.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    after.dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                          VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u,
                         0u, NULL, 0u, NULL, 1u, &after);
    t->layout = restore;

    readback_submit(g, rb, cmd, (void *)dst.data, dst.size);
    return rb;
}

Readback *readback_request_buffer(Gpu *g, BufSlice src, DataSpan dst) {
    BufferSlot *b = buffer_slot(g, src.buffer);
    Readback *rb;
    VkCommandBuffer cmd;
    VkBufferMemoryBarrier barrier;
    VkBufferCopy copy;
    /* Whoever may have written the range: a shader (compute simulation,
     * culling, an indirect-argument writer) or an upload. */
    const VkPipelineStageFlags shader_stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                               VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    if (!b || !dst.data || src.size == 0u) { fail(g, "readback_request_buffer: bad argument"); return NULL; }
    if (src.offset > b->size || src.size > b->size - src.offset) {
        fail(g, "readback_request_buffer: range %llu+%llu exceeds the buffer (%llu bytes)",
             (unsigned long long)src.offset, (unsigned long long)src.size, (unsigned long long)b->size);
        return NULL;
    }
    if (dst.size < src.size) {
        fail(g, "readback_request_buffer: destination holds %llu bytes, the slice is %llu",
             (unsigned long long)dst.size, (unsigned long long)src.size);
        return NULL;
    }
    rb = readback_open(g, src.size, &cmd);
    if (!rb) return NULL;

    /* Only this byte range, and only against its real producers. */
    memset(&barrier, 0, sizeof barrier);
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = b->buffer;
    barrier.offset = src.offset;
    barrier.size = src.size;
    vkCmdPipelineBarrier(cmd, shader_stages | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0u,
                         0u, NULL, 1u, &barrier, 0u, NULL);
    copy.srcOffset = src.offset;
    copy.dstOffset = 0u;
    copy.size = src.size;
    vkCmdCopyBuffer(cmd, b->buffer, rb->buffer, 1u, &copy);
    /* A buffer has no layout to restore, but the next frame's writers must
     * still wait for this read. */
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                            VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         shader_stages | VK_PIPELINE_STAGE_TRANSFER_BIT, 0u,
                         0u, NULL, 1u, &barrier, 0u, NULL);

    readback_submit(g, rb, cmd, (void *)dst.data, src.size);
    return rb;
}

bool readback_ready(Gpu *g, Readback *r, bool blocking) {
    if (!r || !r->live || r->gpu != g) return false;
    retire(g);
    if (g->timeline_done < r->ready_value) {
        VkSemaphoreWaitInfo wi;
        if (!blocking) return false;
        memset(&wi, 0, sizeof wi);
        wi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        wi.semaphoreCount = 1u;
        wi.pSemaphores = &g->timeline;
        wi.pValues = &r->ready_value;
        if (vkWaitSemaphores(g->device, &wi, 1000000000ull) != VK_SUCCESS) return false;
        g->timeline_done = r->ready_value;
        retire(g);
    }
    if (!r->copied) {
        memcpy(r->dst, r->mapped, (size_t)r->size);
        r->copied = true;
    }
    return true;
}
void readback_release(Gpu *g, Readback *r) {
    if (!r || !r->live) return;
    vkDestroyBuffer(g->device, r->buffer, NULL);
    vkFreeMemory(g->device, r->memory, NULL);
    memset(r, 0, sizeof *r);
}

/* ============================================================================
 * 11. PROFILING, DIAGNOSTICS, ESCAPE, INTEGRATION HOOKS
 * ==========================================================================*/

void perf_frame_stats(const Gpu *g, FrameStats *out) {
    if (out) *out = g->stats;
}

void perf_dump(const Gpu *g, const char *path) {
    FILE *f = path ? fopen(path, "w") : stdout;
    uint32_t i;
    if (!f) return;
    fprintf(f, "# frame: cpu %.3f ms, gpu %.3f ms, passes %u, scopes %u\n",
            g->stats.cpu_ms, g->stats.gpu_ms, g->stats.pass_count,
            g->stats.scope_count);
    fprintf(f, "# draws %u, dispatches %u, pushes %u, barriers %u\n",
            g->stats.draws, g->stats.dispatches, g->stats.pushes,
            g->stats.barriers);
    fprintf(f, "# uploads %llu B, transient peak %llu B\n",
            (unsigned long long)g->stats.upload_bytes,
            (unsigned long long)g->stats.transient_peak_bytes);
    fprintf(f, "# declared logical resource bytes (estimate): %llu B; not physical GPU traffic\n",
            (unsigned long long)g->stats.declared_bytes);
    fprintf(f, "# pass                            gpu_ms  declared_B    N N_eff steps/pixel workgroup tile\n");
    for (i = 0; i < g->stats.pass_count; ++i) {
        const PassStat *s = &g->stats.passes[i];
        fprintf(f, "%-32s %7.3f %11llu %4u %5u %11.3f %u,%u,%u %4u\n",
                s->name, s->gpu_ms, (unsigned long long)s->declared_bytes,
                s->samples, s->effective_samples, s->steps_per_pixel,
                s->workgroup_x, s->workgroup_y, s->workgroup_z, s->tile_size);
    }
    if (g->stats.scope_count) {
        fprintf(f, "# scope                            pass                            gpu_ms stage paired resolved\n");
        for (i = 0; i < g->stats.scope_count; ++i) {
            const ScopeStat *s = &g->stats.scopes[i];
            fprintf(f, "%-32s %-32s %7.3f 0x%02x %s %s\n",
                    s->name, s->pass_name, s->gpu_ms, (unsigned)s->stage,
                    s->complete ? "yes" : "no",
                    s->timing_available ? "yes" : "no");
        }
    }
    if (f != stdout) fclose(f);
}

const char *gpu_last_error(const Gpu *g) {
    return g && g->error[0] ? g->error : "no error";
}

bool gpu_validate(const Gpu *g) {
    uint32_t i;
    MU_STATIC_ASSERT(sizeof(DrawState) == 8u,
                     "DrawState is the sort key; repacking it must not grow it");
    MU_STATIC_ASSERT(PUSH_MAX % PUSH_ALIGN == 0u, "PUSH_MAX must be a multiple of PUSH_ALIGN");
    if (!g) return false;
    if (!g->set || !g->pipe_layout || !g->timeline) return false;
    /* Not a range check: every slot here is an array index, so a live count
     * can never exceed the table it is counted in. The invariant worth
     * checking is that no slot is live with a null pipeline, which is what a
     * half-applied creation would leave behind. */
    for (i = 1u; i < MAX_PIPES; ++i)
        if (g->pipes[i].live && !g->pipes[i].pipeline) return false;
    for (i = 1u; i < SLOT_TEXTURES; ++i)
        if (g->textures[i].live && g->textures[i].bindless_index >= g->active_bindless)
            return false;
    if (g->frame_open && g->active_stats.pass_count > MAX_PASSES) return false;
    return true;
}


#ifdef MU_ESCAPE
VkDevice gpu_raw_device(Gpu *g) {
    ++g->escape_uses;
    if (g->frame_open) ++g->active_stats.escape_uses;
    fprintf(stderr, "[mu_vk] escape: gpu_raw_device() used (API gap)\n");
    if (!g->enable_escape) {
        fail(g, "gpu_raw_device: escape disabled in GpuDesc");
        return VK_NULL_HANDLE;
    }
    return g->device;
}
VkCommandBuffer pass_raw_cmd(PassCtx *p) {
    ++p->gpu->escape_uses;
    if (p->gpu->frame_open) ++p->gpu->active_stats.escape_uses;
    fprintf(stderr, "[mu_vk] escape: pass_raw_cmd() used (API gap)\n");
    if (!p->gpu->enable_escape) {
        fail(p->gpu, "pass_raw_cmd: escape disabled in GpuDesc");
        return VK_NULL_HANDLE;
    }
    return p->cmd;
}
#endif

/* ---- WSI integration -----------------------------------------------------
 * The public API has no surface verb. The application owns the Vulkan surface
 * and hands it in once; the backend creates the swapchain and presents from
 * frame_end(). Without it, TARGET_SWAPCHAIN is the internal colour image.
 * ----------------------------------------------------------------------- */
VkInstance gpu_raw_instance(const Gpu *g) {
    return g ? g->instance : VK_NULL_HANDLE;
}

bool set_surface(Gpu *g, VkSurfaceKHR surface) {
    VkSurfaceCapabilitiesKHR caps;
    VkSurfaceFormatKHR *formats = NULL;
    VkPresentModeKHR *modes = NULL;
    VkSwapchainCreateInfoKHR sci;
    VkImage *images = NULL;
    uint32_t fmt_count = 0, mode_count = 0, image_count = 0, i;
    VkPresentModeKHR chosen_mode = VK_PRESENT_MODE_FIFO_KHR;
    VkFormat chosen_fmt = VK_FORMAT_B8G8R8A8_SRGB;
    VkColorSpaceKHR chosen_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkBool32 supported = VK_FALSE;
    VkResult r;

    if (!surface) { fail(g, "set_surface: null surface"); return false; }
    vkGetPhysicalDeviceSurfaceSupportKHR(g->pdev, g->queue_family, surface, &supported);
    if (!supported) { fail(g, "set_surface: queue family cannot present"); return false; }
    r = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g->pdev, surface, &caps);
    if (r != VK_SUCCESS) { fail(g, "surface caps: %s", result_name(r)); return false; }
    vkGetPhysicalDeviceSurfaceFormatsKHR(g->pdev, surface, &fmt_count, NULL);
    if (fmt_count) {
        formats = (VkSurfaceFormatKHR *)calloc(fmt_count, sizeof *formats);
        vkGetPhysicalDeviceSurfaceFormatsKHR(g->pdev, surface, &fmt_count, formats);
        for (i = 0; i < fmt_count; ++i) {
            if (formats[i].format == VK_FORMAT_B8G8R8A8_SRGB ||
                formats[i].format == VK_FORMAT_R8G8B8A8_SRGB) {
                chosen_fmt = formats[i].format;
                chosen_space = formats[i].colorSpace;
                break;
            }
        }
        free(formats);
    }
    vkGetPhysicalDeviceSurfacePresentModesKHR(g->pdev, surface, &mode_count, NULL);
    if (mode_count) {
        modes = (VkPresentModeKHR *)calloc(mode_count, sizeof *modes);
        vkGetPhysicalDeviceSurfacePresentModesKHR(g->pdev, surface, &mode_count, modes);
        chosen_mode = VK_PRESENT_MODE_FIFO_KHR;
        if (!g->vsync)
            for (i = 0; i < mode_count; ++i)
                if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR ||
                    modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) { chosen_mode = modes[i]; break; }
        free(modes);
    }
    g->extent_w = caps.currentExtent.width ? caps.currentExtent.width : g->extent_w;
    g->extent_h = caps.currentExtent.height ? caps.currentExtent.height : g->extent_h;
    image_count = g->frames_in_flight;
    if (caps.minImageCount > image_count) image_count = caps.minImageCount;
    if (caps.maxImageCount && image_count > caps.maxImageCount) image_count = caps.maxImageCount;
    if (image_count > MAX_FRAMES_IN_FLIGHT) image_count = MAX_FRAMES_IN_FLIGHT;

    memset(&sci, 0, sizeof sci);
    sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    sci.surface = surface;
    sci.minImageCount = image_count;
    sci.imageFormat = chosen_fmt;
    sci.imageColorSpace = chosen_space;
    sci.imageExtent.width = g->extent_w;
    sci.imageExtent.height = g->extent_h;
    sci.imageArrayLayers = 1u;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = chosen_mode;
    sci.clipped = VK_TRUE;
    r = vkCreateSwapchainKHR(g->device, &sci, NULL, &g->swapchain);
    if (r != VK_SUCCESS) { fail(g, "swapchain: %s", result_name(r)); return false; }
    g->surface = surface;
    /* The driver may hand back more images than there are frame slots, and
     * the count it reports on the second call is not bounded by the first.
     * swapchain_images[]/swapchain_views[] hold MAX_FRAMES_IN_FLIGHT entries,
     * so clamp against the array and refuse a swapchain that does not fit
     * rather than writing past the end of the table. */
    image_count = 0u;
    r = vkGetSwapchainImagesKHR(g->device, g->swapchain, &image_count, NULL);
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) {
        fail(g, "swapchain image count: %s", result_name(r)); return false;
    }
    if (image_count == 0u || image_count > MAX_FRAMES_IN_FLIGHT) {
        fail(g, "swapchain has %u images, this backend supports 1..%u",
             image_count, (unsigned)MAX_FRAMES_IN_FLIGHT);
        return false;
    }
    images = (VkImage *)calloc(image_count, sizeof *images);
    if (!images) { fail(g, "out of memory"); return false; }
    r = vkGetSwapchainImagesKHR(g->device, g->swapchain, &image_count, images);
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) {
        free(images);
        fail(g, "swapchain images: %s", result_name(r)); return false;
    }
    if (image_count > MAX_FRAMES_IN_FLIGHT) {
        free(images);
        fail(g, "swapchain image count changed between queries"); return false;
    }
    g->swapchain_image_count = image_count;
    for (i = 0; i < image_count; ++i) {
        g->swapchain_images[i] = images[i];
        if (!make_view(g, images[i], chosen_fmt, VK_IMAGE_ASPECT_COLOR_BIT,
                       1u, 1u, 1u, &g->swapchain_views[i])) { free(images); return false; }
    }
    free(images);
    /* Retire the headless image; the swapchain views now own slot 0. */
    vkDestroyImageView(g->device, g->textures[0].storage_view, NULL);
    vkDestroyImageView(g->device, g->textures[0].view, NULL);
    vkDestroyImage(g->device, g->textures[0].image, NULL);
    vkFreeMemory(g->device, g->textures[0].memory, NULL);
    memset(&g->textures[0].image, 0, sizeof g->textures[0].image);
    memset(&g->textures[0].memory, 0, sizeof g->textures[0].memory);
    memset(&g->textures[0].view, 0, sizeof g->textures[0].view);
    memset(&g->textures[0].storage_view, 0, sizeof g->textures[0].storage_view);
    g->textures[0].width = g->extent_w;
    g->textures[0].height = g->extent_h;
    g->targets[0].width = g->extent_w;
    g->targets[0].height = g->extent_h;
    g->swapchain_srgb = chosen_fmt == VK_FORMAT_B8G8R8A8_SRGB ||
                        chosen_fmt == VK_FORMAT_R8G8B8A8_SRGB;
    if (chosen_fmt == VK_FORMAT_B8G8R8A8_SRGB || chosen_fmt == VK_FORMAT_B8G8R8A8_UNORM)
        g->swapchain_format = g->swapchain_srgb ? FMT_BGRA8_SRGB : FMT_BGRA8_UNORM;
    else
        g->swapchain_format = g->swapchain_srgb ? FMT_RGBA8_SRGB : FMT_RGBA8_UNORM;
    return true;
}

Texture target_texture(const Gpu *g, Target target) {
    const TargetSlot *ts = target_slot(g, target);
    if (!ts || !ts->tex) return TEXTURE_NIL;
    return (Texture){(uint32_t)(ts->tex - g->textures), ts->tex->gen};
}
