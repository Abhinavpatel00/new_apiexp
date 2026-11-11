/* scene.c — procedural meshes + placements for the toon demo.
 *
 * Four primitive meshes are generated once (unit sphere, unit capsule, ground
 * quad, unit cube) and concatenated into one vertex array. Objects then only
 * carry {mesh range, model matrix, colour, flags}, which is exactly what the
 * demo's push constant carries per draw.
 */

#include "scene.h"

#include "mu_math.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SPHERE_SLICES 48u
#define SPHERE_STACKS 32u
#define CAPSULE_SLICES 32u
#define CAPSULE_CAP_STACKS 8u
#define PLANE_SLICES 1u
#define PLANE_STACKS 1u

typedef struct VertexBlob {
    ToonVertex *verts;
    uint32_t    count;
    uint32_t    capacity;
} VertexBlob;

static bool blob_reserve(VertexBlob *b, uint32_t extra) {
    if (b->count + extra <= b->capacity)
        return true;
    {
        uint32_t    want = b->capacity ? b->capacity * 2u : 4096u;
        ToonVertex *grown;
        while (want < b->count + extra)
            want *= 2u;
        grown = (ToonVertex *)realloc(b->verts, (size_t)want * sizeof(ToonVertex));
        if (!grown)
            return false;
        b->verts    = grown;
        b->capacity = want;
    }
    return true;
}

static bool push_vertex(VertexBlob *b, float x, float y, float z,
                        float nx, float ny, float nz, float u, float v) {
    ToonVertex *dst;
    if (!blob_reserve(b, 1u))
        return false;
    dst = &b->verts[b->count++];
    memset(dst, 0, sizeof *dst);
    dst->pos[0] = x; dst->pos[1] = y; dst->pos[2] = z; dst->pos[3] = 1.0f;
    dst->nrm[0] = nx; dst->nrm[1] = ny; dst->nrm[2] = nz;
    dst->uv[0]  = u; dst->uv[1] = v;
    return true;
}

static bool push_tri(VertexBlob *b,
                     const float a[8], const float bb[8], const float c[8]) {
    if (!push_vertex(b, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]))
        return false;
    if (!push_vertex(b, bb[0], bb[1], bb[2], bb[3], bb[4], bb[5], bb[6], bb[7]))
        return false;
    return push_vertex(b, c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7]);
}

/* Vertex 8 floats: position(3), normal(3), uv(2). */
static void sphere_point(float lat, float lon, float radius, float y_offset, float out[8]) {
    const float cl = cosf(lat), sl = sinf(lat);
    out[0] = cl * sinf(lon) * radius;
    out[1] = sl * radius + y_offset;
    out[2] = cl * cosf(lon) * radius;
    out[3] = cl * sinf(lon);
    out[4] = sl;
    out[5] = cl * cosf(lon);
    out[6] = lon / (2.0f * (float)M_PI);
    out[7] = lat / (float)M_PI + 0.5f;
}

/* Unit sphere, radius 1, centred at the origin. */
static bool add_sphere(VertexBlob *b, uint32_t *first, uint32_t *count) {
    uint32_t stack, slice;
    *first = b->count;
    for (stack = 0; stack < SPHERE_STACKS; ++stack) {
        const float lat0 = (float)M_PI * ((float)stack / (float)SPHERE_STACKS) - 0.5f * (float)M_PI;
        const float lat1 = (float)M_PI * ((float)(stack + 1u) / (float)SPHERE_STACKS) - 0.5f * (float)M_PI;
        for (slice = 0; slice < SPHERE_SLICES; ++slice) {
            const float lon0 = 2.0f * (float)M_PI * ((float)slice / (float)SPHERE_SLICES);
            const float lon1 = 2.0f * (float)M_PI * ((float)(slice + 1u) / (float)SPHERE_SLICES);
            float p00[8], p10[8], p01[8], p11[8];
            sphere_point(lat0, lon0, 1.0f, 0.0f, p00);
            sphere_point(lat1, lon0, 1.0f, 0.0f, p10);
            sphere_point(lat0, lon1, 1.0f, 0.0f, p01);
            sphere_point(lat1, lon1, 1.0f, 0.0f, p11);
            /* Counter-clockwise seen from outside the mesh, so CULL_BACK
             * keeps the surface and the outline pass' CULL_FRONT keeps the
             * hull's inner shell. */
            if (!push_tri(b, p00, p11, p10) || !push_tri(b, p00, p01, p11))
                return false;
        }
    }
    *count = b->count - *first;
    return true;
}

