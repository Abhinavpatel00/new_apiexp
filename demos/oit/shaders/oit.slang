// oit.slang — transparency without sorting, and two things to compare it with.
//
// Weighted blended order-independent transparency (McGuire & Bavoil, JCGT
// 2013): every transparent fragment adds  w * (colour * alpha, alpha)  to one
// target and multiplies another by (1 - alpha), in any order. The composite
// is a weighted AVERAGE of the transparent colours, laid over the background
// with the exact total coverage 1 - product(1 - alpha). The coverage is
// right; which colour dominates is a guess made by the weight w, larger for
// nearer fragments.
//
//   vertMain        geometry, shared by every pass
//   fragOpaque      the opaque scene
//   fragAccum       sum of w * premultiplied colour       (blend: add)
//   fragReveal      product of (1 - alpha)                 (blend: dst * (1 - src alpha))
//   fragComposite   average over background                (fullscreen, blend: alpha)
//   fragPeel        reference: the farthest layer not yet taken (depth peeling, back to front)
//   fragOver        reference: lay that layer over the picture (fullscreen, blend: premultiplied)
//   fragBlend       ordinary blending in draw order, for comparison
import mu_gfx;
import mu_mesh;

struct Push
{
    float4 vp0, vp1, vp2, vp3;
    float4 eye;                   // w: nearest view depth of anything transparent, metres
    float4 light;                 // xyz towards the light; w: the farthest
    uint   vertex_offset, object_offset;
    uint   weight;                // 0 general depth weight, 1 none (w = 1), 2 fitted to the scene, 3 count layers instead
    uint   flags;                 // 1: transparent colours are black; 2: first peel
    uint   tex0, tex1, tex2, pad; // pass dependent, bindless
};
[[vk::push_constant]] ConstantBuffer<Push> pc;

struct Varyings
{
    float4 position : SV_Position;
    [[vk::location(0)]] float3 world  : TEXCOORD0;
    [[vk::location(1)]] float3 normal : TEXCOORD1;
    [[vk::location(2)]] float4 color  : TEXCOORD2;   // rgb, alpha
    [[vk::location(3)]] float  depth  : TEXCOORD3;   // distance along the view axis, metres
};

[shader("vertex")]
Varyings vertMain(uint vertexIndex : SV_VertexID)
{
    MeshVertex v = muLoadMeshVertex(pc.vertex_offset, vertexIndex);
    MeshObject o = muLoadMeshObject(pc.object_offset);
    Varyings r;
    r.world    = muMulPoint(o.model, v.position);
    r.normal   = muMulDir(o.model, v.normal);
    r.color    = o.color;
    r.position = pc.vp0 * r.world.x + pc.vp1 * r.world.y + pc.vp2 * r.world.z + pc.vp3;
    r.depth    = r.position.w;
    return r;
}

float3 lit(Varyings i)
{
    float3 n = normalize(i.normal);
    if (dot(n, pc.eye.xyz - i.world) < 0.0) n = -n;
    float3 albedo = i.color.rgb;
    if (abs(i.world.y) < 1e-3 && n.y > 0.9)
    {
        float2 g = floor(i.world.xz);
        albedo *= lerp(0.55, 1.0, muMod(g.x + g.y, 2.0));
    }
    return albedo * (0.25 + 0.75 * saturate(dot(n, pc.light.xyz)));
}

// A transparent fragment: colour (not premultiplied) and alpha.
float4 glass(Varyings i)
{
    return float4((pc.flags & 1u) != 0u ? float3(0.0) : lit(i), i.color.a);
}

[shader("fragment")]
float4 fragOpaque(Varyings i) : SV_Target0 { return float4(lit(i), 1.0); }

// 0: the paper's general-purpose weight (its equation 7, as I remember it):
//    falls with view depth z in metres, meant for depths of 0.1 .. 500.
// 2: the shape of its equation 9, 3e3 * (1 - d)^3, but with d running from 0
//    at the nearest transparent thing in view to 1 at the farthest instead of
//    being the depth-buffer value (which is 0.99 and more for this whole
//    scene). That change is mine.
float weight(float z, float alpha)
{
    if (pc.weight == 1u) return 1.0;
    if (pc.weight == 2u)
    {
        float d = saturate((z - pc.eye.w) / max(pc.light.w - pc.eye.w, 1e-3));
        return alpha * max(1e-2, 3e3 * pow(1.0 - d, 3.0));
    }
    return alpha * clamp(10.0 / (1e-5 + pow(z / 5.0, 2.0) + pow(z / 200.0, 6.0)), 1e-2, 3e3);
}

[shader("fragment")]
float4 fragAccum(Varyings i) : SV_Target0
{
    if (pc.weight == 3u) return float4(1.0, 0.0, 0.0, 1.0);          // --check: layers per pixel
    float4 g = glass(i);
    // The paper's sums: premultiplied colour * w and alpha * w. Its weights
    // contain alpha once more: nearer AND more opaque counts for more.
    return float4(g.rgb * g.a, g.a) * weight(i.depth, g.a);
}

[shader("fragment")]
float4 fragReveal(Varyings i) : SV_Target0 { return float4(0.0, 0.0, 0.0, i.color.a); }

// tex0 accum, tex1 reveal.
[shader("fragment")]
float4 fragComposite(FullscreenVaryings i) : SV_Target0
{
    int3   p      = int3((int2)i.position.xy, 0);
    float4 accum  = muSampledImages[NonUniformResourceIndex(pc.tex0)].Load(p);
    float  reveal = muSampledImages[NonUniformResourceIndex(pc.tex1)].Load(p).r;
    return float4(accum.rgb / max(accum.a, 1e-5), 1.0 - reveal);
}

// tex0 opaque depth, tex1 the depth of the layer peeled before this one.
// The depth test keeps the FARTHEST fragment that is in front of the opaque
// scene and nearer than the last layer taken.
[shader("fragment")]
float4 fragPeel(Varyings i) : SV_Target0
{
    int3  p = int3((int2)i.position.xy, 0);
    float z = i.position.z;
    if (z >= muSampledImages[NonUniformResourceIndex(pc.tex0)].Load(p).r) discard;
    if ((pc.flags & 2u) == 0u && z >= muSampledImages[NonUniformResourceIndex(pc.tex1)].Load(p).r) discard;
    float4 g = glass(i);
    return float4(g.rgb * g.a, g.a);
}

// tex0 the peeled layer, premultiplied.
[shader("fragment")]
float4 fragOver(FullscreenVaryings i) : SV_Target0
{
    return muSampledImages[NonUniformResourceIndex(pc.tex0)].Load(int3((int2)i.position.xy, 0));
}

[shader("fragment")]
float4 fragBlend(Varyings i) : SV_Target0
{
    float4 g = glass(i);
    return float4(g.rgb * g.a, g.a);
}
