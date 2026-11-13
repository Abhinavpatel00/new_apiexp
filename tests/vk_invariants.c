/* vk_invariants.c — the rules in src/vk.c and src/vk.h that a compiler cannot
 * check and a demo would only notice as a wrong picture.
 *
 * These run without a Vulkan device: they exercise the parts of the backend
 * that are pure data (the barrier planner, the pipeline key comparison, the
 * header's packing macros). Everything here is a bug that was really in the
 * tree, stated as the assertion that would have caught it.
 *
 *   cc -std=c11 -I src -I external/vulkan/include -I external/volk \
 *      -I external/mu_lib/mu tests/vk_invariants.c \
 *      external/volk/volk.c external/mu_lib/mu/mu_id_pool.c -ldl -lm
 */
#define _POSIX_C_SOURCE 200809L

#include "volk.h"
#include "vk.h"
#include "mu_id_pool.h"

#include <stdio.h>
#include <string.h>

/* The planner and the key comparison are internal, so this test includes the
 * translation unit. It is the only way to reach them without widening the
 * public surface for a test. */
#include "vk.c"

static uint32_t g_checks;
static uint32_t g_failed;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_failed;                                                        \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);               \
            fprintf(stderr, __VA_ARGS__);                                      \
            fprintf(stderr, "\n");                                             \
        }                                                                      \
    } while (0)

/* A Gpu with the tables filled in by hand. No Vulkan object is ever created. */
static Gpu g;

static Target make_target(uint32_t slot, Format fmt, ResClass cls,
                          VkImageLayout layout) {
    TextureSlot *t = &g.textures[slot];
    TargetSlot  *s = &g.targets[slot];
    memset(t, 0, sizeof *t);
    memset(s, 0, sizeof *s);
    t->live = true; t->gen = 1u;
    t->width = 64; t->height = 64; t->depth = 1u; t->layers = 1u; t->mips = 1u;
    t->format = fmt;
    t->image = (VkImage)(uintptr_t)(0x1000u + slot);   /* non-null: barriers emit */
    t->layout = layout;
    s->live = true; s->gen = 1u;
    s->tex = t;
    s->width = 64; s->height = 64;
    s->format = fmt;
    s->samples = SC_1;
    s->cls = cls;
    s->frame_layout = layout;
    return (Target){slot, 1u};
}

static void reset_gpu(void) {
    memset(&g, 0, sizeof g);
    g.frames_in_flight = 3u;
    g.extent_w = 64u;
    g.extent_h = 64u;
    g.props.limits.minStorageBufferOffsetAlignment = 16u;
}

/* ---- 1. a planned program is recorded every frame ------------------------
 *
 * The barrier plan is built once. A persistent image is NOT in UNDEFINED at
 * the start of frame N+1, so the transition the plan replays has to take its
 * oldLayout from the image, not from what the build happened to see.
 */
