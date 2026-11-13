/* vk.h — single public C API for the clean mu_gfx Vulkan library.
 *
 * The public surface uses opaque GPU/resource handles and fixed-width values.
 * VkInstance and VkSurfaceKHR are exposed only so an application can attach a
 * WSI surface to the instance owned by Gpu. Vulkan dispatch remains private
 * to vk.c and uses Volk; no Vulkan loader symbols are linked by this library.
 *
 * Resource tables use generation-checked handles. Bindless descriptor slots
 * are allocated through the mu_id_pool submodule primitive and are not reused
 * until their deferred GPU retirement point. The frame API owns submission and
 * transient lifetimes; the public API does not expose command queues or
 * per-resource Vulkan allocation.
 *
 * Numbered section banners are indexed by scripts/section_index.py.
 */

#ifndef MU_VK_H
#define MU_VK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h> /* the inline helpers below copy names; do not rely on mu_macros */
#include "../third_party/mu_lib/mu/mu_common.h"
#include <vulkan/vulkan_core.h> /* Vk types for WSI integration and optional escape */

#ifndef ALIGNAS
#define ALIGNAS(x) __attribute__((aligned(x)))
#endif

/* ============================================================================
 * 1. CONFIG
 *    These capacities size internal tables; exhaustion is reported, not grown.
 *    Every one is a compile-time budget rather than a growth policy: a table
 *    that fills reports it through gpu_last_error() and the call returns a
 *    nil handle.
 *    MAX_PIPES is the count of LIVE interned pipelines; every program built
 *    over a scene keeps its own, so the number has to cover the whole session.
 * ==========================================================================*/

#define MAX_COLOR_ATTACHMENTS 8  /* MRT cap; each one is bytes/pixel written */
#define MAX_BINDS_PER_PASS    16 /* sampled/storage bindings per pass, checked at build */
/* Interned pipelines are permanent: the table never compacts, so this is the
 * whole budget for a session. One PSO is on the order of 100 KB of driver
 * memory, so 256 leaves room for a few hundred material variants and refuses
 * the thousand-pipeline design loudly instead of silently. */
#define MAX_PIPES             256
/* Compiling more than this many pipelines while a frame is open is treated as
 * a bug (see intern_key): a hitch, not a feature. */
#define MAX_PIPES_IN_FRAME    4
#define MAX_PASSES            64 /* a frame program is a fixed list */
#define MAX_PROFILE_SCOPES    64 /* named timestamp scopes per frame */
#define MAX_FRAMES_IN_FLIGHT  3  /* upper bound; GpuDesc selects the active count */
#define PUSH_MAX              256 /* public push-constant payload limit */
#define BINDLESS_TEXTURES     65536 /* the SHADER-side array size; the C side is
                                     * min(this, device limits), see
                                     * GpuTables.sampled_image_count */
#define BINDLESS_SAMPLERS     256
#define MAX_TARGETS           512
#define MAX_MIPS              13
#define ALIGN_DEVICE          1     /* raised to the device's queried buffer alignment */
#define MAX_PIPE_NAME         48
#define MAX_PASS_NAME         48

/* The header's own width assumptions. A handle is the identity of a resource in
 * every table; ALIGN_DEVICE and PUSH_ALIGN are used in pointer arithmetic. */
MU_STATIC_ASSERT(MAX_FRAMES_IN_FLIGHT <= 8, "frame slots are indexed by small ints");
MU_STATIC_ASSERT(MAX_PASSES <= 64, "pass indices pack into FrameStats");
MU_STATIC_ASSERT(MAX_PIPES > MAX_PASSES, "a program with one pipe per pass must fit the pipe table");

/* Push constants are bounded by Vulkan and padded to this alignment. */
#define PUSH_ALIGN           16     /* payload structure alignment */

/* ============================================================================
 * 2. HANDLES
 *    Eight bytes, passed by value, generation-checked. A stale handle is
 *    detected instead of aliasing a recycled slot — the bug class the old
 *    RenderTargetId comment warned about (vk.h:444). Debug builds validate on
 *    every dereference; release builds cost two loads.
 * ==========================================================================*/

typedef struct Buffer  { uint32_t index, gen; } Buffer;
typedef struct Texture { uint32_t index, gen; } Texture;
typedef struct Target  { uint32_t index, gen; } Target;
typedef struct Pipe    { uint32_t index, gen; } Pipe;
typedef struct Sampler { uint32_t index, gen; } Sampler;

#define HANDLE_NIL(T) ((T){0u, 0u})
#define BUFFER_NIL        HANDLE_NIL(Buffer)
#define TEXTURE_NIL       HANDLE_NIL(Texture)
#define TARGET_NIL        HANDLE_NIL(Target)
#define PIPE_NIL          HANDLE_NIL(Pipe)
#define SAMPLER_NIL       HANDLE_NIL(Sampler)

#define HANDLE_VALID(h) ((h).index != 0u)

/* "index is not the nil sentinel" is only the cheap half of the test: a stale
 * handle still has an index, and the swapchain pseudo-target has no
 * generation at all. Code that decides on HANDLE_VALID() alone therefore
 * accepts garbage. These two name the two real questions, so a call site says
 * which one it means. */
#define TARGET_IS_SWAPCHAIN(t) ((t).index == 0xFFFFFFFFu)
#define HANDLE_MATCHES(h, gen_) (((h).index != 0u) && ((h).index != 0xFFFFFFFFu) && \
                                 ((h).gen == (gen_)))

/* The acquired swapchain image used as a pass attachment. Not a real pool
 * entry: the backend supplies the image view and the present transition. */
#define TARGET_SWAPCHAIN ((Target){0xFFFFFFFFu, 0u})

/* A compiled shader module, one per .slang file. Generated by the shader build
 * step; the public surface knows shaders by id, not by path. 0 = none. */
typedef uint16_t ShaderId;
#define SHADER_NONE ((ShaderId)0)

/* Opaque owners. The caller never sizes or inspects these. */
typedef struct Gpu          Gpu;
typedef struct Upload       Upload;
typedef struct Readback     Readback;
typedef struct FrameProgram FrameProgram;
typedef struct PipeTable    PipeTable;
typedef struct PassCtx      PassCtx;
typedef struct PassDesc     PassDesc;

/* ============================================================================
 * 3. VOCABULARY
 *    Own enums, own names. The Vulkan spellings stay inside vk.c, which is the
 *    boundary these names exist to keep clean.
 * ==========================================================================*/

