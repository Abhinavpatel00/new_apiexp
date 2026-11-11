/* nk_vk.c — Nuklear backend on vk.h (see nk_vk.h for the design).
 *
 * Nuklear implementation lives here (NK_IMPLEMENTATION, exactly one TU).
 * Vertex layout for nk_convert: position float[2], uv float[2],
 * colour R8G8B8A8 (nk_byte[4]) — 20 B. Output SSBO vertex is 32 B
 * (colour expanded to float[4]); de-indexing happens on the CPU.
 */

#define NK_IMPLEMENTATION
#include "nk_vk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Max UI vertices per frame (de-indexed triangles = verts/3).
 * 65536 verts = 2 MiB SSBO — debug UI with a few windows is far below this. */
#define NKVK_MAX_VERTS 65536u
#define NKVK_MAX_BYTES (NKVK_MAX_VERTS * sizeof(NkVkVertex))
#define NKVK_MAX_BATCHES 128u

/* nk_convert input vertex (20 B, must match vertex_layout below). */
typedef struct NkVkConvertVertex {
    float pos[2];
    float uv[2];
    nk_byte col[4];
} NkVkConvertVertex;

struct NkVk {
    struct nk_context ctx;
    struct nk_font_atlas *atlas;
    struct nk_draw_null_texture tex_null;
    struct nk_buffer cmds; /* converted draw commands (default allocator) */

    Pipe pipe; /* interned; process-lifetime, no destroy */
    Texture font_tex;
    Sampler sampler;
    uint32_t font_index;    /* bindless index of font atlas */
    uint32_t sampler_index; /* linear sampler bindless index */
    BufSlice verts;         /* SSBO slice (ARENA_DEVICE, max size) */
    uint32_t triangles;     /* triangles to draw this frame */
    /* One batch per run of draw commands that share a clip rectangle. vk.h has
     * no scissor state, so the rectangle travels in the push block and the
     * fragment stage discards outside it. */
    struct { uint32_t first_vertex, triangles; float clip[4]; } batches[NKVK_MAX_BATCHES];
    uint32_t batch_count;
    uint32_t width, height;
    bool visible;
    NkVkVertex *cpu_verts;  /* staging for de-indexed verts (malloc) */
};

