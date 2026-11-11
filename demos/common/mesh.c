/* mesh.c — see mesh.h. */
#include "mesh.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static DemoVertex *push(DemoMesh *m, uint32_t n) {
    if (m->count + n > m->capacity) {
        m->capacity = m->capacity ? m->capacity * 2u : 4096u;
        while (m->capacity < m->count + n) m->capacity *= 2u;
        m->vertices = (DemoVertex *)realloc(m->vertices, (size_t)m->capacity * sizeof(DemoVertex));
    }
    m->count += n;
    return &m->vertices[m->count - n];
}

static void set(DemoVertex *v, float px, float py, float pz, float nx, float ny, float nz) {
    v->pos[0] = px; v->pos[1] = py; v->pos[2] = pz; v->pos[3] = 1.0f;
    v->nrm[0] = nx; v->nrm[1] = ny; v->nrm[2] = nz; v->nrm[3] = 0.0f;
}

void demo_mesh_free(DemoMesh *m) {
    free(m->vertices);
    memset(m, 0, sizeof *m);
}

/* One quad: centre c, half-edges u and v with u x v = n. */
static void quad(DemoMesh *m, const float c[3], const float u[3], const float v[3], const float n[3]) {
    static const float k[6][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}};
    DemoVertex *out = push(m, 6u);
    uint32_t    i;
    for (i = 0u; i < 6u; ++i)
        set(&out[i], c[0] + k[i][0] * u[0] + k[i][1] * v[0], c[1] + k[i][0] * u[1] + k[i][1] * v[1],
            c[2] + k[i][0] * u[2] + k[i][1] * v[2], n[0], n[1], n[2]);
}

DemoRange demo_mesh_plane(DemoMesh *m, float h) {
    const DemoRange r = {m->count, 6u};
    const float c[3] = {0, 0, 0}, u[3] = {0, 0, h}, v[3] = {h, 0, 0}, n[3] = {0, 1, 0};
    quad(m, c, u, v, n);
    return r;
}

DemoRange demo_mesh_box(DemoMesh *m, float hx, float hy, float hz) {
    const DemoRange r = {m->count, 36u};
    const float h[3] = {hx, hy, hz};
    /* axis of the normal, of u and of v, with u x v = n for the + face */
    static const int axes[3][3] = {{0, 1, 2}, {1, 2, 0}, {2, 0, 1}};
    int a, s;
    for (a = 0; a < 3; ++a) {
        for (s = 0; s < 2; ++s) {
            const int ni = axes[a][0], ui = axes[a][s ? 2 : 1], vi = axes[a][s ? 1 : 2];
            float c[3] = {0, 0, 0}, u[3] = {0, 0, 0}, v[3] = {0, 0, 0}, n[3] = {0, 0, 0};
            n[ni] = s ? -1.0f : 1.0f;
            c[ni] = n[ni] * h[ni];
            u[ui] = h[ui];
            v[vi] = h[vi];
            quad(m, c, u, v, n);
        }
    }
    return r;
}

DemoRange demo_mesh_sphere(DemoMesh *m, float radius, uint32_t segments, uint32_t rings) {
    const DemoRange r = {m->count, segments * rings * 6u};
    DemoVertex *out = push(m, r.count);
    uint32_t i, j, k;
    for (i = 0u; i < rings; ++i) {
        for (j = 0u; j < segments; ++j) {
            /* (theta, phi) corners; +phi then +theta is counter-clockwise from outside */
            static const uint32_t corner[6][2] = {{0, 0}, {0, 1}, {1, 1}, {0, 0}, {1, 1}, {1, 0}};
            for (k = 0u; k < 6u; ++k) {
                const float t = 3.14159265f * (float)(i + corner[k][0]) / (float)rings;
                const float p = 6.28318531f * (float)(j + corner[k][1]) / (float)segments;
                const float nx = sinf(t) * cosf(p), ny = cosf(t), nz = sinf(t) * sinf(p);
                set(out++, radius * nx, radius * ny, radius * nz, nx, ny, nz);
            }
        }
    }
    return r;
}

DemoRange demo_mesh_torus(DemoMesh *m, float major, float minor, uint32_t segments, uint32_t sides) {
    const DemoRange r = {m->count, segments * sides * 6u};
    DemoVertex *out = push(m, r.count);
    uint32_t i, j, k;
    for (i = 0u; i < segments; ++i) {
        for (j = 0u; j < sides; ++j) {
            static const uint32_t corner[6][2] = {{0, 0}, {0, 1}, {1, 1}, {0, 0}, {1, 1}, {1, 0}};
            for (k = 0u; k < 6u; ++k) {
                const float a = 6.28318531f * (float)(i + corner[k][0]) / (float)segments;
                const float b = 6.28318531f * (float)(j + corner[k][1]) / (float)sides;
                const float nx = cosf(b) * cosf(a), ny = sinf(b), nz = cosf(b) * sinf(a);
                set(out++, (major + minor * cosf(b)) * cosf(a), minor * sinf(b),
                    (major + minor * cosf(b)) * sinf(a), nx, ny, nz);
            }
        }
    }
    return r;
}

void demo_object(DemoObject *o, float x, float y, float z, float yaw, float scale,
                 float r, float g, float b) {
    const float c = cosf(yaw) * scale, s = sinf(yaw) * scale;
    memset(o, 0, sizeof *o);
    o->model[0] = c;      o->model[2] = -s;    /* column 0 */
    o->model[5] = scale;                       /* column 1 */
    o->model[8] = s;      o->model[10] = c;    /* column 2 */
    o->model[12] = x; o->model[13] = y; o->model[14] = z; o->model[15] = 1.0f;
    o->color[0] = r; o->color[1] = g; o->color[2] = b; o->color[3] = 1.0f;
}
