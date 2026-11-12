// world.slang — the ground, the sea, the sky and the aeroplanes.
//
//   vertTerrain / fragTerrain     a clipmap of procedurally generated rings
//   vertWater   / fragWater       one quad under the camera, analytic waves
//   fragSky                       the background, drawn first and depth-less
//   vertAircraft/ fragAircraft    instanced from a table of model matrices
//
// The terrain has no vertex buffer and no heightfield texture. Both would be
// regenerated from the same seed every frame anyway, so the vertex shader
// builds the grid from SV_VertexID and evaluates fsTerrainHeight() at each
// point. That is the whole reason mu_shader_noise.h exists: the CPU answers
// "what is the ground under this aircraft" from the same function, so the hill
// you crash into is the hill you can see, and --check can prove it.
#include "fs_common.slang"

import mu_gfx;

// The two colour attachments every opaque pass writes: scene-linear radiance
// and the view distance in metres. The cloud pass reads the second one to know
// where to stop, and 0 in it means "nothing was drawn here".
struct SceneOut
{
    [[vk::location(0)]] float4 color  : SV_Target0;
    [[vk::location(1)]] float  linear : SV_Target1;
};

// ---- terrain --------------------------------------------------------------

struct TerrainPush
{
    float4x4 view_proj;
    float4   eye;         // xyz camera, w seconds
    float4   sun;         // xyz direction TO the sun, w intensity
    // w is the spacing of ring 0; every other ring is twice the one before it,
    // so the whole clipmap is one number and the ring index. Pushing a table of
    // ring origins instead would cost 16 bytes a ring and make the CPU recompute
    // every frame what instanceID already says.
    float4   params;      // x grid quads per side, y ring count, z turbidity, w ring-0 spacing
};
[[vk::push_constant]] ConstantBuffer<TerrainPush> tpc;

struct TerrainVaryings
{
    float4 position : SV_Position;
    [[vk::location(0)]] float3 world  : TEXCOORD0;
    [[vk::location(1)]] float3 normal : TEXCOORD1;
    [[vk::location(2)]] float  spacing: TEXCOORD2;
};

TerrainVaryings vertTerrain(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID)
{
    uint n      = uint(tpc.params.x + 0.5);
    uint quad   = vertexID / 6u;
    uint in_tri = vertexID % 3u;
    uint second = (vertexID / 3u) & 1u;
    uint cx     = quad % n;
    uint cz     = quad / n;

    // Two triangles per quad, wound so the normal is +Y: (a,c,b) and (a,d,c)
    // with a = (0,0), b = (1,0), c = (1,1), d = (0,1).
    float2 corner;
    if (second == 0u) corner = in_tri == 0u ? float2(0.0, 0.0) : in_tri == 1u ? float2(1.0, 1.0) : float2(1.0, 0.0);
    else              corner = in_tri == 0u ? float2(0.0, 0.0) : in_tri == 1u ? float2(0.0, 1.0) : float2(1.0, 1.0);

    // A clipmap ring covers an annulus, not a disc: the middle of every ring
    // past the first is already covered by the next finer one, so those
    // triangles are collapsed here rather than rasterized and then lost the
    // depth test. One integer comparison, no CPU work, no index buffer.
    //
    // The ring's spacing is a geometric progression in the instance index and
    // its centre is the camera snapped to that spacing, which is what keeps the
    // grid from swimming: while the camera moves less than one spacing the
    // snapped centre does not move, and when it does move it moves by exactly
    // one spacing, so every vertex lands where a vertex already was.
    float  spacing = tpc.params.w * exp2(float(instanceID));
    float2 snapped = floor(tpc.eye.xz / spacing + 0.5) * spacing;
    float2 xz0     = snapped - spacing * (float(n) * 0.5);

    bool   hole = instanceID > 0u
                  && cx >= n / 4u && cx < n - n / 4u
                  && cz >= n / 4u && cz < n - n / 4u;

    float2 xz   = xz0 + (float2(cx, cz) + corner) * spacing;
    float  h    = fsTerrainHeight(xz);

    TerrainVaryings o;
    // A collapsed triangle puts all three vertices on the same point behind the
    // near plane: zero area, never rasterized, and cheap to produce.
    o.position  = hole ? float4(0.0, 0.0, 2.0, 1.0)
                       : mul(tpc.view_proj, float4(xz.x, h, xz.y, 1.0));
    o.world     = float3(xz.x, h, xz.y);
    // The normal's step is the ring spacing. On the coarsest ring the vertices
    // are kilometres apart, and a smaller step would sample detail the mesh
    // cannot represent, which reads as noise crawling over a smooth slope.
    o.normal    = fsTerrainNormal(xz, spacing);
    o.spacing   = spacing;
    return o;
}

