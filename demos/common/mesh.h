/* mesh.h — procedural triangle meshes for the demos.
 *
 * Non-indexed triangles, counter-clockwise seen from outside (vk.h's front
 * face), in the 32-byte vertex the shared shader module
 * demos/common/shaders/mu_mesh.slang loads by vertex id:
 *
 *     DemoVertex (C, 32 B)      MeshVertex (Slang)
 *     float pos[4]              float3 position; (w unused, 1)
 *     float nrm[4]              float3 normal;   (w unused, 0)
 *
 * and a 96-byte per-object record:
 *
 *     DemoObject (C, 96 B)      MeshObject (Slang)
 *     float model[16]           four float4 columns (mu_math is column-major)
 *     float color[4]            rgb albedo, a free
 *     float params[4]           demo-defined (roughness, metallic, emission, ...)
 */
#ifndef MU_DEMO_MESH_H
#define MU_DEMO_MESH_H

#include <stdbool.h>
#include <stdint.h>

typedef struct DemoVertex { float pos[4], nrm[4]; } DemoVertex;
typedef struct DemoObject { float model[16], color[4], params[4]; } DemoObject;
_Static_assert(sizeof(DemoVertex) == 32, "DemoVertex must match MeshVertex in mu_mesh.slang");
_Static_assert(sizeof(DemoObject) == 96, "DemoObject must match MeshObject in mu_mesh.slang");

typedef struct DemoRange { uint32_t first, count; } DemoRange; /* in vertices */

typedef struct DemoMesh {
    DemoVertex *vertices;
    uint32_t    count, capacity;
} DemoMesh;

void      demo_mesh_free(DemoMesh *m);
DemoRange demo_mesh_plane(DemoMesh *m, float half_size);                 /* y = 0, +y up */
DemoRange demo_mesh_box(DemoMesh *m, float hx, float hy, float hz);
DemoRange demo_mesh_sphere(DemoMesh *m, float radius, uint32_t segments, uint32_t rings);
DemoRange demo_mesh_torus(DemoMesh *m, float major, float minor, uint32_t segments, uint32_t sides);

/* model = translate(x, y, z) * rotateY(yaw) * scale(s). */
void demo_object(DemoObject *o, float x, float y, float z, float yaw, float scale,
                 float r, float g, float b);

#endif /* MU_DEMO_MESH_H */