/* Unit capsule: radius 1, cylinder from y = -1 to y = +1, hemispherical caps. */
static bool add_capsule(VertexBlob *b, uint32_t *first, uint32_t *count) {
    uint32_t slice, stack;
    *first = b->count;
    for (slice = 0; slice < CAPSULE_SLICES; ++slice) {
        const float lon0 = 2.0f * (float)M_PI * ((float)slice / (float)CAPSULE_SLICES);
        const float lon1 = 2.0f * (float)M_PI * ((float)(slice + 1u) / (float)CAPSULE_SLICES);
        const float s0 = sinf(lon0), c0 = cosf(lon0), s1 = sinf(lon1), c1 = cosf(lon1);
        float p00[8], p01[8], p10[8], p11[8];
        /* side quad */
        {
            float a[8] = {s0, -1.0f, c0, s0, 0.0f, c0, (float)slice / (float)CAPSULE_SLICES, 0.0f};
            float c[8] = {s0,  1.0f, c0, s0, 0.0f, c0, (float)slice / (float)CAPSULE_SLICES, 1.0f};
            float d[8] = {s1,  1.0f, c1, s1, 0.0f, c1, (float)(slice + 1u) / (float)CAPSULE_SLICES, 1.0f};
            float e[8] = {s1, -1.0f, c1, s1, 0.0f, c1, (float)(slice + 1u) / (float)CAPSULE_SLICES, 0.0f};
            if (!push_tri(b, a, d, c) || !push_tri(b, a, e, d))
                return false;
        }
        /* top cap */
        for (stack = 0; stack < CAPSULE_CAP_STACKS; ++stack) {
            const float lat0 = 0.5f * (float)M_PI * ((float)stack / (float)CAPSULE_CAP_STACKS);
            const float lat1 = 0.5f * (float)M_PI * ((float)(stack + 1u) / (float)CAPSULE_CAP_STACKS);
            sphere_point(lat0, lon0, 1.0f, 1.0f, p00);
            sphere_point(lat1, lon0, 1.0f, 1.0f, p10);
            sphere_point(lat0, lon1, 1.0f, 1.0f, p01);
            sphere_point(lat1, lon1, 1.0f, 1.0f, p11);
            if (!push_tri(b, p00, p11, p10) || !push_tri(b, p00, p01, p11))
                return false;
        }
        /* bottom cap */
        for (stack = 0; stack < CAPSULE_CAP_STACKS; ++stack) {
            const float lat0 = -0.5f * (float)M_PI * ((float)stack / (float)CAPSULE_CAP_STACKS);
            const float lat1 = -0.5f * (float)M_PI * ((float)(stack + 1u) / (float)CAPSULE_CAP_STACKS);
            sphere_point(lat1, lon0, 1.0f, -1.0f, p00);
            sphere_point(lat0, lon0, 1.0f, -1.0f, p10);
            sphere_point(lat1, lon1, 1.0f, -1.0f, p01);
            sphere_point(lat0, lon1, 1.0f, -1.0f, p11);
            if (!push_tri(b, p00, p11, p10) || !push_tri(b, p00, p01, p11))
                return false;
        }
    }
    *count = b->count - *first;
    return true;
}