/* ---- formats ------------------------------------------------------------ */
typedef enum Format {
    FMT_NONE = 0,
    /* colour */
    FMT_RGBA8_UNORM, FMT_RGBA8_SRGB, FMT_BGRA8_UNORM, FMT_BGRA8_SRGB,
    FMT_RGBA16_UNORM, FMT_RGBA16F, FMT_RGBA32F,
    FMT_RG16F, FMT_R32F, FMT_R16F, FMT_R8_UNORM, FMT_RG8_UNORM,
    FMT_RGB10A2_UNORM, FMT_R11G11B10F,
    /* depth */
    FMT_D16_UNORM, FMT_D24S8, FMT_D32F,
    /* integer */
    FMT_R32_UINT, FMT_RGBA8_UINT,
    /* Appended (not inserted) so existing Format values stay stable. */
    FMT_RGBA8_SNORM,
    FMT_COUNT,
} Format;

/* ---- samples ------------------------------------------------------------ */
typedef enum SampleCount { SC_1 = 1, SC_2 = 2, SC_4 = 4, SC_8 = 8 } SampleCount;

/* ---- pipeline state ----------------------------------------------------- */
typedef enum Cull    { CULL_NONE = 0, CULL_FRONT, CULL_BACK } Cull;
typedef enum Compare { CMP_NONE = 0, CMP_NEVER, CMP_LESS, CMP_EQUAL, CMP_LEQUAL,
                       CMP_GREATER, CMP_GEQUAL, CMP_ALWAYS } Compare;
typedef enum Blend   { BLEND_OPAQUE = 0, BLEND_ALPHA, BLEND_PREMUL, BLEND_ADD } Blend;
typedef enum Topo    { TOPO_TRI = 0, TOPO_TRI_STRIP, TOPO_LINE, TOPO_POINT } Topo;
typedef enum ResolveMode { RESOLVE_NONE = 0, RESOLVE_AVERAGE, RESOLVE_SAMPLE_ZERO } ResolveMode;
typedef enum Filter  { FILT_NEAREST = 0, FILT_LINEAR } Filter;
typedef enum Address { ADDR_REPEAT = 0, ADDR_CLAMP_EDGE, ADDR_CLAMP_BORDER, ADDR_MIRROR } Address;

/* ---- shader stage bits (for scope + read-visibility hints) -------------- */
typedef enum ShaderBits {
    SH_NONE  = 0,
    SH_VERT  = 1u << 0,
    SH_FRAG  = 1u << 1,
    SH_COMP  = 1u << 2,
    SH_TASK  = 1u << 3,
    SH_MESH  = 1u << 4,
    SH_ALL   = SH_VERT | SH_FRAG | SH_COMP | SH_TASK | SH_MESH,
} ShaderBits;

/* ---- access intent ------------------------------------------------------
 * NOT Vulkan access bits. These say what the pass *does* with the range, and
 * the backend derives stage masks, source/destination access and the barrier
 * scope from that. There is deliberately no way to name "all commands" or
 * "memory read/write": those are how a barrier becomes wider than the
 * dependency it serves.
 *
 *   ACC_ATOMIC  — atomics bypass L1; the range is a write, never a read.
 *   ACC_INDIRECT— the consumer is the command processor, not a shader stage.
 * ----------------------------------------------------------------------- */
typedef enum Access2 {
    ACC_NONE      = 0,
    ACC_READ      = 1u << 0,  /* shader read (sampled / SSBO load)          */
    ACC_WRITE     = 1u << 1,  /* shader write (storage store)               */
    ACC_ATOMIC    = 1u << 2,  /* atomic RMW; implies write visibility       */
    ACC_INDIRECT  = 1u << 3,  /* indirect draw/dispatch arguments           */
    ACC_VERTEX    = 1u << 4,  /* vertex buffer fetch                        */
    ACC_INDEX     = 1u << 5,  /* index buffer fetch                         */
    ACC_UBO       = 1u << 6,  /* uniform/storage-buffer uniform read        */
    ACC_TRANSFER  = 1u << 7,  /* copy source / destination                  */
} Access2;

/* ---- pass kind ----------------------------------------------------------
 * Selects the default shader stage and barrier scope when declarations omit
 * an explicit stage mask.
 * ----------------------------------------------------------------------- */
typedef enum PassKind {
    PASS_COMPUTE = 0,
    PASS_GRAPHICS,
} PassKind;

/* ---- attachment intent --------------------------------------------------
 * The caller states what the pass does with the attachment. The backend picks
 * the layout, the load op and the store op:
 *
 *   WRITE       DONT_CARE + COLOR_ATTACHMENT_OPTIMAL
 *               fully overwritten → no fetch of last frame's contents.
 *               At 1440p RGBA16F that is a 29.5 MB read you do not pay (45 us).
 *   WRITE_KEEP  LOAD + COLOR_ATTACHMENT_OPTIMAL  (partially written)
 *   CLEAR       CLEAR + COLOR_ATTACHMENT_OPTIMAL (fast-clear eligible: 02 §2.1)
 *   STORAGE_RW  storage image read and write in one pass
 *   STORAGE_READ read-only storage image access
 *   DEPTH_WRITE / DEPTH_CLEAR / DEPTH_READ — as named.
 * ----------------------------------------------------------------------- */
typedef enum AttUse {
    ATT_WRITE = 0,
    ATT_WRITE_KEEP,
    ATT_CLEAR,
    ATT_DEPTH_WRITE,
    ATT_DEPTH_CLEAR,
    ATT_DEPTH_READ,
    ATT_STORAGE_RW,
    /* Appended. Lets a read-only storage-image use select a read-only layout
     * instead of the read/write layout required by ATT_STORAGE_RW. */
    ATT_STORAGE_READ,
} AttUse;

/* ---- resource class -----------------------------------------------------
 * RES_FRAME is the new one: born and killed inside one frame, so the frame
 * program may alias its memory with any other RES_FRAME resource whose
 * lifetime does not overlap. This reduces transient memory requirements; it
 * does not change the amount of resource data a pass reads or writes.
 * ----------------------------------------------------------------------- */
typedef enum ResClass { RES_PERSISTENT = 0, RES_FRAME, RES_READBACK } ResClass;