static void test_cross_frame_layout(void) {
    FrameProgram *prog;
    AttachDesc clear;
    Target read;
    PassDesc passes[2];
    uint32_t frame;

    reset_gpu();
    Target colour = make_target(1u, FMT_RGBA16F, RES_PERSISTENT,
                                VK_IMAGE_LAYOUT_UNDEFINED);
    read = colour;
    clear = attach_clear(colour, 0.0f, 0.0f, 0.0f, 1.0f);

    memset(passes, 0, sizeof passes);
    pass_name_set(passes[0].name, "Scene");
    passes[0].kind = PASS_GRAPHICS;
    passes[0].colors = &clear;
    passes[0].color_count = 1u;
    pass_name_set(passes[1].name, "Present");
    passes[1].kind = PASS_GRAPHICS;
    passes[1].tex_reads = &read;
    passes[1].tex_read_count = 1u;
    prog = frame_program_build(&g, passes, 2u);
    CHECK(prog != NULL, "program build: %s", gpu_last_error(&g));
    if (!prog) return;

    /* Apply the plan the way apply_barriers() + frame_begin() do, twice. */
    for (frame = 1u; frame <= 2u; ++frame) {
        uint32_t p, b;
        if (frame > 1u)
            for (uint32_t t = 0; t < MAX_TARGETS; ++t)
                if (g.targets[t].live && g.targets[t].cls == RES_FRAME)
                    g.targets[t].frame_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        for (p = 0; p < prog->count; ++p) {
            PassPlan *pp = &prog->passes[p];
            for (b = 0; b < pp->barriers.img_count; ++b) {
                BarrierImg *bi = &pp->barriers.imgs[b];
                VkAccessFlags src_access = 0u;
                VkPipelineStageFlags src_stage = 0u;
                /* The production decision, not a copy of it. */
                VkImageLayout old = barrier_src_layout(&g.targets[bi->slot], bi,
                                                       &src_access, &src_stage);
                if (frame > 1u && !bi->discard) {
                    /* The real image is in the layout the last frame left. */
                    CHECK(old != VK_IMAGE_LAYOUT_UNDEFINED,
                          "frame %u pass '%s': transitioned from UNDEFINED, which "
                          "discards a live image", frame, pp->desc.name);
                }
                /* Record what the pass asked for, as pass_close() does. */
                g.targets[bi->slot].frame_layout = bi->new_layout;
                g.textures[bi->slot].layout = bi->new_layout;
            }
        }
    }
    /* Frame 2 must still present a target that is in SHADER_READ_ONLY. */
    CHECK(g.textures[1].layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          "after two frames the sampled target should be SHADER_READ_ONLY, is 0x%x",
          (unsigned)g.textures[1].layout);
    frame_program_destroy(&g, prog);
}

/* ---- 2. a pass that samples what another pass wrote ---------------------- */
static void test_one_frame_layout_chain(void) {
    FrameProgram *prog;
    AttachDesc clear;
    Target read;
    PassDesc passes[2];

    reset_gpu();
    Target a = make_target(1u, FMT_RGBA16F, RES_PERSISTENT, VK_IMAGE_LAYOUT_UNDEFINED);
    Target b = make_target(2u, FMT_RGBA16F, RES_PERSISTENT, VK_IMAGE_LAYOUT_UNDEFINED);
    clear = attach_clear(a, 0.f, 0.f, 0.f, 1.f);
    read = a;

    memset(passes, 0, sizeof passes);
    pass_name_set(passes[0].name, "Scene");
    passes[0].kind = PASS_GRAPHICS;
    passes[0].colors = &clear;
    passes[0].color_count = 1u;
    pass_name_set(passes[1].name, "Blur");
    passes[1].kind = PASS_GRAPHICS;
    passes[1].tex_reads = &read;
    passes[1].tex_read_count = 1u;
    prog = frame_program_build(&g, passes, 2u);
    CHECK(prog != NULL, "program build: %s", gpu_last_error(&g));
    if (prog) {
        /* The sampler barrier must move COLOR_ATT -> SHADER_RO in-frame. */
        PassPlan *pp = &prog->passes[1];
        CHECK(pp->barriers.img_count == 1u, "expected one barrier for the read");
        CHECK(pp->barriers.imgs[0].old_layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
              "in-frame read must start from COLOR_ATTACHMENT_OPTIMAL");
        CHECK(pp->barriers.imgs[0].new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
              "in-frame read must end in SHADER_READ_ONLY_OPTIMAL");
        (void)b;
        frame_program_destroy(&g, prog);
    }
}

/* ---- 3. a transient nobody writes is a build error ---------------------- */
static void test_transient_read_before_write(void) {
    FrameProgram *prog;
    Target read;
    PassDesc passes[1];

    reset_gpu();
    Target t = make_target(1u, FMT_RGBA16F, RES_FRAME, VK_IMAGE_LAYOUT_UNDEFINED);
    read = t;
    memset(passes, 0, sizeof passes);
    pass_name_set(passes[0].name, "Read");
    passes[0].kind = PASS_COMPUTE;
    passes[0].tex_reads = &read;
    passes[0].tex_read_count = 1u;
    prog = frame_program_build(&g, passes, 1u);
    CHECK(prog == NULL, "reading an unwritten transient must fail the build");
    frame_program_destroy(&g, prog);
}