/* Unit ground quad: 2x2 in XZ at y = 0, normal +Y. */
static bool add_plane(VertexBlob *b, uint32_t *first, uint32_t *count) {
    float bl[8] = {-1.0f, 0.0f,  1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f};
    float br[8] = { 1.0f, 0.0f,  1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f};
    float tr[8] = { 1.0f, 0.0f, -1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f};
    float tl[8] = {-1.0f, 0.0f, -1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f};
    *first = b->count;
    if (!push_tri(b, bl, br, tr) || !push_tri(b, bl, tr, tl))
        return false;
    *count = b->count - *first;
    return true;
}

/* Unit cube: 2x2x2 centred at the origin, flat normals per face. */
static bool add_cube(VertexBlob *b, uint32_t *first, uint32_t *count) {
    static const float face_n[6][3] = {
        { 1.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f},
        { 0.0f, 1.0f, 0.0f}, { 0.0f,-1.0f, 0.0f},
        { 0.0f, 0.0f, 1.0f}, { 0.0f, 0.0f,-1.0f},
    };
    /* Corner order per face, counter-clockwise from outside. */
    static const float face_c[6][4][3] = {
        {{1,-1,-1},{1,-1,1},{1,1,1},{1,1,-1}},     /* +X */
        {{-1,-1,1},{-1,-1,-1},{-1,1,-1},{-1,1,1}}, /* -X */
        {{-1,1,-1},{1,1,-1},{1,1,1},{-1,1,1}},     /* +Y */
        {{-1,-1,1},{1,-1,1},{1,-1,-1},{-1,-1,-1}}, /* -Y */
        {{-1,-1,1},{-1,1,1},{1,1,1},{1,-1,1}},     /* +Z */
        {{1,-1,-1},{1,1,-1},{-1,1,-1},{-1,-1,-1}}, /* -Z */
    };
    static const float face_uv[4][2] = {{0,1},{1,1},{1,0},{0,0}};
    uint32_t face;
    *first = b->count;
    for (face = 0; face < 6u; ++face) {
        float quad[4][8];
        uint32_t i;
        for (i = 0; i < 4u; ++i) {
            quad[i][0] = face_c[face][i][0];
            quad[i][1] = face_c[face][i][1];
            quad[i][2] = face_c[face][i][2];
            quad[i][3] = face_n[face][0];
            quad[i][4] = face_n[face][1];
            quad[i][5] = face_n[face][2];
            quad[i][6] = face_uv[i][0];
            quad[i][7] = face_uv[i][1];
        }
        if (!push_tri(b, quad[0], quad[2], quad[1]) ||
            !push_tri(b, quad[0], quad[3], quad[2]))
            return false;
    }
    *count = b->count - *first;
    return true;
}

typedef struct MeshRange {
    uint32_t first, count;
} MeshRange;

/* Placement helper: translation, axis-angle rotation, uniform scale. */
static void model_matrix(float out[16], const float translate[3],
                         const float axis[3], float angle, float scale) {
    float q[4], t[3], s[3];
    mu_quat_axis_angle(q, axis, angle);
    mu_vec3_copy(t, translate);
    mu_vec3_make(s, scale, scale, scale);
    mu_mat4_trs(out, t, q, s);
}

void toon_obj_set_transform(ToonObject *object, const float translate[3],
                            const float axis[3], float angle, float scale) {
    model_matrix(object->model, translate, axis, angle, scale);
}

