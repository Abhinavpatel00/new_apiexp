/* scene.h — procedural geometry for the toon demo.
 *
 * The demo has no asset pipeline: every vertex is generated here, in local
 * space, and placed by a per-object model matrix. Geometry is a triangle soup
 * (no index buffer), because vk.h draws by SV_VertexID out of the bindless
 * storage buffer and the meshes are small enough that de-indexing costs
 * nothing (§17 "bindless geometry").
 *
 * The vertex is 48 bytes and std430-friendly: three vec4 slots, with uv packed
 * into the third one's xy. Overlaps the pad on purpose so the C struct and the
 * Slang struct (shaders/toon_common.slang) cannot drift apart.
 *
 *   ToonVertex (C, 48 B)   Slang (48 B)
 *   float pos[4]           float4 pos
 *   float nrm[4]           float4 nrm
 *   float uv[2]            float2 uv  (members 8..9)
 *   float pad[2]           float2 pad (0, or the baked outline normal: see
 *                                      toon_scene_bake_outline_normals)
 */

#ifndef TOON_SCENE_H
#define TOON_SCENE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct ToonVertex {
    float pos[4];
    float nrm[4];
    float uv[2];
    float pad[2];
} ToonVertex;

_Static_assert(sizeof(ToonVertex) == 48, "ToonVertex must match the shader's 48-byte std430 struct");

/* Object flags, mirrored by the shader's `meta.y`. */
#define TOON_OBJ_NO_OUTLINE   (1u << 0) /* inverted-hull pass skips this object */
#define TOON_OBJ_USE_CHECKER  (1u << 1) /* procedural _BaseMap pattern          */

typedef struct ToonObject {
    const char  *name;
    uint32_t     first_vertex; /* index into ToonScene.vertices              */
    uint32_t     vertex_count;
    float        model[16];    /* column-major; uniform scale + rotation     */
    float        color[4];     /* linear albedo + alpha                      */
    uint32_t     flags;        /* TOON_OBJ_*                                 */
    float        outline_scale;/* per-object outline width multiplier        */
} ToonObject;

typedef struct ToonScene {
    ToonVertex *vertices;
    uint32_t    vertex_count;
    ToonObject *objects;
    uint32_t    object_count;
} ToonScene;

/* Re-place one object: translation, axis-angle rotation, uniform scale. */
void toon_obj_set_transform(ToonObject *object, const float translate[3],
                            const float axis[3], float angle, float scale);

/* Builds the demo scene: ground plane, a two-tone toon figure (body, head,
 * arms, eyes), a floating ball, and a crate. Returns false only on allocation
 * failure. Caller frees with toon_scene_free(). */
bool toon_scene_build(ToonScene *scene);
void toon_scene_free(ToonScene *scene);

/* Bakes a smoothed outline normal into every vertex's `pad`, octahedral-encoded
 * in object space ([-1,1]^2).
 *
 * An inverted-hull outline extrudes each vertex along its normal. Where one
 * position carries several normals (every hard edge: the crate), the copies
 * move apart and the hull opens up, leaving gaps at corners. The fix used by
 * production toon shaders is to extrude along the average of the distinct
 * normals that share a position, which is what this stores. Smooth meshes get
 * their own normal back, so the result is identical for them.
 *
 * The lit shading normal (`nrm`) is not touched. Only shaders that opt in read
 * `pad` (demos/slang-toon/bindless, TOON_FLAG_SMOOTH_OUTLINE_NORMAL). */
void toon_scene_bake_outline_normals(ToonScene *scene);

/* The encoding used by the bake; exposed so hosts and tests can round-trip. */
void toon_oct_encode(const float normal[3], float out[2]);
void toon_oct_decode(const float in[2], float normal[3]);

#endif /* TOON_SCENE_H */
