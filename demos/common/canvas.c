/* canvas.c — see canvas.h. */
#include "canvas.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct CanvasPush {
    float    vp[16];
    uint32_t offset, pad[3];
} CanvasPush;
_Static_assert(sizeof(CanvasPush) == 80, "CanvasPush must match canvas.slang");

void canvas_open(Demo *d, Canvas *c, uint32_t max_vertices, uint32_t background) {
    Target   hdr = demo_target(d, "canvas_hdr", FMT_RGBA16F, d->width, d->height, false, NULL);
    Target   z = demo_target(d, "canvas_depth", FMT_D32F, d->width, d->height, false, NULL);
    PassDesc passes[3];
    /* the clear colour is linear; the bytes are what the screen shows */
    const float r = powf((float)(background & 255u) / 255.0f, 2.2f), g = powf((float)(background >> 8 & 255u) / 255.0f, 2.2f),
                b = powf((float)(background >> 16 & 255u) / 255.0f, 2.2f);

    c->capacity   = max_vertices;
    c->count      = 0u;
    c->vertices   = (CanvasVertex *)malloc((size_t)max_vertices * sizeof(CanvasVertex));
    c->slice      = demo_alloc(d, NULL, (uint64_t)max_vertices * sizeof(CanvasVertex));
    c->use        = use_read(c->slice.buffer, c->slice.offset, c->slice.size);
    c->use.stages = SH_VERT;
    c->depth      = attach_depth_clear(z, 1.0f);
    passes[0] = (PassDesc){
        .name = "Draw", .kind = PASS_GRAPHICS, .state = draw_state_opaque(SC_1, CULL_NONE),
        .pipe = pipe_intern_graphics(d->gpu, demo_shader(d, "demos/common/shaders/canvas.vert.spv", STAGE_VERTEX),
                                     demo_shader(d, "demos/common/shaders/canvas.frag.spv", STAGE_FRAGMENT), FMT_RGBA16F, FMT_D32F, BLEND_OPAQUE),
        .colors = (AttachDesc[]){attach_clear(hdr, r, g, b, 1.0f)}, .color_count = 1u, .depth = &c->depth,
        .buf_reads = &c->use, .buf_read_count = 1u};
    passes[1] = demo_present_pass(d, hdr);
    d->program         = demo_program(d, passes, 2u);
    d->present.tonemap = DEMO_TONEMAP_NONE;
}

void canvas_view_orbit(Demo *d, Canvas *c, DemoOrbit *orbit) {
    float view[16];
    int   i;
    demo_orbit_input(d, orbit);
    demo_orbit_camera(orbit, (float)d->width / (float)d->height, view, NULL, c->vp, c->eye);
    /* Rows of the rotation part of a column-major view matrix are the camera axes. */
    for (i = 0; i < 3; ++i) { c->right[i] = view[4 * i]; c->up[i] = view[4 * i + 1]; }
    c->flat = false;
}

void canvas_view_flat(Demo *d, Canvas *c, float cx, float cy, float half_height) {
    const float half_width = half_height * (float)d->width / (float)d->height;
    memset(c->vp, 0, sizeof c->vp);
    c->vp[0]  = 1.0f / half_width;
    c->vp[5]  = 1.0f / half_height;
    c->vp[10] = -0.005f;   /* z from -100 to 100 fits the depth range; larger z is nearer */
    c->vp[12] = -cx / half_width;
    c->vp[13] = -cy / half_height;
    c->vp[14] = 0.5f;
    c->vp[15] = 1.0f;
    c->right[0] = 1.0f; c->right[1] = 0.0f; c->right[2] = 0.0f;
    c->up[0]    = 0.0f; c->up[1]    = 1.0f; c->up[2]    = 0.0f;
    c->flat = true;
}

void canvas_triangle(Canvas *c, const float p0[3], const float p1[3], const float p2[3], uint32_t rgba) {
    CanvasVertex *v = &c->vertices[c->count];
    if (c->count + 3u > c->capacity) return;
    memcpy(v[0].pos, p0, sizeof v[0].pos);
    memcpy(v[1].pos, p1, sizeof v[1].pos);
    memcpy(v[2].pos, p2, sizeof v[2].pos);
    v[0].rgba = v[1].rgba = v[2].rgba = rgba;
    c->count += 3u;
}

void canvas_lit(Canvas *c, const float p0[3], const float p1[3], const float p2[3], uint32_t rgba) {
    static const float k_light[3] = {0.35f, 0.8f, 0.48f};
    const float e1[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]}, e2[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
    float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
    const float length = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) + 1e-30f;
    /* the side that faces the camera is the one that is lit */
    const float facing = c->flat ? n[2] : n[0] * (c->eye[0] - p0[0]) + n[1] * (c->eye[1] - p0[1]) + n[2] * (c->eye[2] - p0[2]);
    const float cosine = (n[0] * k_light[0] + n[1] * k_light[1] + n[2] * k_light[2]) / length * (facing < 0.0f ? -1.0f : 1.0f);
    const float shade = 0.5f + 0.5f * (cosine > 0.0f ? cosine : 0.25f * cosine);
    canvas_triangle(c, p0, p1, p2,
                    CANVAS_RGB((uint32_t)((float)(rgba & 255u) * shade), (uint32_t)((float)(rgba >> 8 & 255u) * shade), (uint32_t)((float)(rgba >> 16 & 255u) * shade)));
}

void canvas_line(Canvas *c, const float p[3], const float q[3], float width, uint32_t rgba) {
    /* A quad whose short side is at right angles to the line as the camera sees it. */
    const float along[3] = {q[0] - p[0], q[1] - p[1], q[2] - p[2]};
    const float ax = along[0] * c->right[0] + along[1] * c->right[1] + along[2] * c->right[2],
                ay = along[0] * c->up[0] + along[1] * c->up[1] + along[2] * c->up[2];
    const float scale = 0.5f * width / (sqrtf(ax * ax + ay * ay) + 1e-30f);
    float side[3], corner[4][3];
    int   i;
    for (i = 0; i < 3; ++i) {
        side[i]      = scale * (-ay * c->right[i] + ax * c->up[i]);
        corner[0][i] = p[i] - side[i];
        corner[1][i] = p[i] + side[i];
        corner[2][i] = q[i] + side[i];
        corner[3][i] = q[i] - side[i];
    }
    canvas_triangle(c, corner[0], corner[1], corner[2], rgba);
    canvas_triangle(c, corner[0], corner[2], corner[3], rgba);
}

void canvas_dot(Canvas *c, const float p[3], float radius, uint32_t rgba) {
    float rim[13][3];
    int   i, k;
    for (i = 0; i <= 12; ++i)
        for (k = 0; k < 3; ++k)
            rim[i][k] = p[k] + radius * (cosf((float)i * 0.5235988f) * c->right[k] + sinf((float)i * 0.5235988f) * c->up[k]);
    for (i = 0; i < 12; ++i) canvas_triangle(c, p, rim[i], rim[i + 1], rgba);
}

void canvas_frame(Demo *d, Canvas *c) {
    CanvasPush push = {.offset = (uint32_t)c->slice.offset};
    PassCtx   *pass;
    memcpy(push.vp, c->vp, sizeof push.vp);
    demo_upload(d, c->slice, c->vertices, (uint64_t)c->count * sizeof(CanvasVertex));
    if (demo_frame_begin(d)) {
        pass = demo_pass(d, "Draw");
        pass_push(pass, &push, sizeof push);
        pass_draw(pass, c->count, 1u, 0u, 0u);
        demo_present(d);
        demo_frame_end(d);
    }
    c->count = 0u;
}