bool toon_scene_build(ToonScene *scene) {
    VertexBlob blob;
    MeshRange  mesh[4];
    ToonObject *objects;
    uint32_t    n = 0u;

    memset(scene, 0, sizeof *scene);
    memset(&blob, 0, sizeof blob);

    if (!add_sphere(&blob, &mesh[0].first, &mesh[0].count) ||
        !add_capsule(&blob, &mesh[1].first, &mesh[1].count) ||
        !add_plane(&blob, &mesh[2].first, &mesh[2].count) ||
        !add_cube(&blob, &mesh[3].first, &mesh[3].count)) {
        free(blob.verts);
        return false;
    }

    /* ground + figure (body, head, two arms, two eyes) + ball + crate */
    objects = (ToonObject *)calloc(10u, sizeof *objects);
    if (!objects) {
        free(blob.verts);
        return false;
    }

#define ADD_OBJECT(mesh_index, label, t, axis, angle, scale, r, g, b, object_flags, width) \
    do {                                                                                  \
        ToonObject *o = &objects[n++];                                                    \
        o->name          = (label);                                                       \
        o->first_vertex  = mesh[(mesh_index)].first;                                      \
        o->vertex_count  = mesh[(mesh_index)].count;                                      \
        model_matrix(o->model, (t), (axis), (angle), (scale));                             \
        o->color[0] = (r); o->color[1] = (g); o->color[2] = (b); o->color[3] = 1.0f;       \
        o->flags        = (object_flags);                                                  \
        o->outline_scale = (width);                                                        \
    } while (0)

    {
        const float up[3]   = {0.0f, 1.0f, 0.0f};
        const float zaxis[3] = {0.0f, 0.0f, 1.0f};

        /* Ground: no outline (an inflated quad would outline the horizon). */
        ADD_OBJECT(2, "ground", ((const float[3]){0.0f, 0.0f, 0.0f}), zaxis, 0.0f, 14.0f,
                   0.52f, 0.55f, 0.70f, TOON_OBJ_NO_OUTLINE | TOON_OBJ_USE_CHECKER, 1.0f);

        /* Body capsule. */
        ADD_OBJECT(1, "body", ((const float[3]){0.0f, 1.05f, 0.0f}), zaxis, 0.0f, 0.40f,
                   0.86f, 0.33f, 0.22f, 0u, 1.0f);

        /* Head sphere. */
        ADD_OBJECT(0, "head", ((const float[3]){0.0f, 2.05f, 0.0f}), zaxis, 0.0f, 0.42f,
                   0.93f, 0.78f, 0.66f, 0u, 1.0f);

        /* Arms: capsules tilted out from the shoulders. */
        ADD_OBJECT(1, "arm L", ((const float[3]){-0.46f, 1.16f, 0.0f}), zaxis, 0.80f, 0.125f,
                   0.86f, 0.33f, 0.22f, 0u, 1.0f);
        ADD_OBJECT(1, "arm R", ((const float[3]){0.46f, 1.16f, 0.0f}), zaxis, -0.80f, 0.125f,
                   0.86f, 0.33f, 0.22f, 0u, 1.0f);

        /* Eyes: no outline (they are already dark). */
        ADD_OBJECT(0, "eye L", ((const float[3]){-0.15f, 2.12f, 0.37f}), zaxis, 0.0f, 0.075f,
                   0.08f, 0.09f, 0.14f, TOON_OBJ_NO_OUTLINE, 0.0f);
        ADD_OBJECT(0, "eye R", ((const float[3]){0.15f, 2.12f, 0.37f}), zaxis, 0.0f, 0.075f,
                   0.08f, 0.09f, 0.14f, TOON_OBJ_NO_OUTLINE, 0.0f);

        /* Floating ball: a second silhouette to compare outline widths. */
        ADD_OBJECT(0, "ball", ((const float[3]){-1.45f, 0.42f, 0.35f}), zaxis, 0.0f, 0.30f,
                   0.40f, 0.78f, 0.66f, TOON_OBJ_USE_CHECKER, 1.0f);

        /* Crate: flat faces, so the cel band shows as crisp quads. */
        ADD_OBJECT(3, "crate", ((const float[3]){1.55f, 0.36f, -0.45f}), up, 0.42f, 0.36f,
                   0.62f, 0.74f, 0.58f, TOON_OBJ_USE_CHECKER, 1.0f);
    }

#undef ADD_OBJECT

    scene->vertices     = blob.verts;
    scene->vertex_count = blob.count;
    scene->objects      = objects;
    scene->object_count = n;
    return true;
}

/* ---- smoothed outline normals -------------------------------------------- */