/* ---- usage bits --------------------------------------------------------- */
typedef enum BufferUsage {
    BUF_DEVICE  = 1u << 0, /* shader-visible storage buffer                 */
    BUF_INDIRECT= 1u << 1,
    BUF_VERTEX  = 1u << 2,
    BUF_INDEX   = 1u << 3,
    BUF_XFER_SRC= 1u << 4,
    BUF_XFER_DST= 1u << 5,
} BufferUsage;

typedef enum TextureUsage {
    TEX_SAMPLED = 1u << 0,
    TEX_STORAGE = 1u << 1,
    TEX_COLOR   = 1u << 2,
    TEX_DEPTH   = 1u << 3,
    TEX_XFER_SRC= 1u << 4,
    TEX_XFER_DST= 1u << 5,
} TextureUsage;

typedef enum TargetUsage {
    TGT_COLOR   = 1u << 0,
    TGT_DEPTH   = 1u << 1,
    TGT_SAMPLED = 1u << 2,
    TGT_STORAGE = 1u << 3,
    TGT_RESOLVE = 1u << 4,
} TargetUsage;

/* ============================================================================
 * 4. SPANS, SLICES, USES
 * ==========================================================================*/

typedef struct DataSpan { const void *data; uint64_t size; } DataSpan;
#define DATA_SPAN(p, n) ((DataSpan){(p), (uint64_t)(n)})
#define DATA_SPAN_NIL   ((DataSpan){NULL, 0})

typedef struct ImageRegion {
    uint32_t mip; uint32_t layer;
    uint32_t x, y, z;
    uint32_t width, height, depth;
} ImageRegion;

/* A byte range inside one buffer. Slices make resource use and copies
 * explicit without creating a descriptor for each range. */
typedef struct BufSlice { Buffer buffer; uint64_t offset; uint64_t size; } BufSlice;
#define BUF_SLICE_NIL ((BufSlice){BUFFER_NIL, 0u, 0u})

/* One dependency declaration. `size == 0` is rejected at frame build: the fast
 * path has no "whole buffer" shorthand, because a whole-buffer dependency is
 * how a byte-range barrier silently becomes a pool-wide one. */
typedef struct BufUse {
    BufSlice   slice;
    Access2    access;
    ShaderBits stages; /* 0 = derived from pass kind (fragment for graphics,
                          compute for compute). Set only to narrow/widen. */
} BufUse;

/* ============================================================================
 * 5. LIFECYCLE
 * ==========================================================================*/

typedef struct GpuDesc {
    bool     use_wayland;
    bool     vsync;
    bool     validation;      /* debug builds: layer + syncval, asserts on     */
    bool     enable_escape;   /* expose gpu_raw_device(); counted when used    */
    uint32_t width, height;   /* initial extent hint                           */
    const char *app_name;
    uint32_t    frames_in_flight; /* 0 = MAX_FRAMES_IN_FLIGHT              */
} GpuDesc;

Gpu *gpu_create(const GpuDesc *desc);
void gpu_destroy(Gpu *g);

/* Everything the frame layer needs to know about the surface. */
typedef struct GpuInfo {
    uint32_t   extent_w, extent_h;
    Format     swapchain_format;
    bool       swapchain_srgb;
    SampleCount max_samples;
    uint64_t   device_memory;
    uint64_t   visible_device_memory;
    uint32_t   frames_in_flight;
    char       device_name[64];
    uint32_t   subgroup_size;
    uint32_t   max_compute_workgroup_invocations;
    uint32_t   max_compute_shared_memory_size;
    uint32_t   non_coherent_atom_size;
    uint32_t   min_storage_buffer_offset_alignment;
    uint64_t   max_uniform_buffer_range;
    uint64_t   max_storage_buffer_range;
    bool       shader_float16;
    bool       storage_buffer_16bit;
    bool       storage_push_constant16;
    bool       storage_input_output16;
    bool       host_visible_device_local;
} GpuInfo;

void gpu_info(const Gpu *g, GpuInfo *out);

/* ============================================================================
 * 6. RESOURCES
 *    Created outside frames. Every create may upload; uploads batch (see §10),
 *    so N creates are not N queue stalls.
 * ==========================================================================*/

typedef struct BufferDesc {
    const char *name;
    uint64_t    size;
    BufferUsage usage;
    ResClass    cls;      /* RES_PERSISTENT for anything the GPU reads later */
} BufferDesc;

typedef struct TextureDesc {
    const char   *name;
    uint32_t      width, height, depth, layers, mips;
    Format        format;
    TextureUsage  usage;
    ResClass      cls;
    const DataSpan *initial_data; /* optional; batches into one upload      */
} TextureDesc;

/* depth > 1 makes a volume: a 3D image that compute passes write (TGT_STORAGE)
 * and any stage samples with trilinear filtering. It cannot be an attachment.
 * It takes one slot in the same bindless tables as 2D images; the shader
 * declares a Texture3D / RWTexture3D array at the same binding and indexes
 * that one instead (see demos/common/shaders/mu_volume.slang). */
typedef struct TargetDesc {
    const char   *name;
    uint32_t      width, height;
    uint32_t      depth;       /* 0 = 1 (a 2D target)                           */
    Format        format;
    TargetUsage   usage;
    uint32_t      mips;        /* 0 = 1                                         */
    SampleCount   samples;
    ResClass      cls;         /* RES_FRAME lets the program alias it           */
} TargetDesc;

typedef struct SamplerDesc {
    Filter  min_filter, mag_filter, mip_filter;
    Address addr_u, addr_v, addr_w;
    float   max_lod, lod_bias, min_lod;
    uint32_t max_aniso;  /* 0/1 = off */
    Compare  compare;    /* CMP_NONE = regular sampler                     */
} SamplerDesc;

Buffer  buffer_create(Gpu *g, const BufferDesc *desc);
Texture texture_create(Gpu *g, const TextureDesc *desc);
Target  target_create(Gpu *g, const TargetDesc *desc);
Sampler sampler_create(Gpu *g, const SamplerDesc *desc);

void buffer_destroy(Gpu *g, Buffer b);
void texture_destroy(Gpu *g, Texture t);
void target_destroy(Gpu *g, Target t);
void sampler_destroy(Gpu *g, Sampler s);

/* Introspection for the few places that genuinely need the GPU-side identity:
 * the device address of a storage slice, and the bindless indices the shaders
 * address. Everything else stays opaque. */
typedef struct BufferInfo {
    uint64_t device_address;   /* for buffer_reference in Slang             */
    uint64_t size;
    void    *mapped;           /* non-NULL for host-visible slices          */
} BufferInfo;