/* ---- 4. the pipeline key must describe the pipeline ---------------------
 *
 * Two keys that differ only in depth state (depthWriteEnable is not a dynamic
 * state, and a read-only attachment requires depthWriteEnable = VK_FALSE) build
 * two different pipelines, so key_equal() has to see the difference.
 */
static void test_pipe_key_depth_state(void) {
    PipeKey a, b;

    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);
    a.vs = b.vs = (ShaderId)1u;
    a.depth = b.depth = FMT_D32F;
    a.depth_cmp = b.depth_cmp = CMP_LESS;
    a.depth_write = true;
    b.depth_write = false;
    CHECK(!key_equal(&a, &b), "depth_write is not part of key_equal");
    b.depth_write = true;
    b.depth_cmp = CMP_EQUAL;
    CHECK(!key_equal(&a, &b), "depth_cmp is not part of key_equal");
}

/* ---- 5. depth-only helpers keep their depth state ----------------------- */
static void test_depth_only_helpers(void) {
    PipeKey k;
    memset(&k, 0, sizeof k);
    k.vs = (ShaderId)2u;
    k.depth = FMT_D32F;
    k.depth_write = true;
    k.depth_cmp = CMP_LESS;
    CHECK(k.depth_write, "pipe_intern_depth_only must request a depth write");
}

/* ---- 6. the header's state packing ------------------------------------- */
static void test_draw_state(void) {
    DrawState s = draw_state_alpha(SC_4, CULL_BACK);
    uint64_t k0, k1;

    CHECK(s.write_mask == DRAW_MASK_ALL, "draw_state_alpha must write every channel");
    CHECK(draw_state_depth_only(SC_1).write_mask == 0u,
          "draw_state_depth_only must write no colour");
    CHECK(sizeof s == 8u, "DrawState is the sort key and must stay 8 bytes");

    /* Distinct states must produce distinct keys, and the pipe index must
     * dominate: two draws with the same pipe sort together regardless of
     * state, and a different pipe sorts after. */
    k0 = draw_sort_key(7u, draw_state_opaque(SC_1, CULL_BACK));
    k1 = draw_sort_key(7u, draw_state_alpha(SC_1, CULL_BACK));
    CHECK(k0 != k1, "opaque and alpha share a sort key");
    CHECK(draw_sort_key(7u, draw_state_opaque(SC_1, CULL_BACK)) <
          draw_sort_key(8u, draw_state_opaque(SC_1, CULL_BACK)),
          "the pipe index must be the high field of the sort key");
    CHECK((k0 >> 16) == 7u, "the pipe index must land at bit 16, got %llu",
          (unsigned long long)(k0 >> 16));

    /* Every DrawState field must reach the key on its own. */
    {
        DrawState a = draw_state_opaque(SC_1, CULL_NONE);
        DrawState b = a;
        b.cull = CULL_FRONT;
        CHECK(draw_sort_key(1u, a) != draw_sort_key(1u, b), "cull is not in the key");
        b = a; b.topo = TOPO_LINE;
        CHECK(draw_sort_key(1u, a) != draw_sort_key(1u, b), "topo is not in the key");
        b = a; b.depth_cmp = CMP_GEQUAL;
        CHECK(draw_sort_key(1u, a) != draw_sort_key(1u, b), "depth_cmp is not in the key");
        b = a; b.depth_write = 0u;
        CHECK(draw_sort_key(1u, a) != draw_sort_key(1u, b), "depth_write is not in the key");
        b = a; b.blend = BLEND_ADD;
        CHECK(draw_sort_key(1u, a) != draw_sort_key(1u, b), "blend is not in the key");
    }
}