SceneOut fragTerrain(TerrainVaryings i)
{
    float3 n      = normalize(i.normal);
    float3 to_eye = normalize(tpc.eye.xyz - i.world);
    float3 sun    = normalize(tpc.sun.xyz);
    float  slope  = saturate(1.0 - n.y);          // 0 flat, 1 a cliff
    float  h      = i.world.y;

    // Ground cover from height and slope alone: silt at the waterline, grass,
    // forest, rock on anything too steep to hold soil, snow above the line.
    // Every term is a smoothstep of a value already computed, so there is no
    // texture fetch and nothing to tile visibly at 90 km.
    float shore  = fsRemap(h, -6.0, 26.0);
    float alpine = fsRemap(h, 1250.0, 2050.0);
    float trees  = fsRemap(h, 60.0, 420.0) * (1.0 - alpine);
    float3 albedo = lerp(float3(0.42, 0.38, 0.29), float3(0.16, 0.26, 0.11), shore);
    albedo        = lerp(albedo, float3(0.09, 0.17, 0.08), trees * 0.75);
    albedo        = lerp(albedo, float3(0.30, 0.29, 0.28), saturate(slope * 2.6 - 0.35));
    albedo        = lerp(albedo, float3(0.86, 0.88, 0.92), alpine * (1.0 - slope * 0.9));

    // Coarse rings lose the finest albedo variation, or the far terrain
    // sparkles against its own vertex spacing.
    albedo       *= lerp(1.0, 0.94, saturate(i.spacing / 900.0));

    float  ndl    = max(dot(n, sun), 0.0);
    float3 amb    = fsSkyRadiance(n, sun, tpc.sun.w, tpc.params.z) * 0.22;
    float3 lit    = albedo * (amb + tpc.sun.w * ndl * float3(1.0, 0.96, 0.90));

    // One specular lobe: wet rock and snow catch the sun, grass does not, and
    // it is what makes a ridgeline read against the sky.
    lit          += pow(max(dot(n, normalize(sun + to_eye)), 0.0), 26.0)
                    * tpc.sun.w * (alpine * 0.5 + slope * 0.12) * float3(1.0, 0.97, 0.92);

    SceneOut o;
    float  dist   = length(tpc.eye.xyz - i.world);
    o.color       = float4(fsAerialPerspective(lit, -to_eye, dist, sun, tpc.sun.w, tpc.params.z), 1.0);
    o.linear      = dist;
    return o;
}

// ---- water ----------------------------------------------------------------

struct WaterPush
{
    float4x4 view_proj;
    float4   eye;         // xyz camera, w seconds
    float4   sun;         // xyz direction TO the sun, w intensity
    float4   params;      // x quad half extent, y sea level, z turbidity, w chop
};
[[vk::push_constant]] ConstantBuffer<WaterPush> wpc;

struct WaterVaryings
{
    float4 position : SV_Position;
    [[vk::location(0)]] float3 world  : TEXCOORD0;
};

WaterVaryings vertWater(uint vertexID : SV_VertexID)
{
    // Same winding as the terrain so the normal is +Y, on one quad centred on
    // the camera. It has to be FS_FAR across: from cruise altitude the sea
    // reaches the horizon and a smaller quad leaves a gap at the bottom of the
    // sky. Snapped to a whole metre by the CPU so it does not swim.
    uint   in_tri = vertexID % 3u;
    uint   second = (vertexID / 3u) & 1u;
    float2 c;
    if (second == 0u) c = in_tri == 0u ? float2(-1.0, -1.0) : in_tri == 1u ? float2(1.0, 1.0) : float2(1.0, -1.0);
    else              c = in_tri == 0u ? float2(-1.0, -1.0) : in_tri == 1u ? float2(-1.0, 1.0) : float2(1.0, 1.0);

    float2 xz   = wpc.eye.xz + c * wpc.params.x;
    WaterVaryings o;
    o.position  = mul(wpc.view_proj, float4(xz.x, wpc.params.y, xz.y, 1.0));
    o.world     = float3(xz.x, wpc.params.y, xz.y);
    return o;
}