NkVk *nkvk_create(Gpu *g, const char *vs_spv, const char *fs_spv,
                  Format color_format, uint32_t width, uint32_t height) {
    NkVk *vk = (NkVk *)calloc(1, sizeof(NkVk));
    if (!vk) return NULL;
    vk->width = width;
    vk->height = height;
    vk->visible = true;

    /* --- font atlas (default ProggyClean, baked RGBA32) --- */
    vk->atlas = (struct nk_font_atlas *)calloc(1, sizeof(struct nk_font_atlas));
    if (!vk->atlas) { free(vk); return NULL; }
    nk_font_atlas_init_default(vk->atlas);
    nk_font_atlas_begin(vk->atlas);
    struct nk_font *font = nk_font_atlas_add_default(vk->atlas, 13.0f, NULL);
    int bake_w = 0, bake_h = 0;
    const void *pixels = nk_font_atlas_bake(vk->atlas, &bake_w, &bake_h,
                                            NK_FONT_ATLAS_RGBA32);
    if (!pixels || bake_w <= 0 || bake_h <= 0) {
        fprintf(stderr, "nk_vk: font atlas bake failed\n");
        nk_font_atlas_clear(vk->atlas);
        free(vk->atlas);
        free(vk);
        return NULL;
    }

    /* --- font texture (bindless sampled image, RGBA8) --- */
    TextureDesc td = {0};
    td.name = "nk_font";
    td.width = (uint32_t)bake_w;
    td.height = (uint32_t)bake_h;
    td.depth = 1u;
    td.layers = 1u;
    td.mips = 1u;
    td.format = FMT_RGBA8_UNORM;
    td.usage = TEX_SAMPLED | TEX_XFER_DST;
    td.cls = RES_PERSISTENT;
    DataSpan font_data = DATA_SPAN(pixels, (uint64_t)bake_w * (uint64_t)bake_h * 4u);
    td.initial_data = &font_data;
    vk->font_tex = texture_create(g, &td);
    if (!HANDLE_VALID(vk->font_tex)) {
        fprintf(stderr, "nk_vk: font texture_create failed: %s\n",
                gpu_last_error(g));
        nk_font_atlas_clear(vk->atlas);
        free(vk->atlas);
        free(vk);
        return NULL;
    }
    if (!texture_info(g, vk->font_tex, &vk->font_index)) {
        fprintf(stderr, "nk_vk: texture_info failed\n");
        texture_destroy(g, vk->font_tex);
        nk_font_atlas_clear(vk->atlas);
        free(vk->atlas);
        free(vk);
        return NULL;
    }

    /* --- linear sampler (bindless) --- */
    SamplerDesc sd = sampler_linear_clamp();
    vk->sampler = sampler_create(g, &sd);
    if (!HANDLE_VALID(vk->sampler)) {
        fprintf(stderr, "nk_vk: sampler_create failed: %s\n",
                gpu_last_error(g));
        texture_destroy(g, vk->font_tex);
        nk_font_atlas_clear(vk->atlas);
        free(vk->atlas);
        free(vk);
        return NULL;
    }
    vk->sampler_index = sampler_bindless_index(g, vk->sampler);

    /* --- atlas end: fills tex_null (white pixel for solid shapes) --- */
    nk_font_atlas_end(vk->atlas, nk_handle_id((int)vk->font_index),
                      &vk->tex_null);

    /* --- context --- */
    if (!nk_init_default(&vk->ctx, &font->handle)) {
        fprintf(stderr, "nk_vk: nk_init_default failed\n");
        sampler_destroy(g, vk->sampler);
        texture_destroy(g, vk->font_tex);
        nk_font_atlas_clear(vk->atlas);
        free(vk->atlas);
        free(vk);
        return NULL;
    }
    nk_buffer_init_default(&vk->cmds);

    /* --- SSBO slice for de-indexed verts (device arena, max size) --- */
    vk->verts = arena_alloc(g, ARENA_DEVICE, NKVK_MAX_BYTES, 64u);
    if (!HANDLE_VALID(vk->verts.buffer)) {
        fprintf(stderr, "nk_vk: arena_alloc failed: %s\n", gpu_last_error(g));
        nk_buffer_free(&vk->cmds);
        nk_free(&vk->ctx);
        sampler_destroy(g, vk->sampler);
        texture_destroy(g, vk->font_tex);
        nk_font_atlas_clear(vk->atlas);
        free(vk->atlas);
        free(vk);
        return NULL;
    }
    vk->cpu_verts = (NkVkVertex *)malloc(NKVK_MAX_BYTES);
    if (!vk->cpu_verts) {
        nk_buffer_free(&vk->cmds);
        nk_free(&vk->ctx);
        sampler_destroy(g, vk->sampler);
        texture_destroy(g, vk->font_tex);
        nk_font_atlas_clear(vk->atlas);
        free(vk->atlas);
        free(vk);
        return NULL;
    }

    /* --- pipe (no vertex input; geometry fetched by SV_VertexID) --- */
    ShaderId vs, fs;
    if (!shader_load(g, vs_spv, STAGE_VERTEX, &vs) ||
        !shader_load(g, fs_spv, STAGE_FRAGMENT, &fs)) {
        fprintf(stderr, "nk_vk: shader load failed: %s\n", gpu_last_error(g));
        free(vk->cpu_verts);
        nk_buffer_free(&vk->cmds);
        nk_free(&vk->ctx);
        sampler_destroy(g, vk->sampler);
        texture_destroy(g, vk->font_tex);
        nk_font_atlas_clear(vk->atlas);
        free(vk->atlas);
        free(vk);
        return NULL;
    }
    vk->pipe = pipe_intern_graphics(g, vs, fs, color_format, FMT_NONE, BLEND_ALPHA);
    if (!HANDLE_VALID(vk->pipe)) {
        fprintf(stderr, "nk_vk: pipe_intern failed: %s\n", gpu_last_error(g));
        free(vk->cpu_verts);
        nk_buffer_free(&vk->cmds);
        nk_free(&vk->ctx);
        sampler_destroy(g, vk->sampler);
        texture_destroy(g, vk->font_tex);
        nk_font_atlas_clear(vk->atlas);
        free(vk->atlas);
        free(vk);
        return NULL;
    }
    return vk;
}