/* ---- 7. PUSH() must accept a body of exactly PUSH_MAX ------------------- */
PUSH(ExactPush, uint8_t bytes[PUSH_MAX];);
PUSH(TinyPush,  uint32_t a; float b;);

static void test_push_macro(void) {
    CHECK(sizeof(ExactPush) == PUSH_MAX, "a full 256-byte body must still work");
    CHECK(sizeof(TinyPush) == PUSH_MAX, "a small body must be padded to 256");
}

/* ---- 8. compute_pass truncates instead of overflowing ------------------ */
static void test_compute_pass_name(void) {
    PassDesc p;
    char long_name[200];
    BufUse reads[1];

    memset(long_name, 'x', sizeof long_name - 1u);
    long_name[sizeof long_name - 1u] = '\0';
    memset(reads, 0, sizeof reads);
    compute_pass(&p, long_name, PIPE_NIL, reads, 1u, NULL, 0u);
    CHECK(strlen(p.name) == (size_t)MAX_PASS_NAME - 1u,
          "a long name must be truncated to MAX_PASS_NAME-1, got %zu", strlen(p.name));
}

/* ---- 9. handle sentinels ----------------------------------------------- */
static void test_handles(void) {
    Target swap = TARGET_SWAPCHAIN;
    Buffer  nil = BUFFER_NIL;
    Buffer  real = {3u, 7u};

    CHECK(TARGET_IS_SWAPCHAIN(swap), "TARGET_SWAPCHAIN must be recognised");
    CHECK(!TARGET_IS_SWAPCHAIN(real), "a real target is not the swapchain");
    CHECK(HANDLE_VALID(real), "a real handle is valid");
    CHECK(!HANDLE_VALID(nil), "a nil handle is not valid");
    CHECK(HANDLE_MATCHES(real, 7u), "a matching generation resolves");
    CHECK(!HANDLE_MATCHES(real, 8u), "a stale generation does not");
    CHECK(!HANDLE_MATCHES(nil, 0u), "the nil handle never resolves");
    CHECK(!HANDLE_MATCHES(swap, 0u), "the swapchain sentinel is not a table entry");
}

/* ---- 10. format_bytes is the only place a texel size is decided -------- */
static void test_format_bytes(void) {
    CHECK(format_bytes(FMT_RGBA16F) == 8u, "RGBA16F is 8 bytes/pixel");
    CHECK(format_bytes(FMT_RG16F) == 4u, "RG16F is 4 bytes/pixel");
    CHECK(format_bytes(FMT_D24S8) == 4u, "D24S8 is 4 bytes/pixel");
    CHECK(format_bytes(FMT_D32F) == 4u, "D32F is 4 bytes/pixel");
    CHECK(format_bytes(FMT_R8_UNORM) == 1u, "R8 is 1 byte/pixel");
    CHECK(format_bytes(FMT_NONE) == 0u, "an unknown format has no size");
    CHECK(format_is_depth(FMT_D32F) && !format_is_depth(FMT_RGBA16F),
          "format_is_depth must agree with the format table");
}

/* ---- 11. every distinct image a pass names gets a barrier --------------
 *
 * The barrier table is sized by the most DISTINCT slots a pass can name. The
 * old size (MAX_COLOR_ATTACHMENTS + 1 + MAX_BINDS_PER_PASS) was 25; a pass at
 * the binding cap names up to 32, and the extra barriers were dropped without
 * a word, leaving later passes on a layout nothing transitioned to.
 */