SceneOut fragWater(WaterVaryings i)
{
    float2 xz     = i.world.xz;
    float  t      = wpc.eye.w;
    float  chop   = wpc.params.w;

    // Three swell directions plus one hash-driven chop. Analytic because the
    // quad is a single primitive: there is no vertex density to carry a
    // displacement, so the wave has to be a function of the fragment.
    float  h      = 0.0;
    float2 gx     = float2(0.0, 0.0);
    float2 dirs[3] = { float2(1.0, 0.22), float2(-0.48, 0.88), float2(0.31, -0.95) };
    float  amps[3] = { 1.15, 0.72, 0.44 };
    float  freqs[3] = { 0.0042, 0.0091, 0.0173 };
    [ForceUnroll]
    for (int k = 0; k < 3; ++k) {
        float ph  = dot(dirs[k], xz) * freqs[k] * 6.2831853 + t * (0.55 + 0.18 * float(k));
        float s   = sin(ph);
        float c   = cos(ph);
        h        += amps[k] * s * chop;
        gx       += dirs[k] * (freqs[k] * 6.2831853) * amps[k] * c * chop;
    }
    // The chop term: a cheap gradient-noise ripple, and the only part of the
    // sea that needs a hash.
    float2 wp     = xz * 0.11 + float2(t * 0.02, -t * 0.013);
    h            += mu_value_noise2(wp) * 0.28 * chop;
    gx           += float2(mu_value_noise2(wp + float2(0.37, 0.0)) - mu_value_noise2(wp - float2(0.37, 0.0)),
                           mu_value_noise2(wp + float2(0.0, 0.37)) - mu_value_noise2(wp - float2(0.0, 0.37)))
                    * 0.28 * chop * 0.11;

    /* Fade the wave slope by the pixel footprint: past a few kilometres one
     * pixel covers more sea than a chop wavelength, and sampling the gradient
     * once per pixel then aliases into moire across the whole horizon. Waves
     * the pixel cannot resolve average out to a flat mirror, so that is what
     * the fade converges to. */
    float2 fw     = fwidth(xz);
    gx           *= 1.0 / (1.0 + max(fw.x, fw.y) * 0.06);

    float3 n      = normalize(float3(-gx.x, 1.0, -gx.y));
    float3 to_eye = normalize(wpc.eye.xyz - i.world);
    float3 sun    = normalize(wpc.sun.xyz);
    float  dist   = length(wpc.eye.xyz - i.world);

    // Fresnel: the sea is a mirror at a grazing angle and a dark volume
    // straight down. Schlick with water's index is enough.
    float  f0     = 0.02;
    float  f90    = 1.0;
    float  cos_t  = max(dot(n, to_eye), 0.0);
    float  fres   = f0 + (f90 - f0) * pow(1.0 - cos_t, 5.0);

    float3 deep   = float3(0.012, 0.045, 0.062);
    float3 refl   = fsSkyRadiance(reflect(-to_eye, n), sun, wpc.sun.w, wpc.params.z);
    float3 body   = lerp(deep, refl, fres);

    // The sun's path on the water. A tight lobe plus a broad one, because a
    // single lobe either disappears or turns the whole sea into a light box.
    float3 hv     = normalize(sun + to_eye);
    float  ndh    = max(dot(n, hv), 0.0);
    body         += wpc.sun.w * (pow(ndh, 900.0) * 9.0 + pow(ndh, 60.0) * 0.35)
                    * float3(1.0, 0.95, 0.88);

    // Foam on the steep crests: whitecaps read as wind from a thousand metres.
    body          = lerp(body, float3(0.72, 0.76, 0.78), saturate(length(gx) * 2.4 - 0.55) * chop);

    SceneOut o;
    o.color       = float4(fsAerialPerspective(body, -to_eye, dist, sun, wpc.sun.w, wpc.params.z), 1.0);
    o.linear      = dist;
    return o;
}

// ---- sky ------------------------------------------------------------------

struct SkyPush
{
    float4 eye;           // xyz camera
    float4 right_tan;     // xyz camera right,   w tan(fov_y / 2)
    float4 up_aspect;     // xyz camera up,      w aspect
    float4 forward_far;   // xyz camera forward, w far plane
    float4 sun;           // xyz direction TO the sun, w intensity
    float4 params;        // x turbidity, y seconds, z unused, w unused
};
[[vk::push_constant]] ConstantBuffer<SkyPush> spc;