void nkvk_destroy(NkVk *vk, Gpu *g) {
    if (!vk) return;
    /* pipe is interned (process-lifetime); no destroy */
    if (HANDLE_VALID(vk->sampler)) sampler_destroy(g, vk->sampler);
    if (HANDLE_VALID(vk->font_tex)) texture_destroy(g, vk->font_tex);
    free(vk->cpu_verts);
    nk_buffer_free(&vk->cmds);
    nk_free(&vk->ctx);
    nk_font_atlas_clear(vk->atlas);
    free(vk->atlas);
    free(vk);
}

struct nk_context *nkvk_context(NkVk *vk) { return vk ? &vk->ctx : NULL; }
Pipe nkvk_pipe(NkVk *vk) { return vk ? vk->pipe : (Pipe){0}; }
uint32_t nkvk_triangle_count(NkVk *vk) { return vk ? vk->triangles : 0u; }

BufUse nkvk_buf_use(NkVk *vk) {
    BufUse u = {0};
    if (!vk) return u;
    u.slice = vk->verts;
    u.slice.size = NKVK_MAX_BYTES;
    u.access = ACC_READ;
    return u;
}

bool nkvk_visible(NkVk *vk) { return vk && vk->visible; }
void nkvk_set_visible(NkVk *vk, bool visible) { if (vk) vk->visible = visible; }
bool nkvk_toggle_visible(NkVk *vk) {
    if (!vk) return false;
    vk->visible = !vk->visible;
    return vk->visible;
}

void nkvk_new_frame(NkVk *vk, const MuInputState *state, float dt) {
    if (!vk) return;
    struct nk_context *ctx = &vk->ctx;
    ctx->delta_time_seconds = dt;
    nk_input_begin(ctx);
    if (state) {
        int mx = (int)state->mouse_x;
        int my = (int)state->mouse_y;
        nk_input_motion(ctx, mx, my);
        nk_input_button(ctx, NK_BUTTON_LEFT, mx, my,
                        mu_input_mouse_down(state, MU_MOUSE_LEFT) ? nk_true : nk_false);
        nk_input_button(ctx, NK_BUTTON_MIDDLE, mx, my,
                        mu_input_mouse_down(state, MU_MOUSE_MIDDLE) ? nk_true : nk_false);
        nk_input_button(ctx, NK_BUTTON_RIGHT, mx, my,
                        mu_input_mouse_down(state, MU_MOUSE_RIGHT) ? nk_true : nk_false);
        /* keys Nuklear cares about (navigation / editing) */
        nk_input_key(ctx, NK_KEY_LEFT, mu_input_key_down(state, MU_KEY_LEFT) ? nk_true : nk_false);
        nk_input_key(ctx, NK_KEY_RIGHT, mu_input_key_down(state, MU_KEY_RIGHT) ? nk_true : nk_false);
        nk_input_key(ctx, NK_KEY_UP, mu_input_key_down(state, MU_KEY_UP) ? nk_true : nk_false);
        nk_input_key(ctx, NK_KEY_DOWN, mu_input_key_down(state, MU_KEY_DOWN) ? nk_true : nk_false);
        nk_input_key(ctx, NK_KEY_BACKSPACE, mu_input_key_down(state, MU_KEY_BACKSPACE) ? nk_true : nk_false);
        nk_input_key(ctx, NK_KEY_DEL, mu_input_key_down(state, MU_KEY_DELETE) ? nk_true : nk_false);
        nk_input_key(ctx, NK_KEY_ENTER, mu_input_key_down(state, MU_KEY_ENTER) ? nk_true : nk_false);
        nk_input_key(ctx, NK_KEY_TAB, mu_input_key_down(state, MU_KEY_TAB) ? nk_true : nk_false);
        nk_input_key(ctx, NK_KEY_SHIFT, mu_input_key_down(state, MU_KEY_LEFT_SHIFT) ? nk_true : nk_false);
        nk_input_key(ctx, NK_KEY_CTRL, mu_input_key_down(state, MU_KEY_LEFT_CTRL) ? nk_true : nk_false);
        nk_input_key(ctx, NK_KEY_ALT, mu_input_key_down(state, MU_KEY_LEFT_ALT) ? nk_true : nk_false);
        nk_input_key(ctx, NK_KEY_TEXT_START, mu_input_key_down(state, MU_KEY_HOME) ? nk_true : nk_false);
        nk_input_key(ctx, NK_KEY_TEXT_END, mu_input_key_down(state, MU_KEY_END) ? nk_true : nk_false);
        nk_input_key(ctx, NK_KEY_TEXT_INSERT_MODE, mu_input_key_down(state, MU_KEY_INSERT) ? nk_true : nk_false);
        nk_input_key(ctx, NK_KEY_SCROLL_UP, mu_input_key_down(state, MU_KEY_PAGE_UP) ? nk_true : nk_false);
        nk_input_key(ctx, NK_KEY_SCROLL_DOWN, mu_input_key_down(state, MU_KEY_PAGE_DOWN) ? nk_true : nk_false);
        nk_input_key(ctx, NK_KEY_F1, mu_input_key_down(state, MU_KEY_F1) ? nk_true : nk_false);
        /* wheel: feed scroll (galaxy skips its own zoom while UI is visible) */
        if (state->wheel_x != 0.0f || state->wheel_y != 0.0f)
            nk_input_scroll(ctx, nk_vec2(state->wheel_x, state->wheel_y));
    }
    nk_input_end(ctx);
}

