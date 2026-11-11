// bricks_common.slang — a distance volume stored sparsely, and the same one stored densely.
//
// The world is a 16 m cube sampled at N = 7 B + 1 points a side. Cut into
// B^3 bricks of 7 cells (8 samples a side; neighbours share their boundary
// samples), only the bricks the surface can pass through are stored, packed
// into an atlas. A B^3 "indirection" volume says where each brick is:
//
//   stored brick   rgb = its slot in the atlas (in bricks), a = 0
//   empty brick    a = a distance that is safe everywhere in the brick:
//                  |d(centre)| minus the brick's half-diagonal, with d's sign
//
// So an empty brick costs one texel and is crossed in steps of at least MARGIN
// voxels; a stored brick costs 512 texels and is sampled like the dense volume.
// Texels are RGBA16F: distance in metres, and the surface normal (central
// differences at bake time), so shading needs no extra taps.
//
// Brick atlas + indirection is how sparse distance fields and voxel worlds
// are commonly stored (e.g. Claybook's world, GVDB, sparse virtual
// textures). Named from memory; no implementation was consulted.
static const float WORLD  = 16.0;      // metres a side
static const float MARGIN = 2.0;       // voxels: bricks nearer the surface than half-diagonal + this are stored
static const float HALF_DIAGONAL = 6.0622;   // of a brick, voxels: 3.5 * sqrt(3)

struct Push
{
    uint   flags, list, args, bricks;            // byte offsets; B
    uint   atlas_side, indirection, atlas, dense; // atlas side in bricks; bindless volume indices
    uint   sampler, view, mode, capacity;        // mode 0 sparse, 1 dense
    float4 eye, right, up, forward, size;        // draw only
    float4 world;                                // x: voxel size in metres
};

// ---- the scene, in metres --------------------------------------------------
float sdSphere(float3 p, float r) { return length(p) - r; }
float sdBox(float3 p, float3 h)
{
    float3 q = abs(p) - h;
    return length(max(q, 0.0)) + min(max(q.x, max(q.y, q.z)), 0.0);
}
float sdTorus(float3 p, float major, float minor) { return length(float2(length(p.xz) - major, p.y)) - minor; }

float scene(float3 p)
{
    float d = p.y - 1.0;                                         // the ground
    // A 5 x 5 field of things, 2.6 m apart. All 25 every time: looking only at the
    // cells around p made the function jump where a far pillar came into the
    // neighbourhood, and --check found empty bricks claiming more room than there was.
    for (int cz = 0; cz < 5; ++cz)
        for (int cx = 0; cx < 5; ++cx)
        {
            float2 c = float2(cx, cz);
            float3 o    = float3(1.5 + 2.6 * c.x + 1.3, 1.0, 1.5 + 2.6 * c.y + 1.3);
            uint   kind = (uint(c.x) + 2u * uint(c.y)) % 4u;
            float  s    = 0.75 + 0.25 * frac(0.37 * c.x + 0.61 * c.y);
            float3 q    = p - o;
            float  e;
            if (kind == 0u)      e = sdSphere(q - float3(0, 0.8 * s, 0), 0.8 * s);
            else if (kind == 1u) e = sdBox(q - float3(0, 0.6 * s, 0), float3(0.6, 0.6, 0.6) * s) - 0.05;
            else if (kind == 2u) e = sdTorus(q - float3(0, 0.3 * s, 0), 0.8 * s, 0.3 * s);
            else                 e = sdBox(q - float3(0, 1.7 * s, 0), float3(0.3, 1.7 * s, 0.3)) - 0.03;
            d = min(d, e);
        }
    d = min(d, sdTorus((p - float3(8.0, 6.5, 8.0)).xzy, 4.5, 0.35));   // a ring standing over the field
    return d;
}

// What every stored texel holds: the distance, and the normal.
float4 texelValue(int3 sample, float voxel)
{
    float3 p = float3(sample) * voxel;
    const float e = 0.5 * voxel;
    float3 n = float3(scene(p + float3(e, 0, 0)) - scene(p - float3(e, 0, 0)),
                      scene(p + float3(0, e, 0)) - scene(p - float3(0, e, 0)),
                      scene(p + float3(0, 0, e)) - scene(p - float3(0, 0, e)));
    return float4(scene(p), normalize(n));
}