bool buffer_info(const Gpu *g, Buffer b, BufferInfo *out);
bool texture_info(const Gpu *g, Texture t, uint32_t *bindless_index);

/* The device-side tables a shader sees. Bindless indices are stable for the
 * resource's lifetime; a texture's index does not move when another is
 * destroyed. */
typedef struct GpuTables {
    BufSlice storage_buffers;   /* one big device buffer, addressed by offset */
    uint32_t sampled_image_base;
    uint32_t sampled_image_count;
    uint32_t sampler_base;
    uint32_t sampler_count;
} GpuTables;

void gpu_tables(const Gpu *g, GpuTables *out);

/* ============================================================================
 * 7. ARENAS
 *    Three arenas. There is no vkAllocateMemory on this surface, and no
 *    per-object memory type to get wrong.
 *      ARENA_DEVICE    persistent, device-local
 *      ARENA_TRANSIENT RES_FRAME scratch and targets; reset per frame
 *      ARENA_STAGING   upload ring; retired by timeline value, waited on only
 *                      when all of its chunks are still in flight
 * ==========================================================================*/

typedef enum Arena { ARENA_DEVICE = 0, ARENA_TRANSIENT, ARENA_STAGING } Arena;

/* align is rounded up to 4 for DEVICE (hardware floor) and 64 for anything
 * used as a storage image. Returns BUF_SLICE_NIL when the arena is full — the
 * caller decides whether that is fatal, because a transient running out is a
 * budget question, not an API error. */
BufSlice arena_alloc(Gpu *g, Arena arena, uint64_t size, uint64_t align);

typedef struct ArenaStats {
    uint64_t reserved_bytes, used_bytes, peak_bytes;
    uint32_t allocations;
} ArenaStats;

void arena_stats(const Gpu *g, Arena arena, ArenaStats *out);

/* ============================================================================
 * 8. PIPELINES
 *    Pipeline objects are interned by shader stages and attachment formats.
 *    Frequently changing raster/depth/blend state is supplied separately.
 * ==========================================================================*/

typedef struct PipeKey {
    ShaderId  vs, fs;                  /* 0 = stage absent (depth-only pipe)  */
    const char *vs_entry, *fs_entry;   /* NULL = "main"                       */
    Format    color[MAX_COLOR_ATTACHMENTS];
    uint8_t   color_count;
    Format    depth;                   /* FMT_NONE = no depth attachment      */
    uint16_t  view_mask;               /* multiview; 0 = off                  */

    /* Static fallback used when extended dynamic blend state is unavailable. */
    Blend blend_fallback;
    /* Optional pipeline specialization for an always-opaque class. */
    bool blend_static_off;

    /* Depth state. depthTestEnable / depthWriteEnable / depthCompareOp ARE
     * dynamic in Vulkan 1.3, but four pipelines cannot differ only in them:
     * VK_EXT_extended_dynamic_state3 does not cover depth write, and a
     * read-only depth attachment (ATT_DEPTH_READ) requires depthWriteEnable =
     * VK_FALSE at pipeline creation to be legal at all. Two pipelines that
     * differ only in these fields are therefore distinct pipeline objects,
     * and the key has to say so or the second one is silently reused. */
    Compare   depth_cmp;               /* CMP_NONE = depth test off at creation */
    bool      depth_write;
} PipeKey;

/* Interned. Same key -> same Pipe, no matter how many passes ask for it. */
Pipe        pipe_intern(Gpu *g, const PipeKey *key);
Pipe        pipe_find(const Gpu *g, const PipeKey *key); /* no creation */
uint32_t    pipe_count(const Gpu *g);
void        pipe_stats(const Gpu *g, uint32_t *pipelines, uint32_t *compiled_this_frame);

/* Common case: one color attachment, entries "main", no multiview. Fill
 * PipeKey directly for multiple attachments, custom entries or view masks. */
static inline Pipe pipe_intern_graphics(Gpu *g, ShaderId vs, ShaderId fs,
                                        Format color, Format depth, Blend blend_fallback) {
    PipeKey key = {0};
    key.vs = vs;
    key.fs = fs;
    key.color[0] = color;
    key.color_count = 1u;
    key.depth = depth;
    key.blend_fallback = blend_fallback;
    return pipe_intern(g, &key);
}

/* Depth-only pipe (shadow maps, depth prepasses): no colour attachment. `fs`
 * may be SHADER_NONE, or a stage that only discards (alpha-tested casters).
 * Depth-only is also the one class whose depth state must be static:
 * depth_write_enable is not a dynamic state, so a face-culled shadow pass and
 * a depth-equal prepass need different pipelines. */
static inline Pipe pipe_intern_depth_only(Gpu *g, ShaderId vs, ShaderId fs, Format depth) {
    PipeKey key = {0};
    key.vs = vs;
    key.fs = fs;
    key.color_count = 0u;
    key.depth = depth;
    key.depth_write = true;
    key.depth_cmp = CMP_LESS;
    return pipe_intern(g, &key);
}

/* Depth-only pipe with explicit static depth state (shadow pass variants). */
static inline Pipe pipe_intern_depth_cmp(Gpu *g, ShaderId vs, ShaderId fs, Format depth,
                                         Compare cmp, bool write) {
    PipeKey key = {0};
    key.vs = vs;
    key.fs = fs;
    key.color_count = 0u;
    key.depth = depth;
    key.depth_cmp = cmp;
    key.depth_write = write;
    return pipe_intern(g, &key);
}

/* ---- per-draw state ------------------------------------------------------
 * Small value object suitable for sorting draw batches and suppressing
 * redundant dynamic-state commands.
 * ----------------------------------------------------------------------- */
/* What is fixed at pipeline creation and what is per-draw is not a matter of
 * taste — it is the Vulkan dynamic-state list. These are the per-draw ones:
 * viewport, scissor, cull mode, front face, topology, depth test enable,
 * depth write enable, depth compare op, and (under EDS3) rasterization
 * samples, colour blend enable/equation, colour write mask.
 *
 * What is NOT in DrawState, and why: vertex input and input assembly, because
 * geometry is fetched by SV_VertexID; polygon mode, depth bias, logic op,
 * line width and stencil state are not dynamic states in core 1.3, so they
 * belong to the pipeline (PipeKey) and are not per-draw at all.
 *
 * `write_mask` is a "no colour written" encoding, not a partition across
 * attachments: bit i of attachment 0's nibble is R, and the same 4 bits are
 * applied to every attachment in the pass (Vulkan has one mask per attachment,
 * this struct has one mask for the draw).
 */