float4 fragSky(FullscreenVaryings i) : SV_Target0
{
    // The ray the sphere tracer builds (demos/sdf): uv.y = 1 is the top of the
    // image, so ndc.y = +1 is up.
    float2 ndc = i.uv * 2.0 - 1.0;
    float3 rd  = normalize(spc.forward_far.xyz
                           + spc.right_tan.xyz * (ndc.x * spc.up_aspect.w * spc.right_tan.w)
                           + spc.up_aspect.xyz * (ndc.y * spc.right_tan.w));
    float3 sky = fsSkyRadiance(rd, normalize(spc.sun.xyz), spc.sun.w, spc.params.x);

    // Above the cloud deck the sky is dark enough for stars, and they cost one
    // hash only where it is already dark.
    float  dark = saturate(0.30 - dot(sky, float3(0.33, 0.34, 0.33)) * 2.2);
    if (dark > 0.0 && rd.y > 0.0) {
        float3 cell = floor(rd * 420.0);
        sky += smoothstep(0.9988, 0.9999, mu_hash01(uint3(int3(cell)))) * dark * 6.0;
    }
    return float4(sky, 1.0);
}

// ---- aircraft -------------------------------------------------------------

struct AircraftPush
{
    float4x4 view_proj;
    float4   eye;          // xyz camera, w seconds
    float4   sun;          // xyz direction TO the sun, w intensity
    float4   params;       // x turbidity, y unused, z unused, w unused
    uint     records;      // byte offset of the model-matrix + colour table
    uint     stride;       // bytes per record
    uint     vertex_base;  // byte offset of the shared airframe vertices
    uint     vertex_count; // vertices in one airframe
};
[[vk::push_constant]] ConstantBuffer<AircraftPush> apc;

// One record, 80 bytes: a model matrix in mu_math's column-major order, built
// on the CPU by mu_mat4_mul_batch, and a colour whose alpha is emission.
struct AircraftRecord
{
    float4 m0, m1, m2, m3;
    float4 color;
};

// One vertex, 32 bytes: the same shape demos/common/mesh.h uses, so the CPU
// builds the airframe out of plain float arrays with no wrapper.
struct AircraftVertex
{
    float4 position;
    float4 normal;
};

struct AircraftVaryings
{
    float4 position : SV_Position;
    [[vk::location(0)]] float3 world  : TEXCOORD0;
    [[vk::location(1)]] float3 normal : TEXCOORD1;
    [[vk::location(2)]] float4 color  : TEXCOORD2;
};

AircraftVaryings vertAircraft(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID)
{
    AircraftVertex v = muScene.Load<AircraftVertex>(apc.vertex_base + vertexID * 32u);
    AircraftRecord r = muScene.Load<AircraftRecord>(apc.records + instanceID * apc.stride);
    float4x4 model   = float4x4(r.m0, r.m1, r.m2, r.m3);

    AircraftVaryings o;
    float3 wp        = mul(model, float4(v.position.xyz, 1.0)).xyz;
    o.position       = mul(apc.view_proj, float4(wp, 1.0));
    o.world          = wp;
    o.normal         = normalize(mul(model, float4(v.normal.xyz, 0.0)).xyz);
    o.color          = r.color;
    return o;
}

SceneOut fragAircraft(AircraftVaryings i)
{
    float3 n      = normalize(i.normal);
    float3 sun    = normalize(apc.sun.xyz);
    float3 to_eye = normalize(apc.eye.xyz - i.world);
    float  ndl    = max(dot(n, sun), 0.0);
    float3 amb    = fsSkyRadiance(n, sun, apc.sun.w, apc.params.x) * 0.35;
    float3 lit    = i.color.rgb * (amb + apc.sun.w * ndl * float3(1.0, 0.97, 0.92));

    lit          += pow(max(dot(n, normalize(sun + to_eye)), 0.0), 48.0) * apc.sun.w * 0.30;
    lit          += i.color.a * i.color.rgb * 3.0;     // beacon and navigation lights

    SceneOut o;
    float  dist   = length(apc.eye.xyz - i.world);
    o.color       = float4(fsAerialPerspective(lit, -to_eye, dist, sun, apc.sun.w, apc.params.x), 1.0);
    o.linear      = dist;
    return o;
}
