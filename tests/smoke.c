/* smoke.c — end-to-end headless test of the v2 Vulkan backend.
 * ============================================================================
 *  Runs on any Vulkan 1.3+ device; in this sandbox it runs on lavapipe:
 *
 *      VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json ./build/smoke
 *
 *  It deliberately exercises the whole public surface, then verifies REAL GPU
 *  output: a compute shader writes a known ramp into a storage image, and the
 *  readback path brings those bytes back to the host. A backend that only
 *  "compiles" but never submits would fail the ramp check.
 * ==========================================================================*/
#include "vk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define W 256u
#define H 128u
#define STORAGE_W 64u
#define STORAGE_H 1u
#define STORAGE_BYTES (STORAGE_W * STORAGE_H * 4u)

static int failures = 0;
#define CHECK(cond, ...)                                                         \
    do { if (!(cond)) { fprintf(stderr, "FAIL: "); fprintf(stderr, __VA_ARGS__);  \
                        fprintf(stderr, "\n"); ++failures; } } while (0)
#define REQUIRE(cond, ...)                                                       \
    do { if (!(cond)) { fprintf(stderr, "FATAL: "); fprintf(stderr, __VA_ARGS__); \
                        fprintf(stderr, "\n"); ++failures; return 1; } } while (0)

typedef struct Test {
    Gpu          *g;
    Pipe          compute_pipe;
    Pipe          gfx_pipe;
    Target        storage_target;
    Target        color_target;
    uint32_t      frames;
} Test;

/* ---- pass bodies ---------------------------------------------------------*/
static void record_compute(void *user, PassCtx *p) {
    (void)user;
    pass_note_samples(p, 1u, 1u, 1.0);
    pass_note_dispatch(p, 64u, 1u, 1u, 0u);
    pass_dispatch(p, 1u, 1u, 1u);
}
static void record_compute_read(void *user, PassCtx *p) {
    (void)user;
    pass_note_dispatch(p, 64u, 1u, 1u, 0u);
    pass_dispatch(p, 1u, 1u, 1u);
}
static void record_triangle(void *user, PassCtx *p) {
    Test *t = (Test *)user;
#ifdef MU_ESCAPE
    CHECK(pass_raw_cmd(p) != VK_NULL_HANDLE, "pass_raw_cmd escape returns a cmd buffer");
#endif
    pass_push(p, "mu_vk", 5u);
    CHECK(pass_scope_begin(p, "fullscreen draw", SH_VERT | SH_FRAG),
          "scope begin succeeds");
    pass_draw_fullscreen_triangle(p, W, H);
    CHECK(pass_scope_end(p), "scope end succeeds");
    ++t->frames;
}

