// lines.slang — two ways to draw toon outlines, side by side.
//
//   vertMain / fragScene   two-tone shading; also world normal + object number, view depth
//   vertHull / fragHull    the inverted hull: the object again, back faces only, every
//                          vertex pushed `width` pixels along its normal on screen
//   fragEdges              lines found in the picture: where depth breaks (drawn on the
//                          nearer surface) or the normal turns sharply
//
// NiloToon uses both (hull for the silhouette, screen space for the rest); see
// docs/nilo-toon-study.md. View space here: x right, y up, z forward.
import mu_gfx;
import mu_mesh;

static const uint HULL = 1u, EDGES = 2u;

struct Push
{
    float4 vp0, vp1, vp2, vp3;
    float4 eye;
    float4 right;        // w: tan(half fov x)
    float4 up;           // w: tan(half fov y)
    float4 forward;
    float4 size;         // width, height, line width in pixels, crease: cos of the angle
    float4 light;
    uint   vertex_offset, object_offset, object_id, mode;
    uint   color, normal, depth, naive;   // naive: compare neighbouring depths directly
};
[[vk::push_constant]] ConstantBuffer<Push> pc;

struct Varyings
{
    float4 position : SV_Position;
    [[vk::location(0)]] float3 world  : TEXCOORD0;
    [[vk::location(1)]] float3 normal : TEXCOORD1;
    [[vk::location(2)]] float4 color  : TEXCOORD2;
};

float4 project(float3 w) { return pc.vp0 * w.x + pc.vp1 * w.y + pc.vp2 * w.z + pc.vp3; }

[shader("vertex")]
Varyings vertMain(uint vertexIndex : SV_VertexID)
{
    MeshVertex v = muLoadMeshVertex(pc.vertex_offset, vertexIndex);
    MeshObject o = muLoadMeshObject(pc.object_offset);
    Varyings r;
    r.world    = muMulPoint(o.model, v.position);
    r.normal   = muMulDir(o.model, v.normal);
    r.color    = o.color;
    r.position = project(r.world);
    return r;
}

struct SceneOut
{
    float4 color  : SV_Target0;
    float4 normal : SV_Target1;   // world normal, a: object number (0 = nothing)
    float  depth  : SV_Target2;   // along the view direction, metres
};

[shader("fragment")]
SceneOut fragScene(Varyings i)
{
    SceneOut o;
    float3 n = normalize(i.normal);
    float  lit = smoothstep(0.02, 0.06, dot(n, pc.light.xyz));            // two tones with a narrow blend
    o.color  = float4(i.color.rgb * lerp(0.55, 1.0, lit), 1.0);
    o.normal = float4(n, float(pc.object_id));
    o.depth  = dot(i.world - pc.eye.xyz, pc.forward.xyz);
    return o;
}

// The hull. On screen the normal points along (n.right, n.up); the vertex moves that way by
// `width` pixels whatever its distance. Flat-shaded boxes have three normals per corner, so
// their hull splits there — the reason NiloToon bakes smoothed normals for this pass.
[shader("vertex")]
float4 vertHull(uint vertexIndex : SV_VertexID) : SV_Position
{
    MeshVertex v = muLoadMeshVertex(pc.vertex_offset, vertexIndex);
    MeshObject o = muLoadMeshObject(pc.object_offset);
    float3 n    = normalize(muMulDir(o.model, v.normal));
    float4 clip = project(muMulPoint(o.model, v.position));
    float2 dir  = float2(dot(n, pc.right.xyz), dot(n, pc.up.xyz));
    dir /= max(length(dir), 0.1);
    clip.xy += dir * (pc.size.z * 2.0 / pc.size.xy) * clip.w;
    return clip;
}

[shader("fragment")]
float4 fragHull() : SV_Target0 { return float4(0.0, 0.0, 0.0, 1.0); }

float  depthAt(int2 px) { return muSampledImages[NonUniformResourceIndex(pc.depth)].Load(int3(px, 0)).r; }
float4 normalAt(int2 px) { return muSampledImages[NonUniformResourceIndex(pc.normal)].Load(int3(px, 0)); }

float3 ray(float2 pixel)   // view space, z = 1
{
    float2 ndc = float2(pixel.x / pc.size.x * 2.0 - 1.0, 1.0 - pixel.y / pc.size.y * 2.0);
    return float3(ndc.x * pc.right.w, ndc.y * pc.up.w, 1.0);
}

[shader("fragment")]
float4 fragEdges(FullscreenVaryings i) : SV_Target0
{
    int2   px = int2(i.position.xy);
    float4 c  = muSampledImages[NonUniformResourceIndex(pc.color)].Load(int3(px, 0));
    float  d  = depthAt(px);
    if ((pc.mode & EDGES) == 0u || d <= 0.0) return c;
    float4 n  = normalAt(px);
    float3 nv = float3(dot(n.xyz, pc.right.xyz), dot(n.xyz, pc.up.xyz), dot(n.xyz, pc.forward.xyz));
    // The plane through this pixel's surface point: a neighbour on the same flat surface has
    // exactly the depth the plane gives it, however shallow the view. Comparing the two depths
    // directly (`naive`, kept for --check) draws lines on a floor seen at a low angle.
    float  plane = dot(nv, ray(i.position.xy) * d);
    int    reach = int(pc.size.z + 0.5), half_reach = (reach + 1) / 2;
    bool   line = false;
    static const int2 k_dir[4] = {int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1)};
    for (int k = 1; k <= reach && !line; ++k)
        for (int j = 0; j < 4; ++j)
        {
            int2 q = px + k_dir[j] * k;
            if (any(q < 0) || any(q >= int2(pc.size.xy))) continue;
            float  qd = depthAt(q);
            float  denom = dot(nv, ray(float2(q) + 0.5));
            float  expected = (pc.naive == 0u && abs(denom) > 1e-4) ? plane / denom : d;
            // The neighbour is farther than this surface would be there: this is the near side of a break.
            if (qd <= 0.0 || qd - expected > 0.03 * expected + 0.05) { line = true; break; }
            // A crease: the same depth, another direction. Half the width on each side.
            if (k <= half_reach && abs(qd - expected) < 0.03 * expected + 0.05 && dot(n.xyz, normalAt(q).xyz) < pc.size.w) { line = true; break; }
        }
    return line ? float4(0.0, 0.0, 0.0, 1.0) : c;
}