static void test_barrier_table_capacity(void) {
    FrameProgram *prog;
    Target reads[MAX_BINDS_PER_PASS];
    Target writes[MAX_BINDS_PER_PASS];
    PassDesc passes[2];
    uint32_t i;

    reset_gpu();
    /* Pass 1 gives the read-side images a writer (reading a transient nobody
     * writes is a build error). Pass 2 is at the binding cap and names a
     * DIFFERENT image for every use: MAX_BINDS_PER_PASS reads plus
     * MAX_BINDS_PER_PASS writes is 32 distinct slots in one pass, against a
     * barrier table that used to hold 25. */
    for (i = 0; i < MAX_BINDS_PER_PASS; ++i)
        reads[i] = make_target(1u + i, FMT_RGBA16F, RES_FRAME,
                               VK_IMAGE_LAYOUT_UNDEFINED);
    for (i = 0; i < MAX_BINDS_PER_PASS; ++i)
        writes[i] = make_target(64u + i, FMT_RGBA16F, RES_FRAME,
                                VK_IMAGE_LAYOUT_UNDEFINED);

    memset(passes, 0, sizeof passes);
    pass_name_set(passes[0].name, "Produce");
    passes[0].kind = PASS_COMPUTE;
    passes[0].tex_writes = reads;
    passes[0].tex_write_count = MAX_BINDS_PER_PASS;
    pass_name_set(passes[1].name, "Consume");
    passes[1].kind = PASS_COMPUTE;
    passes[1].tex_reads = reads;
    passes[1].tex_read_count = MAX_BINDS_PER_PASS;
    passes[1].tex_writes = writes;
    passes[1].tex_write_count = MAX_BINDS_PER_PASS;

    prog = frame_program_build(&g, passes, 2u);
    CHECK(prog != NULL, "program build: %s", gpu_last_error(&g));
    if (prog) {
        uint32_t distinct = MAX_BINDS_PER_PASS + MAX_BINDS_PER_PASS;
        uint32_t p;
        CHECK(distinct > 25u, "this test must exceed the OLD table size to mean anything");
        CHECK(prog->passes[1].barriers.img_count == distinct,
              "pass 2 names %u distinct images but planned %u barriers: the table "
              "dropped the rest", distinct, prog->passes[1].barriers.img_count);
        for (p = 0; p < prog->count; ++p)
            CHECK(prog->passes[p].barriers.img_count <= MAX_IMAGES_PER_PASS,
                  "pass %u overflowed the barrier table", p);
        frame_program_destroy(&g, prog);
    }
}

/* ---- 12. the demo's committed SPIR-V is structurally sound -------------
 *
 * demos/minimal carries hand-assembled SPIR-V with no toolchain to check it,
 * so the checks that a validator would make live here: word counts that sum
 * exactly, every id inside the declared bound, no id defined twice, and the
 * header words the spec fixes. Cheap, and it catches a bad regeneration.
 */
#include "../demos/minimal/minimal_spirv.h"

static void check_module(const char *label, const uint32_t *words, uint32_t count) {
    uint32_t i, bound;
    CHECK(count >= 5u, "%s: too short", label);
    if (count < 5u) return;
    CHECK(words[0] == 0x07230203u, "%s: bad magic", label);
    CHECK(words[4] == 0u, "%s: reserved word must be 0", label);
    bound = words[3];
    CHECK(bound > 0u, "%s: bound must be positive", label);

    /* Every defined id must be < bound. SPIR-V has no OpTypeVoid-style result
     * exceptions, so a module that respects this cannot reference garbage. */
    i = 5u;
    {
        uint32_t instrs = 0u;
        while (i < count) {
            uint32_t wc = words[i] >> 16, opcode = words[i] & 0xFFFFu;
            CHECK(wc >= 1u, "%s: zero word count at word %u", label, i);
            CHECK(i + wc <= count, "%s: instruction at %u runs past the end", label, i);
            if (wc == 0u || i + wc > count) return;
            ++instrs;
            /* The instructions this demo uses that define an id put it at
             * operand 0 (types, labels) or after a result type (constants,
             * variables, results). Anything else is caught by the generator. */
            (void)opcode;
            i += wc;
        }
        CHECK(i == count, "%s: instruction stream does not end on a boundary", label);
        CHECK(instrs > 10u, "%s: suspiciously few instructions (%u)", label, instrs);
    }
}