typedef struct DrawState {
    uint8_t samples;      /* SampleCount: 1/2/4/8                           */
    uint8_t blend;        /* Blend: OPAQUE / ALPHA / PREMUL / ADD             */
    uint8_t depth_cmp;    /* Compare: CMP_NONE = depth test off              */
    uint8_t depth_write;  /* 0/1                                              */
    uint8_t cull;         /* Cull                                             */
    uint8_t topo;         /* Topo                                             */
    uint16_t write_mask;  /* 0x0 = no colour written; anything else = all    */
} DrawState;

MU_STATIC_ASSERT(sizeof(DrawState) == 8, "DrawState is the sort key; keep it 8 bytes");

/* DRAW_MASK_ALL is "write every channel", and it is the value memset(0) *does
 * not* produce. A DrawState that reached zero through design, rather than
 * through draw_state_*(), writes no colour at all. */
#define DRAW_MASK_ALL 0xFFFFu

/* The states this engine actually needs, named for intent. */
DrawState draw_state_opaque(uint8_t samples, uint8_t cull);
DrawState draw_state_alpha(uint8_t samples, uint8_t cull);
DrawState draw_state_depth_only(uint8_t samples);

/* 64-bit sort key: pipe first, then state. Sorting the draw list on this is
 * what keeps dynamic state from costing a context roll per draw.
 *
 * The state fields are packed by width, so every one of them is named:
 * samples:3 blend:2 | depth_cmp:3 depth_write:1 | cull:2 topo:2 |
 * write_mask:1 (only "any colour at all") | pipes:16. That is 30 bits, which
 * leaves room for the pipe index in the low half where the state lives — the
 * old packing shifted a 16-bit write_mask into a 24-bit state field and then
 * put the pipe index at bit 40, so two thirds of the key was always zero.
 *
 * The key orders by pipe, then by state: that is the order that turns dynamic
 * state changes into one run per pipeline instead of one per draw.
 */
static inline uint64_t draw_sort_key(uint32_t pipe_index, DrawState s) {
    uint64_t k = 0;
    k |= (uint64_t)(s.samples     & 0x7u) <<  0;
    k |= (uint64_t)(s.blend       & 0x3u) <<  3;
    k |= (uint64_t)(s.depth_cmp   & 0x7u) <<  5;
    k |= (uint64_t)(s.depth_write & 0x1u) <<  8;
    k |= (uint64_t)(s.cull        & 0x3u) <<  9;
    k |= (uint64_t)(s.topo        & 0x3u) << 11;
    k |= (uint64_t)(s.write_mask ? 1u : 0u) << 13;
    k |= (uint64_t) pipe_index            << 16;
    return k;
}
MU_STATIC_ASSERT(MAX_PIPES <= 0x10000, "draw_sort_key reserves 16 bits for the pipe index");

/* ============================================================================
 * 9. FRAME
 *    frame_begin → frame_run(program) → frame_end. One command buffer, one
 *    submission, one present. There is no submit, fence, semaphore or queue in
 *    this API.
 * ==========================================================================*/

/* Returns false when there is no frame to record: a frame is already open, the
 * window is minimised, or image acquisition failed (a swapchain that went out
 * of date). Nothing is recorded and the caller skips the frame. This does not
 * recreate the swapchain — call set_surface() again with the same surface when
 * gpu_last_error() names acquisition. */
bool frame_begin(Gpu *g);

/* A frame program is the resolved pass list for one configuration: barriers
 * planned across passes, transients allocated and aliased, layouts derived.
 * Build it when configuration changes, not per frame. */
FrameProgram *frame_program_build(Gpu *g, const PassDesc *passes, uint32_t count);
void          frame_program_destroy(Gpu *g, FrameProgram *p);

/* Two ways to record the passes of a program, between frame_begin and
 * frame_end:
 *
 * frame_pass() begins the next pass of the program (planned barriers, render
 * targets, pipeline, default state) and ends the one before it; the caller
 * records the pass with the pass_* calls and moves on. The passes are taken
 * in the order they were given to frame_program_build(); `name`, unless NULL,
 * must be the name of the pass that is due, so code that has drifted from
 * the program fails here (NULL, gpu_last_error) instead of drawing into the
 * wrong targets. frame_end() ends the last pass.
 *
 *     frame_begin(g);
 *     p = frame_pass(g, program, "Shadow");  pass_draw(p, ...);
 *     p = frame_pass(g, program, "Scene");   pass_push(p, ...); pass_draw(p, ...);
 *     frame_end(g);
 *
 * frame_run() does the same through callbacks: it walks the whole program
 * and hands `user` to each pass's PassDesc.record. */
PassCtx *frame_pass(Gpu *g, FrameProgram *p, const char *name);
void     frame_run(Gpu *g, FrameProgram *p, void *user);

/* Flushes the planned barriers, submits, presents, and advances the timeline.
 * Retires any staging or transient memory whose timeline value has passed. */
void frame_end(Gpu *g);

/* Optional summary of the pass program and its latest timing results. */
void frame_program_report(const Gpu *g, const FrameProgram *p, const char *path);

/* ---- passes ------------------------------------------------------------- */

typedef struct AttachDesc {
    Target      target;
    AttUse      use;
    float       clear[4];      /* ATT_CLEAR / ATT_DEPTH_CLEAR; rgba or [0]=depth */
    uint32_t    mip;
    Target      resolve;       /* TARGET_NIL = none                              */
    ResolveMode resolve_mode;
} AttachDesc;

struct PassDesc {
    char     name[MAX_PASS_NAME];
    PassKind kind;             /* selects default shader stage/barrier scope  */
    Pipe     pipe;             /* interned; PIPE_NIL = the callback binds one  */
    DrawState state;           /* the pass's default draw state               */

    const AttachDesc *colors;       uint32_t color_count;
    const AttachDesc *depth;
    const Target     *tex_reads;    uint32_t tex_read_count;   /* sampled    */
    const Target     *tex_writes;   uint32_t tex_write_count;  /* storage    */
    const BufUse     *buf_reads;    uint32_t buf_read_count;
    const BufUse     *buf_writes;   uint32_t buf_write_count;

    /* 0 = derive from pass kind. Every extra stage bit is a dependency the
     * scheduler must honour, so the default is the narrow one. */
    ShaderBits read_stages;

