// toon.slang — forward pass of the toon demo.
//
// The fragment stage is the lighting equation of
// ColinLeung-NiloCat/UnityURPToonLitShaderExample
// (SimpleURPToonLitOutlineExample_LightingEquation.hlsl, MIT — see ../LICENSE),
// kept in the same order as the HLSL so the two can be read side by side:
//
//   ShadeGI                   -> shadeGI()
//   ShadeSingleLight          -> shadeSingleLight()
//   CompositeAllLightResults  -> fragMain()
//
// Deliberate differences from the URP original (demos/toon/README.md):
//   - No _BaseMap texture: the UVs drive a procedural checker so the demo ships
//     without assets. OBJ_USE_CHECKER selects it per object.
//   - No shadow atlas in this demo, so light.shadowAttenuation is 1 and
//     _ReceiveShadowMappingAmount has nothing to attenuate. (The Slang toon
//     host, demos/slang-toon, has the shadow-map path.)
//   - No light probes: the constant SH term is replaced by a hemispherical
//     ambient built from the same sky constants the backdrop uses.
//   - Rim light is not part of the tutorial; it is optional (cel.z = 0 = off).
//
// Push (256 B) must match ToonPush in demos/toon/main.c. The C side memcpy's
// column-major mu_math matrices, hence `column_major` + mul(M, v).
import mu_gfx;
import toon_common;

struct ToonPush
{
    column_major float4x4 view_proj;
    column_major float4x4 model;
    float4 camera;       // xyz eye position, w vertical fov in degrees
    float4 light_dir;    // xyz direction towards the light, w intensity
    float4 light_color;  // rgb light colour, w direct light multiplier
    float4 base_color;   // rgb albedo, a alpha
    float4 shadow_color; // rgb shadow tint, w indirect light multiplier
    float4 ambient_min;  // rgb minimum indirect colour, w is_face
    float4 cel;          // x cel midpoint, y softness, z rim, w unused
    uint4  meta;         // x vertex byte offset, y object flags
};
[[vk::push_constant]] ConstantBuffer<ToonPush> pc;

static const uint OBJ_NO_OUTLINE  = 1u;
static const uint OBJ_USE_CHECKER = 2u;

// The same two colours record_sky() in main.c pushes to the backdrop
// (sky.slang); keeping them in sync is what makes the indirect light read as
// coming from this sky.
static const float3 SKY_ZENITH  = float3(0.20, 0.26, 0.46);
static const float3 SKY_HORIZON = float3(0.74, 0.68, 0.80);

struct ToonVaryings
{
    float4 position : SV_Position;
    [[vk::location(0)]] float3 normal_ws   : NORMAL;
    [[vk::location(1)]] float3 position_ws : TEXCOORD0;
    [[vk::location(2)]] float2 uv          : TEXCOORD1;
};

[shader("vertex")]
ToonVaryings vertMain(uint vertexIndex : SV_VertexID)
{
    ToonVertex v = toonLoadVertex(pc.meta.x, vertexIndex);

    float4 position_ws = mul(pc.model, float4(v.pos.xyz, 1.0));

    ToonVaryings o;
    o.position_ws = position_ws.xyz;
    // Uniform scale + rotation only, so the model's upper 3x3 is a valid normal
    // transform up to length; the fragment stage renormalises.
    o.normal_ws = mul((float3x3)pc.model, v.nrm.xyz);
    o.uv        = v.uv;
    o.position  = mul(pc.view_proj, position_ws);
    return o;
}

float checker(float2 uv)
{
    float2 c = floor(uv * 8.0);
    return muMod(c.x + c.y, 2.0);
}

// Stand-in for _BaseMap * _BaseColor.
float3 baseMap(float2 uv)
{
    float3 tint = float3(1.0, 1.0, 1.0);
    if ((pc.meta.y & OBJ_USE_CHECKER) != 0u)
        tint = lerp(float3(0.82, 0.82, 0.82), float3(1.06, 1.06, 1.06), checker(uv));
    return tint;
}

// ShadeGI(): an average environment colour, deliberately flat so the figure
// keeps its 2D read. The URP version uses SampleSH(0) from baked probes.
float3 shadeGI(float3 normal_ws, float occlusion)
{
    float3 average_sh = lerp(SKY_HORIZON, SKY_ZENITH, normal_ws.y * 0.5 + 0.5) * 0.45;
    average_sh = max(pc.ambient_min.rgb, average_sh);
    return average_sh * lerp(1.0, occlusion, 0.5) * pc.shadow_color.w;
}

// ShadeSingleLight(): the one cel-shaded term.
float3 shadeSingleLight(float3 normal_ws, float occlusion)
{
    float3 l    = normalize(pc.light_dir.xyz);
    float  no_l = dot(normal_ws, l);

    // "the simplest 1 line cel shade": a soft step around _CelShadeMidPoint
    float lit_or_shadow_area = smoothstep(pc.cel.x - pc.cel.y, pc.cel.x + pc.cel.y, no_l);

    lit_or_shadow_area *= occlusion;

    // faces ignore cel shading: NoL banding on a face is usually ugly
    if (pc.ambient_min.w > 0.5)
        lit_or_shadow_area = lerp(0.5, 1.0, lit_or_shadow_area);

    // light.shadowAttenuation is 1: this demo has no shadow atlas (README)
    float3 lit_or_shadow_color = lerp(pc.shadow_color.rgb, float3(1.0, 1.0, 1.0), lit_or_shadow_area);

    // No point/spot lights in this port, so distanceAttenuation is 1 and
    // light_dir.w carries the directional light's intensity.
    return saturate(pc.light_color.rgb) * lit_or_shadow_color * pc.light_dir.w * pc.light_color.w;
}

[shader("fragment")]
float4 fragMain(ToonVaryings i) : SV_Target0
{
    float3 normal_ws = normalize(i.normal_ws);
    float3 view_dir  = normalize(pc.camera.xyz - i.position_ws);
    float3 albedo    = pc.base_color.rgb * baseMap(i.uv);

    // No _OcclusionMap in this port, so the term is 1 and only its position in
    // the equation is preserved.
    const float occlusion = 1.0;

    float3 indirect = shadeGI(normal_ws, occlusion);
    float3 direct   = shadeSingleLight(normal_ws, occlusion);

    // CompositeAllLightResults(): keep the light colour's hue instead of letting
    // two light terms add past white. No emission map in this port.
    float3 raw_light_sum = max(indirect, direct);
    float3 color         = albedo * raw_light_sum;

    // Optional rim light (not in the tutorial shader).
    float no_l = dot(normal_ws, normalize(pc.light_dir.xyz));
    float rim  = pow(1.0 - saturate(dot(normal_ws, view_dir)), 4.0) * smoothstep(-0.1, 0.45, no_l);
    color += pc.light_color.rgb * rim * pc.cel.z * 0.6;

    return float4(color, pc.base_color.a);
}