void nkvk_prepare(NkVk *vk, Gpu *g, uint32_t width, uint32_t height) {
    if (!vk) return;
    vk->triangles = 0u;
    vk->width = width;
    vk->height = height;
    if (!vk->visible) {
        nk_clear(&vk->ctx);
        return;
    }

    static const struct nk_draw_vertex_layout_element vertex_layout[] = {
        {NK_VERTEX_POSITION, NK_FORMAT_FLOAT, NK_OFFSETOF(NkVkConvertVertex, pos)},
        {NK_VERTEX_TEXCOORD, NK_FORMAT_FLOAT, NK_OFFSETOF(NkVkConvertVertex, uv)},
        {NK_VERTEX_COLOR, NK_FORMAT_R8G8B8A8, NK_OFFSETOF(NkVkConvertVertex, col)},
        {NK_VERTEX_LAYOUT_END},
    };
    struct nk_convert_config config;
    memset(&config, 0, sizeof config);
    config.vertex_layout = vertex_layout;
    config.vertex_size = sizeof(NkVkConvertVertex);
    config.vertex_alignment = NK_ALIGNOF(NkVkConvertVertex);
    config.tex_null = vk->tex_null;
    config.circle_segment_count = 22;
    config.curve_segment_count = 22;
    config.arc_segment_count = 22;
    config.global_alpha = 1.0f;
    config.shape_AA = NK_ANTI_ALIASING_ON;
    config.line_AA = NK_ANTI_ALIASING_ON;

    struct nk_buffer vbuf, ebuf;
    /* Fixed CPU buffers for nk_convert output. Sized to the max; nk_convert
     * fills them and we de-index immediately. */
    static NkVkConvertVertex s_vbuf[NKVK_MAX_VERTS];
    static nk_draw_index s_ebuf[NKVK_MAX_VERTS * 3u];
    nk_buffer_init_fixed(&vbuf, s_vbuf, sizeof s_vbuf);
    nk_buffer_init_fixed(&ebuf, s_ebuf, sizeof s_ebuf);
    nk_flags result = nk_convert(&vk->ctx, &vk->cmds, &vbuf, &ebuf, &config);
    if (result != NK_CONVERT_SUCCESS) {
        fprintf(stderr, "nk_vk: nk_convert failed (%d)\n", (int)result);
        nk_clear(&vk->ctx);
        return;
    }

    const NkVkConvertVertex *verts =
        (const NkVkConvertVertex *)nk_buffer_memory(&vbuf);
    const nk_draw_index *indices =
        (const nk_draw_index *)nk_buffer_memory(&ebuf);
    uint32_t vert_count = (uint32_t)(nk_buffer_total(&vbuf) / sizeof(NkVkConvertVertex));
    uint32_t index_count = (uint32_t)(nk_buffer_total(&ebuf) / sizeof(nk_draw_index));

    /* De-index: one output vertex per index (non-indexed triangles), walking
     * the draw commands so each keeps its clip rectangle. */
    uint32_t out = 0u;
    uint32_t first_index = 0u;
    const struct nk_draw_command *cmd;
    vk->batch_count = 0u;
    nk_draw_foreach(cmd, &vk->ctx, &vk->cmds) {
        const uint32_t count = (uint32_t)cmd->elem_count - (uint32_t)cmd->elem_count % 3u;
        const float clip[4] = {cmd->clip_rect.x, cmd->clip_rect.y,
                               cmd->clip_rect.x + cmd->clip_rect.w,
                               cmd->clip_rect.y + cmd->clip_rect.h};
        const uint32_t begin = out;
        uint32_t i;
        if (!cmd->elem_count) continue;
        if (first_index + count > index_count || out + count > NKVK_MAX_VERTS) break;
        for (i = 0u; i < count; ++i) {
            uint32_t vi = (uint32_t)indices[first_index + i];
            NkVkVertex *dst = &vk->cpu_verts[out++];
            if (vi >= vert_count) vi = 0u;
            dst->pos[0] = verts[vi].pos[0];
            dst->pos[1] = verts[vi].pos[1];
            dst->uv[0] = verts[vi].uv[0];
            dst->uv[1] = verts[vi].uv[1];
            dst->col[0] = (float)verts[vi].col[0] / 255.0f;
            dst->col[1] = (float)verts[vi].col[1] / 255.0f;
            dst->col[2] = (float)verts[vi].col[2] / 255.0f;
            dst->col[3] = (float)verts[vi].col[3] / 255.0f;
        }
        first_index += (uint32_t)cmd->elem_count;
        if (vk->batch_count &&
            !memcmp(vk->batches[vk->batch_count - 1u].clip, clip, sizeof clip)) {
            vk->batches[vk->batch_count - 1u].triangles += count / 3u;
        } else if (vk->batch_count < NKVK_MAX_BATCHES) {
            vk->batches[vk->batch_count].first_vertex = begin;
            vk->batches[vk->batch_count].triangles = count / 3u;
            memcpy(vk->batches[vk->batch_count].clip, clip, sizeof clip);
            ++vk->batch_count;
        } else {
            out = begin; /* out of batches: drop the rest rather than draw it unclipped */
            break;
        }
    }
    vk->triangles = out / 3u;

    if (out > 0u) {
        Upload *up = upload_begin(g);
        if (up) {
            BufSlice dst = vk->verts;
            dst.size = (uint64_t)out * sizeof(NkVkVertex);
            upload_buffer(up, dst, DATA_SPAN(vk->cpu_verts, (uint64_t)out * sizeof(NkVkVertex)));
            upload_end(g, up);
        }
    }
    nk_clear(&vk->ctx);
}

void nkvk_draw(NkVk *vk, PassCtx *p) {
    uint32_t i;
    if (!vk || !p || vk->triangles == 0u) return;
    for (i = 0u; i < vk->batch_count; ++i) {
        NkVkPush push = {0};
        /* The batch start goes into the offset, not into first_instance: Slang's
         * SV_InstanceID excludes the base instance. */
        push.vert_offset = (uint32_t)vk->verts.offset +
                           vk->batches[i].first_vertex * (uint32_t)sizeof(NkVkVertex);
        push.texture_index = vk->font_index;
        push.sampler_index = vk->sampler_index;
        push.width = (float)vk->width;
        push.height = (float)vk->height;
        memcpy(push.clip, vk->batches[i].clip, sizeof push.clip);
        pass_push(p, &push, sizeof push);
        pass_draw(p, 3u, vk->batches[i].triangles, 0u, 0u);
    }
}