    /* The pass body, for frame_run(): called between the planned barriers and
     * the end of the pass. NULL when the pass is recorded through frame_pass().
     * Anything recorded outside a pass is a programming error caught by the
     * debug layer shim. */
    void (*record)(void *user, PassCtx *p);
};

/* ---- recording inside a pass -------------------------------------------- */

void pass_push(PassCtx *p, const void *data, uint32_t size); /* <= 256 B     */
void pass_bind_pipe(PassCtx *p, Pipe pipe);                  /* PIPE_NIL desc */

/* Set the dynamic state used by subsequent draw commands. */
void pass_state(PassCtx *p, DrawState state);

/* Indirect commands let GPU-generated work counts remain on the GPU. */
void pass_draw_indirect(PassCtx *p, BufSlice args, uint32_t max_draws);
void pass_draw_indirect_count(PassCtx *p, BufSlice args, BufSlice count,
                              uint32_t max_draws, uint32_t stride);
void pass_dispatch(PassCtx *p, uint32_t x, uint32_t y, uint32_t z);
/* `args` holds three uint32 group counts (x, y, z) that an earlier pass - or
 * an earlier frame - wrote; offset a multiple of 4. Declare the range with
 * use_indirect() in the pass's buf_reads. Any count 0 runs nothing, which is
 * how a GPU-side decision ("no impact this frame") costs no work. */
void pass_dispatch_indirect(PassCtx *p, BufSlice args);

/* Debug and full-screen only. Counted per frame and reported, because a
 * fixed-count draw in a hot loop is the one thing this API cannot make fast. */
void pass_draw(PassCtx *p, uint32_t verts, uint32_t instances,
               uint32_t first_vert, uint32_t first_instance);
void pass_draw_fullscreen_triangle(PassCtx *p, uint32_t width, uint32_t height);

/* Optional workload annotations for reports. These are caller-supplied
 * semantics, not guessed from shader source; timing remains automatic. */
void pass_note_samples(PassCtx *p, uint32_t samples, uint32_t effective_samples,
                       double steps_per_pixel);
void pass_note_dispatch(PassCtx *p, uint32_t workgroup_x, uint32_t workgroup_y,
                        uint32_t workgroup_z, uint32_t tile_size);

/* Optional sequential GPU timestamp region inside a pass. `stage` is
 * descriptive metadata only. Scopes resolve with the frame queries; nesting
 * is not supported. Returns false for a full scope table or unbalanced calls. */
bool pass_scope_begin(PassCtx *p, const char *name, ShaderBits stage);
bool pass_scope_end(PassCtx *p);

/* ============================================================================
 * 10. UPLOADS
 *     One batch, one submit, no wait. vkQueueWaitIdle per one-shot buffer —
 *     what the current helpers.h:233 does — is the pattern this section
 *     exists to make impossible.
 * ==========================================================================*/

Upload *upload_begin(Gpu *g);
void    upload_buffer(Upload *u, BufSlice dst, DataSpan src);
void    upload_texture(Upload *u, Texture dst, const ImageRegion *r, DataSpan src);
void    upload_end(Gpu *g, Upload *u);   /* records, submits once, retires by timeline */

/* ============================================================================
 * 11. READBACK
 * ==========================================================================*/

/* Copy GPU contents back to host memory. A request queues one copy behind
 * everything submitted so far - so after frame_end() it sees that frame's
 * results - and returns at once. `dst` must stay valid until readback_ready()
 * returns true (it does the memcpy) or the request is released.
 *
 *   uint32_t args[4];
 *   Readback *rb = readback_request_buffer(g, args_slice, DATA_SPAN(args, sizeof args));
 *   while (rb && !readback_ready(g, rb, true)) {}   // blocking waits up to 1 s per call
 *   readback_release(g, rb);
 *
 * readback_request: mip 0 of a texture (target_texture() for a Target), tightly
 *   packed; `dst.size` is the number of bytes to copy and must cover the image.
 *   A 3D texture is read whole: slice after slice, width * height * depth texels.
 * readback_request_buffer: the bytes of `src` exactly (any Buffer: an
 *   arena_alloc() slice, a buffer_create() buffer). `dst.size >= src.size`;
 *   `src.size` bytes are copied. For data a shader wrote and the CPU did not:
 *   indirect arguments, counters, culling results, a simulation's state.
 *
 * Neither is for the frame path: each request allocates a staging buffer and
 * submits a command buffer. At most 32 requests can be in flight. */
Readback *readback_request(Gpu *g, Texture src, DataSpan dst);
Readback *readback_request_buffer(Gpu *g, BufSlice src, DataSpan dst);
bool      readback_ready(Gpu *g, Readback *r, bool blocking);
void      readback_release(Gpu *g, Readback *r);

/* ============================================================================
 * 12. PROFILING AND DIAGNOSTICS
 *       Pass timing is automatic. Named intra-pass scopes and workload
 *       annotations are optional; frame_end resolves their timestamp queries.
 * ==========================================================================*/

typedef struct PassStat {
    char     name[MAX_PASS_NAME];
    double   gpu_ms;
    uint64_t declared_bytes;       /* logical estimate, not physical traffic */
    uint32_t samples, effective_samples; /* zero means caller did not annotate */
    double   steps_per_pixel;           /* caller annotation; zero if omitted */
    uint32_t workgroup_x, workgroup_y, workgroup_z, tile_size;
} PassStat;

typedef struct ScopeStat {
    char      pass_name[MAX_PASS_NAME];
    char      name[MAX_PASS_NAME];
    ShaderBits stage;              /* descriptive tag; does not alter timing */
    double    gpu_ms;
    bool      complete;         /* begin/end calls were paired */
    bool      timing_available; /* query data was successfully resolved */
} ScopeStat;

typedef struct FrameStats {
    double    cpu_ms, gpu_ms;
    uint32_t  pass_count;
    PassStat  passes[MAX_PASSES];
    uint32_t  scope_count;
    ScopeStat scopes[MAX_PROFILE_SCOPES];
    uint32_t  draws, dispatches, pushes;
    uint32_t  barriers, state_changes, state_runs;
    uint32_t  pipeline_creations, descriptor_writes;
    uint32_t  layout_transitions;
    uint32_t  escape_uses;
    uint64_t  upload_bytes;
    uint64_t  transient_peak_bytes;  /* highest transient high-water mark of
                                      * THIS frame, not of the session */
    uint64_t  declared_bytes;       /* sum of per-pass logical estimates */
} FrameStats;