void toon_oct_encode(const float normal[3], float out[2]) {
    const float l1 = fabsf(normal[0]) + fabsf(normal[1]) + fabsf(normal[2]);
    float x = l1 > 0.0f ? normal[0] / l1 : 0.0f;
    float y = l1 > 0.0f ? normal[1] / l1 : 0.0f;
    if (normal[2] < 0.0f) {
        const float fx = (1.0f - fabsf(y)) * (x >= 0.0f ? 1.0f : -1.0f);
        const float fy = (1.0f - fabsf(x)) * (y >= 0.0f ? 1.0f : -1.0f);
        x = fx;
        y = fy;
    }
    out[0] = x;
    out[1] = y;
}

void toon_oct_decode(const float in[2], float normal[3]) {
    float x = in[0], y = in[1];
    const float z = 1.0f - fabsf(x) - fabsf(y);
    const float t = z < 0.0f ? -z : 0.0f;
    float len;
    x += x >= 0.0f ? -t : t;
    y += y >= 0.0f ? -t : t;
    len = sqrtf(x * x + y * y + z * z);
    normal[0] = x / len;
    normal[1] = y / len;
    normal[2] = z / len;
}

/* One mesh range. Distinct normals only: a triangle soup repeats a corner once
 * per triangle, and counting a face twice because it happens to be split into
 * two triangles there would tilt the average towards that face. */
static void bake_range(ToonVertex *verts, uint32_t count) {
    const float pos_eps = 1.0e-5f;
    uint32_t i, j, k;

    for (i = 0u; i < count; ++i) {
        float distinct[8][3];
        uint32_t n = 0u;
        float sum[3] = {0.0f, 0.0f, 0.0f};
        float len;

        for (j = 0u; j < count; ++j) {
            const float *a = verts[i].pos;
            const float *b = verts[j].pos;
            bool seen = false;
            if (fabsf(a[0] - b[0]) > pos_eps || fabsf(a[1] - b[1]) > pos_eps ||
                fabsf(a[2] - b[2]) > pos_eps)
                continue;
            for (k = 0u; k < n; ++k) {
                const float d = distinct[k][0] * verts[j].nrm[0] +
                                distinct[k][1] * verts[j].nrm[1] +
                                distinct[k][2] * verts[j].nrm[2];
                if (d > 0.9999f) { seen = true; break; }
            }
            if (seen || n == 8u) continue;
            distinct[n][0] = verts[j].nrm[0];
            distinct[n][1] = verts[j].nrm[1];
            distinct[n][2] = verts[j].nrm[2];
            ++n;
        }
        for (k = 0u; k < n; ++k) {
            sum[0] += distinct[k][0];
            sum[1] += distinct[k][1];
            sum[2] += distinct[k][2];
        }
        len = sqrtf(sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2]);
        if (len < 1.0e-6f) {
            /* Opposing normals cancel (a two-sided sheet): keep the vertex's own. */
            sum[0] = verts[i].nrm[0]; sum[1] = verts[i].nrm[1]; sum[2] = verts[i].nrm[2];
            len = sqrtf(sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2]);
        }
        if (len > 0.0f) { sum[0] /= len; sum[1] /= len; sum[2] /= len; }
        toon_oct_encode(sum, verts[i].pad);
    }
}

void toon_scene_bake_outline_normals(ToonScene *scene) {
    uint32_t i, j;
    /* Objects share mesh ranges (several spheres, several capsules): bake each
     * distinct range once. */
    for (i = 0u; i < scene->object_count; ++i) {
        const ToonObject *o = &scene->objects[i];
        bool done = false;
        for (j = 0u; j < i; ++j)
            if (scene->objects[j].first_vertex == o->first_vertex) { done = true; break; }
        if (!done) bake_range(scene->vertices + o->first_vertex, o->vertex_count);
    }
}

void toon_scene_free(ToonScene *scene) {
    free(scene->vertices);
    free(scene->objects);
    memset(scene, 0, sizeof *scene);
}