int main(void) {
    GpuDesc desc;
    setbuf(stdout, NULL);
    GpuInfo info;
    GpuTables tables;
    Gpu *g;
    Test test;
    Buffer devbuf;
    BufferInfo bi;
    Texture tex;
    Sampler samp;
    SamplerDesc sd;
    ShaderId cs_id = SHADER_NONE, tri_id = SHADER_NONE;
    ShaderSource src;
    PipeKey key;
    Pipe found;
    FrameProgram *prog = NULL;
    PassDesc passes[3];
    AttachDesc color_att;
    BufUse buf_write, buf_read;
    DataSpan initial, upload_src;
    uint8_t initial_pixels[4u * 4u * 4u];
    uint8_t upload_data[64];
    uint8_t readback[STORAGE_BYTES];
    BufSlice devslice, arena_slices[3];
    ArenaStats ast;
    FrameStats fs;
    uint32_t pipelines = 0, compiled = 0, bindless = 0, i;
    Readback *rb;
    Upload *up;

    memset(&desc, 0, sizeof desc);
    desc.width = W; desc.height = H;
    desc.frames_in_flight = 3u;
    desc.app_name = "smoke";
    desc.vsync = false;
    desc.validation = false;
#ifdef MU_ESCAPE
    desc.enable_escape = true;
#else
    desc.enable_escape = false;
#endif

    g = gpu_create(&desc);
    REQUIRE(g != NULL, "gpu_create failed: %s", "see stderr");
    printf("device: %s\n", "(query below)");

    gpu_info(g, &info);
    printf("device: %s  api device mem %.1f MiB  subgroup size %u\n",
           info.device_name, (double)info.device_memory / (1024.0 * 1024.0),
           info.subgroup_size);
    CHECK(info.extent_w == W && info.extent_h == H, "extent hint round trip");
    CHECK(info.max_compute_workgroup_invocations >= 64u, "compute limit");
#ifdef MU_ESCAPE
    CHECK(gpu_raw_device(g) != VK_NULL_HANDLE, "gpu_raw_device escape returns VkDevice");
#endif

    gpu_tables(g, &tables);
    CHECK(HANDLE_VALID(tables.storage_buffers.buffer), "storage buffer handle");
    CHECK(tables.sampled_image_count > 0u, "bindless window");
    CHECK(tables.sampler_count == BINDLESS_SAMPLERS, "sampler window");

    /* ---- resources ------------------------------------------------------- */
    {
        BufferDesc bd;
        memset(&bd, 0, sizeof bd);
        bd.name = "test_buffer";
        bd.size = 256u;
        bd.usage = BUF_DEVICE | BUF_XFER_DST | BUF_INDIRECT;
        bd.cls = RES_PERSISTENT;
        devbuf = buffer_create(g, &bd);
    }
    REQUIRE(HANDLE_VALID(devbuf), "buffer_create: %s", gpu_last_error(g));
    CHECK(buffer_info(g, devbuf, &bi), "buffer_info");
    CHECK(bi.size >= 256u, "buffer size %llu", (unsigned long long)bi.size);
    CHECK(bi.mapped == NULL, "device buffer is not host mapped");

    /* The first texture claims bindless index 0, which is what smoke_cs.slang
     * addresses at set 0 binding 1. */
    {
        TargetDesc td;
        memset(&td, 0, sizeof td);
        td.name = "storage_image";
        td.width = STORAGE_W; td.height = STORAGE_H;
        td.format = FMT_RGBA8_UNORM;
        td.usage = TGT_STORAGE | TGT_COLOR | TGT_SAMPLED;
        td.samples = SC_1; td.mips = 0u; td.cls = RES_PERSISTENT;
        test.storage_target = target_create(g, &td);
    }
    REQUIRE(HANDLE_VALID(test.storage_target), "storage target: %s", gpu_last_error(g));
    tex = target_texture(g, test.storage_target);
    REQUIRE(HANDLE_VALID(tex), "target -> texture bridge");
    CHECK(texture_info(g, tex, &bindless) && bindless == 0u,
          "first texture is bindless index 0 (got %u)", bindless);

    {
        TargetDesc td;
        memset(&td, 0, sizeof td);
        td.name = "color_target";
        td.width = W; td.height = H;
        td.format = FMT_RGBA8_UNORM;
        td.usage = TGT_COLOR | TGT_SAMPLED;
        td.samples = SC_1; td.cls = RES_PERSISTENT;
        test.color_target = target_create(g, &td);
    }
    REQUIRE(HANDLE_VALID(test.color_target), "color target: %s", gpu_last_error(g));

    /* A texture with initial_data exercises upload_texture + its own submit. */
    memset(initial_pixels, 0, sizeof initial_pixels);
    for (i = 0; i < sizeof initial_pixels; ++i) initial_pixels[i] = (uint8_t)(i * 7u);
    initial = DATA_SPAN(initial_pixels, sizeof initial_pixels);
    {
        TextureDesc td;
        memset(&td, 0, sizeof td);
        td.name = "uploaded";
        td.width = 4u; td.height = 4u; td.depth = 1u; td.layers = 1u; td.mips = 1u;
        td.format = FMT_RGBA8_UNORM;
        td.usage = TEX_SAMPLED | TEX_XFER_DST;
        td.cls = RES_PERSISTENT;
        td.initial_data = &initial;
        CHECK(HANDLE_VALID(texture_create(g, &td)), "texture_create(upload): %s",
              gpu_last_error(g));
    }

    sd = sampler_linear_clamp();
    sd.max_aniso = 4u;
    samp = sampler_create(g, &sd);
    REQUIRE(HANDLE_VALID(samp), "sampler_create: %s", gpu_last_error(g));
    {
        Sampler temporary, replacement;
        uint32_t first = sampler_bindless_index(g, samp);
        uint32_t temporary_index, replacement_index;
        temporary = sampler_create(g, &sd);
        REQUIRE(HANDLE_VALID(temporary), "second sampler_create: %s", gpu_last_error(g));
        temporary_index = sampler_bindless_index(g, temporary);
        CHECK(first != temporary_index, "live samplers have unique bindless slots");
        sampler_destroy(g, temporary);
        replacement = sampler_create(g, &sd);
        REQUIRE(HANDLE_VALID(replacement), "replacement sampler_create: %s", gpu_last_error(g));
        replacement_index = sampler_bindless_index(g, replacement);
        CHECK(temporary_index != replacement_index,
              "retired sampler slot is not reused before timeline completion");
        sampler_destroy(g, replacement);
    }

    /* upload_buffer with <= 64 KB takes the vkCmdUpdateBuffer path. */
    for (i = 0; i < sizeof upload_data; ++i) upload_data[i] = (uint8_t)(i ^ 0xA5u);
    upload_src = DATA_SPAN(upload_data, sizeof upload_data);
    devslice.buffer = devbuf; devslice.offset = 0u; devslice.size = sizeof upload_data;
    up = upload_begin(g);
    REQUIRE(up != NULL, "upload_begin: %s", gpu_last_error(g));
    upload_buffer(up, devslice, upload_src);
    upload_end(g, up);

    /* ---- arenas ---------------------------------------------------------- */
    arena_slices[0] = arena_alloc(g, ARENA_DEVICE, 4096u, 64u);
    arena_slices[1] = arena_alloc(g, ARENA_TRANSIENT, 1024u, 64u);
    arena_slices[2] = arena_alloc(g, ARENA_STAGING, 512u, 16u);
    CHECK(HANDLE_VALID(arena_slices[0].buffer) && arena_slices[0].size == 4096u,
          "arena_alloc DEVICE");
    CHECK(HANDLE_VALID(arena_slices[1].buffer), "arena_alloc TRANSIENT");
    CHECK(HANDLE_VALID(arena_slices[2].buffer), "arena_alloc STAGING");
    arena_stats(g, ARENA_DEVICE, &ast);
    CHECK(ast.reserved_bytes == (64u << 20) && ast.allocations == 1u,
          "arena_stats DEVICE");

    /* ---- shaders + pipelines --------------------------------------------- */
    memset(&src, 0, sizeof src);
    src.path = "tests/smoke_cs.spv";
    src.entry = "main";
    src.stage = STAGE_COMPUTE;
    REQUIRE(shader_compile_in_process(g, &src, &cs_id),
            "compile compute: %s", gpu_last_error(g));
    memset(&src, 0, sizeof src);
    src.path = "tests/smoke_triangle.spv";
    src.entry = "vs_main";
    src.stage = STAGE_VERTEX;
    REQUIRE(shader_compile_in_process(g, &src, &tri_id),
            "compile triangle: %s", gpu_last_error(g));

    test.compute_pipe = pipe_intern_compute(g, cs_id, "main");
    REQUIRE(HANDLE_VALID(test.compute_pipe), "compute pipe: %s", gpu_last_error(g));
    memset(&key, 0, sizeof key);
    key.vs = tri_id; key.fs = tri_id;
    key.vs_entry = "vs_main"; key.fs_entry = "fs_main";
    key.color[0] = FMT_RGBA8_UNORM; key.color_count = 1u;
    key.depth = FMT_NONE;
    test.gfx_pipe = pipe_intern(g, &key);
    REQUIRE(HANDLE_VALID(test.gfx_pipe), "graphics pipe: %s", gpu_last_error(g));
    found = pipe_find(g, &key);
    CHECK(found.index == test.gfx_pipe.index && found.gen == test.gfx_pipe.gen,
          "pipe_intern is idempotent");
    CHECK(pipe_count(g) == 2u, "pipeline budget count (got %u)", pipe_count(g));
    pipe_stats(g, &pipelines, &compiled);
    CHECK(pipelines == 2u, "pipe_stats pipelines");
    CHECK(compiled == 0u, "no pipeline compiled inside a frame");

    /* ---- frame program --------------------------------------------------- */
    memset(passes, 0, sizeof passes);
    buf_write = use_write(tables.storage_buffers.buffer, 0u, 256u);
    buf_read = use_read(tables.storage_buffers.buffer, 0u, 256u);
    color_att = attach_clear(test.color_target, 0.1f, 0.2f, 0.3f, 1.0f);

    strcpy(passes[0].name, "cs_write");
    passes[0].kind = PASS_COMPUTE;
    passes[0].pipe = test.compute_pipe;
    passes[0].state = draw_state_opaque(SC_1, CULL_NONE);
    passes[0].tex_writes = &test.storage_target;
    passes[0].tex_write_count = 1u;
    passes[0].buf_writes = &buf_write;
    passes[0].buf_write_count = 1u;
    passes[0].record = record_compute;

    strcpy(passes[1].name, "gfx_triangle");
    passes[1].kind = PASS_GRAPHICS;
    passes[1].pipe = test.gfx_pipe;
    passes[1].state = draw_state_opaque(SC_1, CULL_NONE);
    passes[1].colors = &color_att;
    passes[1].color_count = 1u;
    passes[1].record = record_triangle;

    strcpy(passes[2].name, "cs_read");
    passes[2].kind = PASS_COMPUTE;
    passes[2].pipe = test.compute_pipe;
    passes[2].state = draw_state_opaque(SC_1, CULL_NONE);
    passes[2].buf_reads = &buf_read;
    passes[2].buf_read_count = 1u;
    passes[2].record = record_compute_read;

    test.g = g;
    test.frames = 0u;
    prog = frame_program_build(g, passes, 3u);
    REQUIRE(prog != NULL, "frame_program_build: %s", gpu_last_error(g));
    frame_program_report(g, prog, "tests/smoke_report.txt");

    /* size == 0 must be rejected: the fast path has no whole-buffer shorthand */
    {
        PassDesc bad;
        BufUse zero = use_read(tables.storage_buffers.buffer, 0u, 0u);
        memset(&bad, 0, sizeof bad);
        strcpy(bad.name, "bad_zero_range");
        bad.kind = PASS_COMPUTE;
        bad.record = record_compute;
        bad.buf_reads = &zero; bad.buf_read_count = 1u;
        CHECK(frame_program_build(g, &bad, 1u) == NULL,
              "zero-sized BufUse rejected at build");
    }

    /* ---- run real frames ------------------------------------------------- */
    for (i = 0; i < 3u; ++i) {
        REQUIRE(frame_begin(g), "frame_begin %u: %s", i, gpu_last_error(g));
        frame_run(g, prog, &test);
        frame_end(g);
    }
    CHECK(test.frames == 3u, "triangle pass ran 3 times (got %u)", test.frames);

    /* ---- verify REAL GPU output through readback -------------------------- */
    memset(readback, 0, sizeof readback);
    rb = readback_request(g, tex, DATA_SPAN(readback, sizeof readback), 0u);
    REQUIRE(rb != NULL, "readback_request: %s", gpu_last_error(g));
    REQUIRE(readback_ready(g, rb, true), "readback_ready(blocking)");
    printf("readback pixel[0]  = %u %u %u %u\n",
           readback[0], readback[1], readback[2], readback[3]);
    printf("readback pixel[63] = %u %u %u %u\n",
           readback[63u * 4u + 0u], readback[63u * 4u + 1u],
           readback[63u * 4u + 2u], readback[63u * 4u + 3u]);
    CHECK(abs((int)readback[1] - 128) <= 2 && abs((int)readback[2] - 64) <= 2 &&
          abs((int)readback[3] - 255) <= 2, "green/blue/alpha ramp from compute shader");
    CHECK(readback[0] <= 2, "red at x=0 is 0");
    CHECK(readback[63u * 4u] >= 250, "red at x=63 is 255");
    readback_release(g, rb);

    /* ---- buffer readback: what the compute shader left in the storage buffer */
    {
        uint32_t  words[64], small[4];
        BufSlice  range = {tables.storage_buffers.buffer, 0u, sizeof words};
        BufSlice  beyond = {tables.storage_buffers.buffer, tables.storage_buffers.size - 8u, 16u};
        Readback *brb;
        uint32_t  k, wrong = 0u;
        memset(words, 0, sizeof words);
        brb = readback_request_buffer(g, range, DATA_SPAN(words, sizeof words));
        REQUIRE(brb != NULL, "readback_request_buffer: %s", gpu_last_error(g));
        REQUIRE(readback_ready(g, brb, true), "buffer readback_ready(blocking)");
        for (k = 0u; k < 64u; ++k) if (words[k] != k * 2u + 1u) ++wrong;
        printf("buffer readback    = %u %u %u ... %u\n", words[0], words[1], words[2], words[63]);
        CHECK(wrong == 0u, "compute wrote g_storage[i] = 2i + 1 (%u of 64 wrong)", wrong);
        readback_release(g, brb);

        /* a sub-range at a non-zero offset: words 10..13 */
        range.offset = 40u; range.size = sizeof small;
        memset(small, 0, sizeof small);
        brb = readback_request_buffer(g, range, DATA_SPAN(small, sizeof small));
        REQUIRE(brb != NULL && readback_ready(g, brb, true), "buffer readback at an offset");
        CHECK(small[0] == 21u && small[3] == 27u, "offset 40 reads words 10..13 (got %u..%u)", small[0], small[3]);
        readback_release(g, brb);

        /* rejected: a range past the end, a destination smaller than the slice */
        CHECK(readback_request_buffer(g, beyond, DATA_SPAN(words, sizeof words)) == NULL,
              "buffer readback past the end is rejected");
        range.offset = 0u; range.size = sizeof words;
        CHECK(readback_request_buffer(g, range, DATA_SPAN(small, sizeof small)) == NULL,
              "buffer readback into a too-small destination is rejected");
    }

    /* ---- basic timing and command statistics ---------------------------- */
    perf_frame_stats(g, &fs);
    printf("frame: cpu %.3f ms gpu %.3f ms passes %u draws %u dispatches %u barriers %u\n",
           fs.cpu_ms, fs.gpu_ms, fs.pass_count, fs.draws, fs.dispatches, fs.barriers);
    CHECK(fs.pass_count == 3u, "three passes recorded");
    CHECK(fs.dispatches >= 2u, "dispatches recorded");
    CHECK(fs.declared_bytes > 0u, "declared logical traffic estimate is reported");
    CHECK(fs.gpu_ms > 0.0, "timestamp queries resolved (gpu_ms %.3f)", fs.gpu_ms);
    CHECK(fs.passes[0].samples == 1u && fs.passes[0].effective_samples == 1u &&
          fs.passes[0].steps_per_pixel == 1.0,
          "caller workload annotations are reported");
    CHECK(fs.passes[0].workgroup_x == 64u && fs.passes[0].workgroup_y == 1u &&
          fs.passes[0].workgroup_z == 1u,
          "dispatch shape annotation is reported");
    CHECK(fs.scope_count == 1u && fs.scopes[0].complete &&
          fs.scopes[0].timing_available,
          "intra-pass scope timestamp resolves");
    frame_program_report(g, prog, "tests/smoke_report_after.txt");
    perf_dump(g, "tests/smoke_perf.txt");

    CHECK(gpu_validate(g), "gpu_validate");

    /* ---- 3D textures ------------------------------------------------------
     * Created and read back here; written and sampled by shaders in
     * demos/sdfworld, whose --check replays the result on the CPU. */
    {
        static uint16_t volume_back[4 * 3 * 2 * 4];
        uint16_t   pattern[4 * 3 * 2 * 4];
        DataSpan   span = DATA_SPAN(pattern, sizeof pattern);
        TextureDesc vd;
        TargetDesc  td;
        Texture     volume;
        Target      volume_target;
        Readback   *vrb;
        uint32_t    i, wrong = 0u, tries;
        for (i = 0u; i < 4u * 3u * 2u * 4u; ++i) pattern[i] = (uint16_t)(i * 37u + 5u);
        memset(&vd, 0, sizeof vd);
        vd.name = "smoke volume"; vd.width = 4u; vd.height = 3u; vd.depth = 2u;
        vd.format = FMT_RGBA16F; vd.usage = TEX_SAMPLED | TEX_XFER_SRC | TEX_XFER_DST;
        vd.cls = RES_PERSISTENT; vd.initial_data = &span;
        volume = texture_create(g, &vd);
        CHECK(HANDLE_VALID(volume), "texture_create(3D): %s", gpu_last_error(g));
        vrb = readback_request(g, volume, DATA_SPAN(volume_back, sizeof volume_back), 0u);
        CHECK(vrb != NULL, "readback_request(3D): %s", gpu_last_error(g));
        for (tries = 0u; vrb && tries < 120u && !readback_ready(g, vrb, true); ++tries) {}
        if (vrb) readback_release(g, vrb);
        for (i = 0u; i < 4u * 3u * 2u * 4u; ++i) wrong += volume_back[i] != pattern[i];
        CHECK(wrong == 0u, "a 4 x 3 x 2 volume reads back what was uploaded, both slices (%u of 96 values wrong)", wrong);

        memset(&td, 0, sizeof td);
        td.name = "smoke volume target"; td.width = 8u; td.height = 8u; td.depth = 8u;
        td.format = FMT_RGBA16F; td.usage = TGT_SAMPLED | TGT_STORAGE; td.samples = SC_1; td.cls = RES_PERSISTENT;
        volume_target = target_create(g, &td);
        CHECK(HANDLE_VALID(volume_target), "target_create(3D storage): %s", gpu_last_error(g));
        td.usage = TGT_SAMPLED | TGT_COLOR;
        CHECK(!HANDLE_VALID(target_create(g, &td)), "a 3D target cannot be a colour attachment");
        vd.layers = 2u; vd.initial_data = NULL;
        CHECK(!HANDLE_VALID(texture_create(g, &vd)), "a 3D texture cannot have layers");
        target_destroy(g, volume_target);
        texture_destroy(g, volume);
    }

    /* ---- teardown -------------------------------------------------------- */
    frame_program_destroy(g, prog);
    buffer_destroy(g, devbuf);
    target_destroy(g, test.storage_target);
    target_destroy(g, test.color_target);
    sampler_destroy(g, samp);
    gpu_destroy(g);

    printf("\n%s (%d failure%s)\n", failures ? "SMOKE FAILED" : "ALL CHECKS PASSED",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