void perf_frame_stats(const Gpu *g, FrameStats *out);
void perf_dump(const Gpu *g, const char *path);   /* latest completed frame */

/* ============================================================================
 * 13. DIAGNOSTICS, ASSERTIONS, ESCAPE
 * ==========================================================================*/

/* Human-readable reason for the last failed create/build. Never NULL. */
const char *gpu_last_error(const Gpu *g);

/* Debug builds: validates handles, pass ordering, push sizes, indirect
 * alignment, and that every pass's pipe matches its attachments. Cheap enough
 * to leave on during development; compiled out with NDEBUG. */
bool gpu_validate(const Gpu *g);

/* The escape hatch. Only under MU_ESCAPE; each call increments a counter
 * that shows up in FrameStats.escape_uses and perf_dump, and the debug build
 * logs the line that used it. If this counter is ever non-zero in a
 * steady-state frame, the API is missing something and should grow instead. */
#ifdef MU_ESCAPE
VkDevice    gpu_raw_device(Gpu *g);
VkCommandBuffer pass_raw_cmd(PassCtx *p);
#endif

/* ============================================================================
 * 14. SHADER ABI — the contract, in one screen
 *
 *    set 0, binding 0 : sampled images   [BINDLESS_TEXTURES]
 *    set 0, binding 1 : storage images   [BINDLESS_TEXTURES]
 *    set 0, binding 2 : samplers         [BINDLESS_SAMPLERS]
 *    set 0, binding 3 : one storage buffer, addressed by {offset, size}
 *    push constants   : <= 256 bytes, per draw or per dispatch
 *
 *    Bound once per frame for graphics and compute; nothing else may be bound.
 *    A texture's bindless index is stable for its lifetime.
 * ==========================================================================*/

enum {
    BIND_SAMPLED_IMAGES  = 0,
    BIND_STORAGE_IMAGES  = 1,
    BIND_SAMPLERS        = 2,
    BIND_STORAGE_BUFFER  = 3,
    BIND_COUNT           = 4,
};

/* Push constants are padded to 256 bytes so the shader side and the C side are
 * always the same shape, and the size is checked at compile time rather than
 * discovered by validation. Same idiom the current common.h uses — a body that
 * outgrows 256 bytes is a compile error, not a validation message.
 *
 *   PUSH(DrawPush, float mvp[16]; uint32_t texture_id; float tint[4];);
 */
/* The pad length is a compile-time constant, so it has to be able to be zero:
 * a body that lands exactly on PUSH_MAX is legal, and `uint8_t _pad[0]` is
 * not (GCC accepts it as an extension; -Werror + -pedantic does not). */