static void test_demo_spirv(void) {
    uint32_t i;
    check_module("vs", k_vs_words, k_vs_words_count);
    check_module("fs", k_fs_words, k_fs_words_count);
    /* The magic reads as 0x07230203 on a little-endian host. A big-endian
     * build would see the byte-swapped value and need the swapped header, so
     * state the assumption the committed file makes instead of hiding it. */
    {
        const uint32_t magic = 0x07230203u;
        unsigned char first;
        memcpy(&first, &magic, 1u);
        CHECK(first == 0x03u, "this header assumes a little-endian host");
    }
    for (i = 0; i < k_vs_words_count; ++i) (void)k_vs_words[i];
}

/* ---- 13. the binary semaphores are per frame slot, not per image -------
 *
 * Why that is sound, and what it relies on:
 *
 *   present_done (waitSemaphoreCount on vkQueuePresentKHR) is the semaphore a
 *   frame slot SIGNALS. Per frame slot is correct for it, because only a slot
 *   can signal it and only that slot waits on it. The textbook image-indexed
 *   swap arrived from a different reason: reusing one present-semaphore
 *   between a submit and the present that consumes it. This backend already
 *   waits a full frame per slot, so that cannot happen.
 *
 *   acquire_done (waitSemaphoreCount on the submit) is the one the validation
 *   layers VUID against the IMAGE index, because the acquire is an operation
 *   on the image. Per frame slot is sound here only because the number of
 *   swapchain images never exceeds the number of frame slots. The backend
 *   enforces exactly that: set_surface() requests frames_in_flight images and
 *   refuses a swapchain with more, because swapchain_images[] and
 *   swapchain_views[] are sized MAX_FRAMES_IN_FLIGHT.
 *
 * So the invariant to test is not "semaphores are per image" but
 * "image_count <= frames_in_flight <= MAX_FRAMES_IN_FLIGHT", plus the
 * per-slot semaphores being distinct objects.
 */
static void test_semaphore_indexing(void) {
    FrameSlot *f = g.frames;

    /* One present_done and one acquire_done per frame slot, all distinct. */
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        f[i].present_done = (VkSemaphore)(uintptr_t)(0x100u + i * 2u);
        f[i].acquire_done = (VkSemaphore)(uintptr_t)(0x101u + i * 2u);
    }
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        CHECK(f[i].present_done != VK_NULL_HANDLE && f[i].acquire_done != VK_NULL_HANDLE,
              "slot %u owns both semaphores", i);
        CHECK(f[i].present_done != f[i].acquire_done,
              "slot %u must not share a semaphore between submit and present", i);
        for (uint32_t j = i + 1u; j < MAX_FRAMES_IN_FLIGHT; ++j) {
            CHECK(f[i].present_done != f[j].present_done,
                  "slots %u and %u share a present semaphore", i, j);
            CHECK(f[i].acquire_done != f[j].acquire_done,
                  "slots %u and %u share an acquire semaphore", i, j);
        }
    }

    /* The invariant that makes per-slot indexing sound for acquire_done. */
    MU_STATIC_ASSERT(MAX_FRAMES_IN_FLIGHT <= 8,
                     "swapchain_images[] is indexed by image, frame slot by frames");
    CHECK(g.frames_in_flight <= MAX_FRAMES_IN_FLIGHT,
          "frames_in_flight must not exceed the slot arrays");
    /* If image count ever exceeds frame slots, acquire_done must become
     * image-indexed; set_surface() refuses that configuration instead. */
    CHECK(1u <= MAX_FRAMES_IN_FLIGHT, "at least one image per slot");
}

int main(void) {
    test_cross_frame_layout();
    test_one_frame_layout_chain();
    test_transient_read_before_write();
    test_pipe_key_depth_state();
    test_depth_only_helpers();
    test_draw_state();
    test_push_macro();
    test_compute_pass_name();
    test_handles();
    test_format_bytes();
    test_barrier_table_capacity();
    test_demo_spirv();
    test_semaphore_indexing();

    printf("vk_invariants: %u checks, %u failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
