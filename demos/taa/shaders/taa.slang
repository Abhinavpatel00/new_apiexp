// taa.slang — temporal anti-aliasing.
//
// Every frame the projection is shifted by a fraction of a pixel (an 8-entry
// Halton sequence), so that over time each pixel sees several points of its
// own area. The resolve pass blends the new frame into a history image:
//
//     history = lerp(history at where this surface was last frame, current, 0.1)
//
// "Where it was" comes from a velocity target written by the scene pass
// (object and camera motion). Where that history is stale — something else
// was there last frame — it is clamped into the range of the current frame's
// 3x3 neighbourhood (Karis, "High Quality Temporal Supersampling", 2014).
//
//   vertMain / fragScene   colour + velocity in pixels
//   fragResolve            fullscreen: history for the next frame, and the picture
import mu_gfx;
import mu_mesh;

static const uint RESOLVE_CLAMP = 1u, RESOLVE_VELOCITY = 2u, RESOLVE_PASS_THROUGH = 4u, RESOLVE_NO_HISTORY = 8u;

struct Push
{
    float4 vp0, vp1, vp2, vp3;        // this frame, without the jitter
    float4 pv0, pv1, pv2, pv3;        // last frame, without the jitter
    float4 eye;
    float4 jitter_size;               // xy jitter in pixels, zw target size
    uint   vertex_offset, object_offset, prev_object_offset, flags;
    uint   color, velocity, history, sampler;
    float  alpha;
    float  pad0, pad1, pad2;
};
[[vk::push_constant]] ConstantBuffer<Push> pc;

struct Varyings
{
    float4 position : SV_Position;
    [[vk::location(0)]] float3 world  : TEXCOORD0;
    [[vk::location(1)]] float3 normal : TEXCOORD1;
    [[vk::location(2)]] float4 color  : TEXCOORD2;
    [[vk::location(3)]] float4 clip   : TEXCOORD3;   // this frame, no jitter
    [[vk::location(4)]] float4 prev   : TEXCOORD4;   // last frame, no jitter
};

[shader("vertex")]
Varyings vertMain(uint vertexIndex : SV_VertexID)
{
    MeshVertex v  = muLoadMeshVertex(pc.vertex_offset, vertexIndex);
    MeshObject o  = muLoadMeshObject(pc.object_offset);
    MeshObject po = muLoadMeshObject(pc.prev_object_offset);
    Varyings r;
    r.world  = muMulPoint(o.model, v.position);
    r.normal = muMulDir(o.model, v.normal);
    r.color  = o.color;
    r.clip   = pc.vp0 * r.world.x + pc.vp1 * r.world.y + pc.vp2 * r.world.z + pc.vp3;
    float3 pw = muMulPoint(po.model, v.position);
    r.prev   = pc.pv0 * pw.x + pc.pv1 * pw.y + pc.pv2 * pw.z + pc.pv3;
    // A shift of `jitter` pixels: one pixel is 2 / size in clip space (y points up).
    r.position = r.clip;
    r.position.xy += float2(pc.jitter_size.x, -pc.jitter_size.y) * 2.0 / pc.jitter_size.zw * r.clip.w;
    return r;
}

struct SceneOut
{
    float4 color    : SV_Target0;
    float4 velocity : SV_Target1;    // xy: pixels moved since last frame
};

[shader("fragment")]
SceneOut fragScene(Varyings i)
{
    float3 n = normalize(i.normal);
    float3 albedo = i.color.rgb;
    if (i.color.a < 0.5)             // the ground: 25 cm squares, deliberately too fine far away
    {
        float2 g = floor(i.world.xz * 4.0);
        albedo *= lerp(0.15, 1.0, muMod(g.x + g.y, 2.0));
    }
    float3 l = normalize(float3(0.40, 0.80, 0.45));
    SceneOut o;
    o.color = float4(albedo * (0.25 + 0.75 * saturate(dot(n, l))), 1.0);
    float2 now  = i.clip.xy / i.clip.w, then = i.prev.xy / i.prev.w;
    o.velocity  = float4((now - then) * float2(0.5, -0.5) * pc.jitter_size.zw, 0.0, 0.0);
    return o;
}

float3 current(int2 p)
{
    p = clamp(p, int2(0), int2(pc.jitter_size.zw) - 1);
    return muSampledImages[NonUniformResourceIndex(pc.color)].Load(int3(p, 0)).rgb;
}

[shader("fragment")]
float4 fragResolve(FullscreenVaryings i) : SV_Target0
{
    int2   p   = (int2)i.position.xy;
    float3 now = current(p);
    if ((pc.flags & (RESOLVE_PASS_THROUGH | RESOLVE_NO_HISTORY)) != 0u) return float4(now, 1.0);

    float2 moved = (pc.flags & RESOLVE_VELOCITY) != 0u
                 ? muSampledImages[NonUniformResourceIndex(pc.velocity)].Load(int3(p, 0)).xy : float2(0.0);
    float2 was   = i.position.xy - moved;                         // pixel centre it came from
    if (any(was < 0.0) || any(was >= pc.jitter_size.zw)) return float4(now, 1.0);
    float3 old = muSampledImages[NonUniformResourceIndex(pc.history)]
                     .SampleLevel(muSamplers[NonUniformResourceIndex(pc.sampler)], was / pc.jitter_size.zw, 0.0).rgb;
    if ((pc.flags & RESOLVE_CLAMP) != 0u)
    {
        float3 lo = now, hi = now;
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x)
            {
                float3 c = current(p + int2(x, y));
                lo = min(lo, c);
                hi = max(hi, c);
            }
        old = clamp(old, lo, hi);
    }
    return float4(lerp(old, now, pc.alpha), 1.0);
}