#define PUSH_PAD_(used) ((used) >= PUSH_MAX ? 0u : (size_t)(PUSH_MAX - (used)))
#define PUSH(name, BODY)                                                                     \
    typedef struct ALIGNAS(PUSH_ALIGN) name##_body { BODY } name##_body;                     \
    MU_STATIC_ASSERT(sizeof(name##_body) <= PUSH_MAX,                                          \
                   #name " exceeds the 256-byte push-constant limit");                       \
    typedef struct ALIGNAS(PUSH_ALIGN) name {                                                \
        BODY uint8_t _pad[PUSH_PAD_(sizeof(name##_body))];                                   \
    } name;                                                                                  \
    MU_STATIC_ASSERT(sizeof(name) == PUSH_MAX, #name " must be exactly 256 bytes");            \
    MU_STATIC_ASSERT(PUSH_MAX % PUSH_ALIGN == 0, "push size must be 16-byte aligned")

/* ============================================================================
 * 15. INLINE HELPERS
 *       Presets for the two shapes almost every pass uses, so a call site reads
 *       as intent. No allocation, no state, all const-foldable.
 * ==========================================================================*/

static inline AttachDesc attach_color(Target t, AttUse use) {
    AttachDesc a; a.target = t; a.use = use; a.mip = 0u;
    a.resolve = TARGET_NIL; a.resolve_mode = RESOLVE_NONE;
    a.clear[0] = a.clear[1] = a.clear[2] = a.clear[3] = 0.0f;
    return a;
}

static inline AttachDesc attach_clear(Target t, float r, float g_, float b, float a) {
    AttachDesc d = attach_color(t, ATT_CLEAR);
    d.clear[0] = r; d.clear[1] = g_; d.clear[2] = b; d.clear[3] = a;
    return d;
}

static inline AttachDesc attach_depth_clear(Target t, float depth) {
    AttachDesc d = attach_color(t, ATT_DEPTH_CLEAR);
    d.clear[0] = depth;
    return d;
}

static inline AttachDesc attach_resolve(AttachDesc a, Target dst, ResolveMode m) {
    a.resolve = dst; a.resolve_mode = m;
    return a;
}

static inline BufUse use_read(Buffer b, uint64_t off, uint64_t size) {
    BufUse u; u.slice.buffer = b; u.slice.offset = off; u.slice.size = size;
    u.access = ACC_READ; u.stages = SH_NONE;
    return u;
}

static inline BufUse use_write(Buffer b, uint64_t off, uint64_t size) {
    BufUse u; u.slice.buffer = b; u.slice.offset = off; u.slice.size = size;
    u.access = ACC_WRITE; u.stages = SH_NONE;
    return u;
}

static inline BufUse use_indirect(Buffer b, uint64_t off, uint64_t size) {
    BufUse u; u.slice.buffer = b; u.slice.offset = off; u.slice.size = size;
    u.access = ACC_INDIRECT; u.stages = SH_NONE;
    return u;
}

static inline SamplerDesc sampler_linear_clamp(void) {
    SamplerDesc s = {0};
    s.min_filter = FILT_LINEAR; s.mag_filter = FILT_LINEAR; s.mip_filter = FILT_LINEAR;
    s.addr_u = ADDR_CLAMP_EDGE; s.addr_v = ADDR_CLAMP_EDGE; s.addr_w = ADDR_CLAMP_EDGE;
    s.max_lod = 1000.0f;
    return s;
}

/* Bytes per pixel for callers that need format-size arithmetic.
 * Unknown/NONE formats return 0. */
static inline uint32_t format_bytes(Format f) {
    switch (f) {
    case FMT_RGBA8_UNORM: case FMT_RGBA8_SRGB:
    case FMT_BGRA8_UNORM: case FMT_BGRA8_SRGB:
    case FMT_RGBA8_UINT:  case FMT_R32F: case FMT_R32_UINT:
    case FMT_RGBA8_SNORM:
    case FMT_RGB10A2_UNORM: case FMT_R11G11B10F:
    case FMT_D24S8: case FMT_D32F:
        return 4u;
    case FMT_RG8_UNORM: case FMT_R16F: case FMT_D16_UNORM:
        return 2u;
    case FMT_R8_UNORM:
        return 1u;
    case FMT_RG16F:
        return 4u;
    case FMT_RGBA16_UNORM: case FMT_RGBA16F:
        return 8u;
    case FMT_RGBA32F:
        return 16u;
    default:
        return 0u;
    }
}

static inline bool format_is_depth(Format f) {
    return f == FMT_D16_UNORM || f == FMT_D24S8 || f == FMT_D32F;
}

/* Compile-time honesty about the handles: 8 bytes, no padding, no surprises
 * when they are stored in app-side structs. */
MU_STATIC_ASSERT(sizeof(Buffer) == 8, "handle must stay 8 bytes");
MU_STATIC_ASSERT(sizeof(Texture) == 8, "handle must stay 8 bytes");
MU_STATIC_ASSERT(sizeof(Target) == 8, "handle must stay 8 bytes");
MU_STATIC_ASSERT(sizeof(Pipe) == 8, "handle must stay 8 bytes");
MU_STATIC_ASSERT(PUSH_MAX == 256, "push constant range is 256 bytes");
MU_STATIC_ASSERT(PUSH_ALIGN == 16, "push constants must be 16-byte aligned");


/* ============================================================================
 * 16. SHADER INPUTS
 * ==========================================================================*/

typedef enum ShaderStage2 {
    STAGE_VERTEX = 0, STAGE_FRAGMENT, STAGE_COMPUTE,
    STAGE_TASK, STAGE_MESH,
} ShaderStage2;

typedef struct ShaderSource {
    const char  *path;
    const char  *entry;
    ShaderStage2 stage;
    const char *const *defines;
    uint32_t     define_count;
} ShaderSource;
bool shader_compile_in_process(Gpu *g, const ShaderSource *src, ShaderId *out);

/* Common case: load `path` with entry "main". Fill ShaderSource directly for a
 * custom entry point or preprocessor defines. */
static inline bool shader_load(Gpu *g, const char *path, ShaderStage2 stage, ShaderId *out) {
    ShaderSource src = {0};
    src.path = path;
    src.entry = "main";
    src.stage = stage;
    return shader_compile_in_process(g, &src, out);
}

/* ============================================================================
 * 17. BINDLESS SHADER DATA
 * ==========================================================================*/

/* Bindless geometry is shader-driven: allocate its bytes with
 * arena_alloc(g, ARENA_DEVICE, ...), upload them, and use the returned slice's
 * offset into GpuTables.storage_buffers (set 0, binding 3). Put that offset
 * and vertex/index metadata in an application-defined push-constant struct and
 * send it with pass_push(); the vertex shader fetches by vertex/instance ID.
 * Separately created BUF_DEVICE buffers can use buffer_info()'s device address
 * when buffer-device-address support is available. No library geometry wrapper
 * or Vulkan vertex-input binding is required; the app and shader share the
 * small geometry-record ABI. */

/* Integration points that must share the GPU's Vulkan instance, shader
 * compiler lifetime, or pipeline-statistics identity. These remain in the
 * single public header so applications do not need a second backend header. */
/* ---- in-process shader source compiler -----------------------------------
 * Registered by the application (e.g. a libslang binding). `out_words` stays
 * owned by the callback; the backend copies it into a VkShaderModule immediately.
 * With no callback registered, shader_compile_in_process() accepts a .spv
 * path and returns false for source it cannot compile in-process. */
typedef bool (*compile_fn)(void *user,
                           const char *path, const char *entry,
                           ShaderStage2 stage,
                           const char *const *defines, uint32_t define_count,
                           const uint32_t **out_words, uint32_t *out_word_count,
                           char *error, size_t error_size);
void set_compiler(Gpu *g, compile_fn fn, void *user);

/* Register a SPIR-V module that the app already holds in memory. */
bool shader_register_spv(Gpu *g, const uint32_t *words, uint32_t word_count,
                         const char *path, const char *entry,
                         ShaderStage2 stage, ShaderId *out);

/* PipeKey describes graphics stages; compute pipelines have their own key
 * and use the same fixed-capacity intern table. */
Pipe pipe_intern_compute(Gpu *g, ShaderId cs, const char *entry);

/* The app must create its VkSurfaceKHR against the SAME VkInstance the device
 * was created from, so the WSI adapter needs the instance. */
VkInstance gpu_raw_instance(const Gpu *g);

/* Attach a WSI surface created by the application. Without one the backend
 * runs headless: TARGET_SWAPCHAIN is an internal colour image and frame_end
 * does not present. Returns false if swapchain creation fails. */
bool set_surface(Gpu *g, VkSurfaceKHR surface);

/* Bindless index to store in shader-facing material or push data. */
uint32_t sampler_bindless_index(const Gpu *g, Sampler s);

/* Resolve a render target to the underlying texture when an operation accepts
 * a texture handle rather than a target handle (for example readback). */
Texture target_texture(const Gpu *g, Target target);

/* ============================================================================
 * 18. PASS BUILDERS
 *       compute_pass() fills a PassDesc for the one shape most compute demos
 *       use. It was at the bottom of the file under "inline helpers", untyped
 *       for its name argument and copying it with strcpy: a 60-character pass
 *       name overran a 48-byte field silently. Names are truncated now, the
 *       same way copy_name() in vk.c truncates every other name.
 * ==========================================================================*/

static inline void pass_name_set(char dst[MAX_PASS_NAME], const char *src) {
    size_t i = 0u;
    if (!src) src = "";
    while (i + 1u < (size_t)MAX_PASS_NAME && src[i]) { dst[i] = src[i]; ++i; }
    dst[i] = '\0';
}

static inline PassDesc *compute_pass(PassDesc *p, const char *name, Pipe pipe,
                                     const BufUse *reads, uint32_t read_count,
                                     const BufUse *writes, uint32_t write_count) {
    memset(p, 0, sizeof *p);
    pass_name_set(p->name, name);
    p->kind = PASS_COMPUTE;
    p->pipe = pipe;
    p->buf_reads = reads;   p->buf_read_count = read_count;
    p->buf_writes = writes; p->buf_write_count = write_count;
    return p;
}

#endif /* MU_VK_H */
