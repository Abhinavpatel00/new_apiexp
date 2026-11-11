// Bindless port of toon.slang for the mu_gfx shader ABI.
//
// Original fixed-binding package: ../toon.slang (kept for reference and for
// hosts that still provide the descriptor set 0 layout from ../README.md).
// This file uses the mu_gfx contract:
//
//   set 0, binding 0 : Texture2D<float4> sampledImages[]  (bindless)
//   set 0, binding 2 : SamplerState sampledSamplers[]      (bindless)
//   set 0, binding 3 : ByteAddressBuffer scene             (one storage buffer)
//   push constants   : BindlessPush, byte offsets into scene
//
// All per-frame, per-material, point-light and vertex data live in the single
// storage buffer; texture and sampler slots are indices. See BINDLESS.md for
// the byte layout. Every entry point still exports as `main`.
//
// ABI v2 (see BINDLESS.md "ABI v2"). Everything added after the first port is
// opt-in through BindlessPush.flags or through fields that used to be "unused"
// and are neutral at 0, so flags = 0 reproduces the v1 image:
//
//   TOON_FLAG_OBJECT_DATA           per-object matrices in their own record,
//                                   so one FrameData serves every draw
//   TOON_FLAG_MATERIAL_STYLE        MaterialStyle (32 B) follows MaterialData
//   TOON_FLAG_SMOOTH_OUTLINE_NORMAL hull extrudes along a baked, position-
//                                   averaged normal (closes hard-edge gaps)
//   TOON_FLAG_OUTLINE_PIXEL_WIDTH   outline width is in pixels, exact for any
//                                   distance, fov and resolution
//   TOON_FLAG_SHADOW_FLIP_V         shadow map was rendered through a
//                                   negative-height viewport (mu_gfx default)
//   vertShadow                      shadow-caster stage with slope-aware bias
//
// fragTutorial / fragTutorialOutline stay a strict port of the upstream
// equation; the shading additions are only in fragExtended* and fragInspired*.
//
// Coordinate, matrix and color conventions are unchanged from the original:
// row-major matrices, mul(M, column vector), view looks down -Z, linear RGB.

struct FrameData {
    row_major float4x4 objectToWorld;
    row_major float4x4 worldToView;
    row_major float4x4 viewToClip;
    row_major float4x4 normalToWorld; // inverse transpose(objectToWorld), translation ignored
    row_major float4x4 worldToShadow; // shadow clip, z/w in [0,1]
    float4 cameraPosition;          // xyz world
    float4 mainDirectionShadow;     // xyz normalized surface->light, w: shadow enabled 0/1
    float4 mainColor;               // rgb
    float4 indirectColor;           // rgb, host evaluates Unity SampleSH(0) for tutorial matching
    float4 fogColor;                // rgb
    float4 fogParams;               // x start, y end, z enabled, w unused
    float4 shadowParams;            // x caster depth bias WS, y receiver normalized depth bias,
                                    // z receiver offset WS, w caster normal bias WS
    float4 outlineCamera;           // x vertical FOV degrees, y ortho half-height, z isOrtho,
                                    // w render-target height in pixels (pixel-width outlines)
};

// Per-object transform. Read when TOON_FLAG_OBJECT_DATA is set; otherwise the
// two matrices come from FrameData, as in v1 (one FrameData per object).
struct ObjectData {
    row_major float4x4 objectToWorld;
    row_major float4x4 normalToWorld; // inverse transpose(objectToWorld), translation ignored
};                                    // 128 bytes

struct MaterialData {
    float4 baseColor;
    float4 baseUV;                  // xy scale, zw offset, applies to base/emission/AO
    float4 emissionColor;           // rgb color, w use emission
    float4 emissionChannel;         // rgb mask, w multiply by albedo [0,1]
    float4 occlusionChannel;        // rgba dot-product channel selector
    float4 occlusionParams;         // x use AO, y strength, z remap start, w remap end
    float4 indirectMin;             // rgb minimum GI
    float4 celParams;               // x midpoint, y softness, z isFace, w receive shadow amount
    float4 shadowColor;             // rgb
    float4 alphaParams;             // x alpha clip enabled, y cutoff
    float4 outlineColorWidth;       // rgb multiplier, w width
    float4 outlineParams;           // x depth offset in view units, y/z mask remap,
                                    // w pixel-width mode: view distance beyond which the outline
                                    //   stops growing in world space (0 = constant at any range)
    // Independent model controls (no claim of NiloToonURP equivalence).
    float4 bandParams;              // x second threshold, y second softness, z face-map enabled, w unused
    float4 midColor;                // rgb middle band tint
    float4 specularParams;          // x exponent, y threshold, z softness, w intensity
    float4 specularColor;           // rgb
    float4 rimParams;               // x power, y threshold, z softness, w intensity
    float4 rimColor;                // rgb
};

// Optional style block, stored directly after MaterialData (materialOffset +
// 288) and read when TOON_FLAG_MATERIAL_STYLE is set. Used by fragExtended* and
// fragInspired* only. neutralStyle() is what a v1 material behaves like.
struct MaterialStyle {
    float4 shadowHSV;               // x hue shift in turns, y saturation multiplier,
                                    // z value multiplier, w unused: applied to albedo in shadow
    float4 styleParams;             // x band anti-aliasing (0 off; 1 = one pixel of fwidth),
                                    // y cast-shadow toon softness (0 = raw PCF ramp),
                                    // z rim restricted to the lit side 0..1, w unused
};                                  // 32 bytes

struct PointLight { float4 positionInvRange; float4 colorIntensity; };

// 48-byte bindless vertex, identical to demos/toon/shaders/toon_common.slang.
struct ToonVertex {
    float4 pos;   // xyz object position
    float4 nrm;   // xyz object normal
    float2 uv;
    float2 pad;   // octahedral-encoded smoothed outline normal, object space
                  // (TOON_FLAG_SMOOTH_OUTLINE_NORMAL); ignored otherwise
};

// Bindless indices for one material. 0xFFFFFFFF means "not bound"; the shader
// then uses a neutral default (white/black/0.5/far), so a host can omit
// optional maps without writing descriptor slots.
struct MaterialMaps {
    uint baseMap;            // 0
    uint emissionMap;        // 4
    uint occlusionMap;       // 8
    uint outlineMask;        // 12
    uint shadowMap;          // 16
    uint faceMap;            // 20
    uint normalMap;          // 24
    uint materialMap;        // 28  R roughness, G metallic
    uint matcapMap;          // 32
    uint noiseMap;           // 36
    uint sceneLinearDepth;   // 40  positive abs(view.z)
    uint hairShadowMask;     // 44  receiver-aligned 0..1
    uint linearSampler;      // 48
    uint shadowSampler;      // 52  nearest, non-comparison
    uint shadowWidth;        // 56
    uint shadowHeight;       // 60
};                           // 64 bytes

struct BindlessPush {
    uint frameOffset;       // byte offset of FrameData in scene
    uint materialOffset;    // byte offset of MaterialData in scene
    uint inspiredOffset;    // byte offset of InspiredData in scene (0 if none)
    uint mapsOffset;        // byte offset of MaterialMaps in scene
    uint pointLightOffset;  // byte offset of PointLight[0] in scene
    uint pointLightCount;   // clamped to 8
    uint vertexOffset;      // byte offset of ToonVertex[0] in scene
    uint flags;             // BINDLESS_HAS_INSPIRED | TOON_FLAG_* below
    uint objectOffset;      // byte offset of ObjectData (TOON_FLAG_OBJECT_DATA)
    uint reserved0;         // must be 0
    uint reserved1;         // must be 0
    uint reserved2;         // must be 0
};                          // 48 bytes (v1 was 32; push the extra 16 as zeros)

#define BINDLESS_HAS_INSPIRED           (1u << 0)
#define TOON_FLAG_OBJECT_DATA           (1u << 1)
#define TOON_FLAG_MATERIAL_STYLE        (1u << 2)
#define TOON_FLAG_SMOOTH_OUTLINE_NORMAL (1u << 3)
#define TOON_FLAG_OUTLINE_PIXEL_WIDTH   (1u << 4)
#define TOON_FLAG_SHADOW_FLIP_V         (1u << 5)

bool toonFlag(uint bit) { return (pc.flags & bit) != 0u; }

[[vk::binding(0,0)]] Texture2D<float4> sampledImages[];
[[vk::binding(2,0)]] SamplerState sampledSamplers[];
[[vk::binding(3,0)]] ByteAddressBuffer scene;
[[vk::push_constant]] ConstantBuffer<BindlessPush> pc;

struct Ctx {
    FrameData frame;
    MaterialData material;
    MaterialMaps maps;
    MaterialStyle style;
};

MaterialStyle neutralStyle() {
    MaterialStyle s;
    s.shadowHSV = float4(0, 1, 1, 0);
    s.styleParams = float4(0, 0, 0, 0);
    return s;
}

Ctx loadCtx() {
    Ctx c;
    c.frame = scene.Load<FrameData>(pc.frameOffset);
    c.material = scene.Load<MaterialData>(pc.materialOffset);
    c.maps = scene.Load<MaterialMaps>(pc.mapsOffset);
    c.style = neutralStyle();
    if (toonFlag(TOON_FLAG_MATERIAL_STYLE))
        c.style = scene.Load<MaterialStyle>(pc.materialOffset + 288u);
    return c;
}

ObjectData loadObject(Ctx c) {
    ObjectData o;
    o.objectToWorld = c.frame.objectToWorld;
    o.normalToWorld = c.frame.normalToWorld;
    if (toonFlag(TOON_FLAG_OBJECT_DATA))
        o = scene.Load<ObjectData>(pc.objectOffset);
    return o;
}

// ---- bindless sampling with neutral fallbacks -----------------------------

#define NO_TEXTURE 0xFFFFFFFFu

float4 sampleLinearOr(Ctx c, uint tex, float2 uv, float4 fallback) {
    if (tex == NO_TEXTURE) return fallback;
    return sampledImages[NonUniformResourceIndex(tex)]
        .Sample(sampledSamplers[NonUniformResourceIndex(c.maps.linearSampler)], uv);
}

float4 sampleLinearLevelOr(Ctx c, uint tex, float2 uv, float lod, float4 fallback) {
    if (tex == NO_TEXTURE) return fallback;
    return sampledImages[NonUniformResourceIndex(tex)]
        .SampleLevel(sampledSamplers[NonUniformResourceIndex(c.maps.linearSampler)], uv, lod);
}

float4 sampleShadowOr(Ctx c, uint tex, float2 uv, float4 fallback) {
    if (tex == NO_TEXTURE) return fallback;
    return sampledImages[NonUniformResourceIndex(tex)]
        .SampleLevel(sampledSamplers[NonUniformResourceIndex(c.maps.shadowSampler)], uv, 0);
}

// ---- shared math ---------------------------------------------------------

struct VertexIndex { uint index : SV_VertexID; };

struct Varyings {
    float4 position : SV_Position;
    [[vk::location(0)]] float3 positionWS : TEXCOORD0;
    [[vk::location(1)]] float3 normalWS : TEXCOORD1;
    [[vk::location(2)]] float2 uv : TEXCOORD2;
    [[vk::location(3)]] float fogDistance : TEXCOORD3;
};

float3 safeNorm(float3 x) { return x * rsqrt(max(dot(x,x), 1e-20)); }

float remapClamp(float a, float b, float x) {
    // Equal endpoints are undefined in upstream. Here they produce a hard step.
    if (abs(b-a) < 1e-6) return x >= a ? 1.0 : 0.0;
    return saturate((x-a)/(b-a));
}

float smoothBand(float center, float softness, float x) {
    if (softness <= 1e-6) return x >= center ? 1.0 : 0.0;
    return smoothstep(center-softness, center+softness, x);
}

// Octahedral decode, e in [-1,1]^2 (Cigolle et al. 2014, "Survey of Efficient
// Representations for Independent Unit Vectors").
float3 octDecode(float2 e) {
    float3 n = float3(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));
    float t = saturate(-n.z);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return safeNorm(n);
}

// World-space length of the outline extrusion for a vertex at view-space z.
//
// Tutorial mode (default) is the upstream formula, quirks included:
//   width * saturate(|z|) * fovDegrees * 0.00005
// which is a constant screen width only within one unit of the camera, and is
// linear in the fov angle rather than in tan(fov/2).
//
// Pixel mode (TOON_FLAG_OUTLINE_PIXEL_WIDTH and outlineCamera.w > 0) is exact:
// one pixel at view depth |z| covers 2|z|tan(fov/2)/H world units, and
// viewToClip[1][1] is 1/tan(fov/2) (perspective) or 1/halfHeight (orthographic),
// so a width of w pixels is  w * 2 * d / (|P11| * H)  with d = |z| or 1.
float outlineExtrusion(Ctx c, float viewZ) {
    float width = c.material.outlineColorWidth.w;
    bool ortho = c.frame.outlineCamera.z > 0.5;
    float height = c.frame.outlineCamera.w;
    if (toonFlag(TOON_FLAG_OUTLINE_PIXEL_WIDTH) && height > 0.0) {
        float d = 1.0;
        if (!ortho) {
            d = abs(viewZ);
            float maxDistance = c.material.outlineParams.w;
            if (maxDistance > 0.0) d = min(d, maxDistance);
        }
        float p11 = max(abs(c.frame.viewToClip[1][1]), 1e-6);
        return width * 2.0 * d / (p11 * height);
    }
    float cameraFix = ortho
        ? saturate(abs(c.frame.outlineCamera.y))*50.0
        : saturate(abs(viewZ))*c.frame.outlineCamera.x;
    return width * cameraFix * 0.00005;
}

Varyings vertexCommon(Ctx c, VertexIndex vin, bool outline) {
    ToonVertex v = scene.Load<ToonVertex>(pc.vertexOffset + vin.index * 48u);
    ObjectData obj = loadObject(c);

    Varyings o;
    float3 p = mul(obj.objectToWorld, float4(v.pos.xyz,1)).xyz;
    float3 n = safeNorm(mul(obj.normalToWorld, float4(v.nrm.xyz,0)).xyz);
    float4 originalView = mul(c.frame.worldToView, float4(p,1));
    if (outline) {
        // Extruding along the shading normal tears the hull apart wherever a
        // position has several normals (every hard edge). The smoothed normal
        // is the same for all of them, so the hull stays closed.
        float3 extrude = n;
        if (toonFlag(TOON_FLAG_SMOOTH_OUTLINE_NORMAL))
            extrude = safeNorm(mul(obj.normalToWorld, float4(octDecode(v.pad),0)).xyz);
        p += extrude * outlineExtrusion(c, originalView.z);
    }
    float4 viewPos = mul(c.frame.worldToView, float4(p,1));
    float4 clipPos = mul(c.frame.viewToClip, viewPos);
    if (outline) {
        // Vertex stage cannot use implicit-LOD sampling.
        float m = 1.0 - sampleLinearLevelOr(c, c.maps.outlineMask, v.uv, 0, float4(0,0,0,1)).r;
        m = remapClamp(c.material.outlineParams.y, c.material.outlineParams.z, m);
        float offset = c.material.outlineParams.x*m + 0.03*c.material.celParams.z;
        // Camera convention: view looks down -Z; negative Z moves away from camera.
        float4 shiftedView = viewPos; shiftedView.z -= offset;
        float4 shiftedClip = mul(c.frame.viewToClip, shiftedView);
        if (abs(shiftedClip.w) > 1e-6) clipPos.z = shiftedClip.z/shiftedClip.w*clipPos.w;
    }
    o.position=clipPos; o.positionWS=p; o.normalWS=n;
    o.uv=v.uv*c.material.baseUV.xy+c.material.baseUV.zw;
    o.fogDistance=abs(originalView.z);
    return o;
}

[shader("vertex")] Varyings vertMain(VertexIndex vin) {
    return vertexCommon(loadCtx(), vin, false);
}
[shader("vertex")] Varyings vertOutline(VertexIndex vin) {
    return vertexCommon(loadCtx(), vin, true);
}

// Shadow caster: pair with fragDepth, depth-only pipeline, no culling.
//
// The upstream ShadowCaster pass calls URP's ApplyShadowBias and clamps z to
// the near plane. Same two steps here, with the biases in world units:
//   depth bias   moves the caster away from the light by shadowParams.x
//   normal bias  moves it against its normal by shadowParams.w * (1 - N.L),
//                i.e. more where the surface grazes the light, where a constant
//                depth bias is not enough and acne appears
// worldToShadow is expected to be orthographic (directional light), so
// clamping clip.z to 0 "pancakes" casters that sit in front of the light's
// near plane instead of clipping them away.
[shader("vertex")] Varyings vertShadow(VertexIndex vin) {
    Ctx c = loadCtx();
    ToonVertex v = scene.Load<ToonVertex>(pc.vertexOffset + vin.index * 48u);
    ObjectData obj = loadObject(c);

    float3 p = mul(obj.objectToWorld, float4(v.pos.xyz,1)).xyz;
    float3 n = safeNorm(mul(obj.normalToWorld, float4(v.nrm.xyz,0)).xyz);
    float3 L = safeNorm(c.frame.mainDirectionShadow.xyz);
    p -= L * c.frame.shadowParams.x;
    p -= n * (c.frame.shadowParams.w * (1.0 - saturate(dot(n, L))));

    Varyings o;
    o.position = mul(c.frame.worldToShadow, float4(p,1));
    o.position.z = max(o.position.z, 0.0);
    o.positionWS = p; o.normalWS = n;
    o.uv = v.uv*c.material.baseUV.xy+c.material.baseUV.zw;
    o.fogDistance = 0.0;
    return o;
}

struct Surface { float3 albedo; float alpha; float3 emission; float ao; };

Surface getSurface(Ctx c, Varyings i) {
    Surface s;
    float4 base = sampleLinearOr(c, c.maps.baseMap, i.uv, float4(1,1,1,1)) * c.material.baseColor;
    if (c.material.alphaParams.x > 0.5) { if(base.a < c.material.alphaParams.y) discard; }
    s.albedo=base.rgb; s.alpha=base.a;
    s.emission=0;
    if(c.material.emissionColor.w > 0.5)
        s.emission=sampleLinearOr(c, c.maps.emissionMap, i.uv, float4(1,1,1,1)).rgb
                   *c.material.emissionChannel.rgb*c.material.emissionColor.rgb;
    s.ao=1;
    if(c.material.occlusionParams.x > 0.5) {
        float ao=dot(sampleLinearOr(c, c.maps.occlusionMap, i.uv, float4(1,1,1,1)),
                     c.material.occlusionChannel);
        ao=lerp(1.0,ao,c.material.occlusionParams.y);
        s.ao=remapClamp(c.material.occlusionParams.z,c.material.occlusionParams.w,ao);
    }
    return s;
}

// Single directional shadow map, depth stored as conventional 0 near / 1 far.
// Explicit point sample 3x3 PCF: host supplies nearest non-comparison sampler.
float mainShadow(Ctx c, float3 p) {
    if (c.frame.mainDirectionShadow.w < 0.5) return 1;
    p += safeNorm(c.frame.mainDirectionShadow.xyz)*(c.frame.shadowParams.z+c.material.celParams.z);
    float4 q=mul(c.frame.worldToShadow,float4(p,1));
    if (q.w <= 0) return 1;
    float3 ndc=q.xyz/q.w;
    float2 uv=ndc.xy*0.5+0.5; // shadow projection must encode any Vulkan Y flip...
    // ...or the host says the map was rasterized with NDC +y at row 0, which is
    // what a negative-height viewport (the mu_gfx default) produces.
    if (toonFlag(TOON_FLAG_SHADOW_FLIP_V)) uv.y = 1.0 - uv.y;
    if(any(uv < 0.0) || any(uv > 1.0) || ndc.z < 0 || ndc.z > 1) return 1;
    float2 texel=1.0/max(float2(c.maps.shadowWidth,c.maps.shadowHeight),float2(1,1));
    float sum=0;
    for(int y=-1;y<=1;y++) for(int x=-1;x<=1;x++) {
        float2 suv=uv+float2(x,y)*texel;
        if(any(suv < 0.0)||any(suv > 1.0)) sum+=1;
        else sum += (ndc.z-c.frame.shadowParams.y
                     <= sampleShadowOr(c, c.maps.shadowMap, suv, float4(1,1,1,1)).r)
                    ? 1.0 : 0.0;
    }
    return sum/9.0;
}

float3 tutorialLight(Ctx c, Surface s, float3 n, float3 L, float3 color,
                     float attenuation, float shadow, bool additional) {
    float t=smoothBand(c.material.celParams.x,c.material.celParams.y,dot(n,L))*s.ao;
    if(c.material.celParams.z > 0.5) t=lerp(0.5,1.0,t);
    t*=lerp(1.0,shadow,c.material.celParams.w);
    return saturate(color)*lerp(c.material.shadowColor.rgb,float3(1,1,1),t)
        *min(4.0,attenuation)*(additional?0.25:1.0);
}

float3 indirect(Ctx c, Surface s) {
    return max(c.material.indirectMin.rgb,c.frame.indirectColor.rgb)*lerp(1.0,s.ao,0.5);
}
float3 emission(Ctx c, Surface s) {
    return lerp(s.emission,s.emission*s.albedo,c.material.emissionChannel.w);
}
float3 fog(Ctx c, float3 color,float distance) {
    if(c.frame.fogParams.z < 0.5) return color;
    float vis=1.0-remapClamp(c.frame.fogParams.x,c.frame.fogParams.y,distance);
    return lerp(c.frame.fogColor.rgb,color,vis);
}

float3 pointDirection(PointLight l,float3 p,out float attenuation) {
    float3 d=l.positionInvRange.xyz-p; float dist2=max(dot(d,d),1e-8);
    float rangeFactor=saturate(1.0-sqrt(dist2)*l.positionInvRange.w);
    attenuation=rangeFactor*rangeFactor/max(dist2,0.01);
    return d*rsqrt(dist2);
}

// Additional (point) lights use the tutorial equation in every shading model.
float3 additionalLights(Ctx c, Surface s, float3 n, float3 p) {
    float3 sum = 0;
    for(uint k=0;k<min(pc.pointLightCount,8u);k++) {
        PointLight l=scene.Load<PointLight>(pc.pointLightOffset + k*32u);
        float a; float3 L=pointDirection(l,p,a);
        sum+=tutorialLight(c,s,n,L,l.colorIntensity.rgb*l.colorIntensity.w,a,1,true);
    }
    return sum;
}

float3 shadeTutorial(Ctx c, Surface s,Varyings i) {
    float3 n=safeNorm(i.normalWS);
    float3 direct=tutorialLight(c,s,n,safeNorm(c.frame.mainDirectionShadow.xyz),
                                c.frame.mainColor.rgb,1,mainShadow(c,i.positionWS),false);
    direct+=additionalLights(c,s,n,i.positionWS);
    return s.albedo*max(indirect(c,s),direct)+emission(c,s);
}

// ---- style helpers (fragExtended*, fragInspired*) ---------------------------
// All three are identities for neutralStyle().

// Cel band with a screen-space floor on the softness. A band with softness 0
// is a step function of N.L, so its edge aliases like untextured geometry does;
// widening it to `aa` pixels of the interpolated value's footprint gives an
// analytically anti-aliased edge that stays one pixel wide at any distance.
float styledBand(Ctx c, float center, float softness, float x) {
    softness = max(softness, fwidth(x - center) * c.style.styleParams.x);
    return smoothBand(center, softness, x);
}

// The 3x3 PCF result is a 10-level ramp; pushing it through a band gives cast
// shadows the same edge character as the N.L terminator instead of a blur.
float styledShadow(Ctx c, float shadow) {
    float softness = c.style.styleParams.y;
    if (softness <= 0.0) return shadow;
    return smoothstep(0.5 - softness, 0.5 + softness, shadow);
}

float3 rgbToHsv(float3 c) {
    float4 K = float4(0.0, -1.0/3.0, 2.0/3.0, -1.0);
    float4 p = c.g < c.b ? float4(c.bg, K.wz) : float4(c.gb, K.xy);
    float4 q = c.r < p.x ? float4(p.xyw, c.r) : float4(c.r, p.yzx);
    float d = q.x - min(q.w, q.y);
    return float3(abs(q.z + (q.w - q.y) / (6.0*d + 1e-10)), d / (q.x + 1e-10), q.x);
}
float3 hsvToRgb(float3 c) {
    float3 p = abs(frac(c.xxx + float3(1.0, 2.0/3.0, 1.0/3.0)) * 6.0 - 3.0);
    return c.z * lerp(float3(1,1,1), saturate(p - 1.0), c.y);
}

// Albedo used where the surface is in shadow: hue-shifted, (de)saturated and
// darkened instead of only multiplied by one flat shadowColor. `lit` is the
// first band's 0..1 result, so the shift fades in exactly with the terminator.
float3 styledAlbedo(Ctx c, float3 albedo, float lit) {
    if (!toonFlag(TOON_FLAG_MATERIAL_STYLE)) return albedo;
    float3 hsv = rgbToHsv(albedo);
    hsv.x = frac(hsv.x + c.style.shadowHSV.x + 1.0);
    hsv.y = saturate(hsv.y * c.style.shadowHSV.y);
    hsv.z = hsv.z * c.style.shadowHSV.z;
    return lerp(hsvToRgb(hsv), albedo, saturate(lit));
}

// Original extended model: 3 diffuse bands, thresholded Blinn-Phong, rim,
// optional artist-painted face threshold. These additions are not a Nilo port.
float3 shadeExtended(Ctx c, Surface s,Varyings i) {
    float3 n=safeNorm(i.normalWS), L=safeNorm(c.frame.mainDirectionShadow.xyz);
    float3 V=safeNorm(c.frame.cameraPosition.xyz-i.positionWS);
    float NoL=dot(n,L);
    float threshold=c.material.celParams.x;
    if(c.material.bandParams.z > 0.5)
        threshold += sampleLinearOr(c, c.maps.faceMap, i.uv, float4(0.5,0.5,0.5,1)).r*2.0-1.0;
    float lo=styledBand(c,threshold,c.material.celParams.y,NoL)*s.ao;
    float hi=styledBand(c,max(threshold,c.material.bandParams.x),c.material.bandParams.y,NoL)*s.ao;
    float shadow=lerp(1.0,styledShadow(c,mainShadow(c,i.positionWS)),c.material.celParams.w);
    lo*=shadow; hi*=shadow;
    float3 tint=lerp(c.material.shadowColor.rgb,c.material.midColor.rgb,lo);
    tint=lerp(tint,float3(1,1,1),hi);
    float3 direct=saturate(c.frame.mainColor.rgb)*tint;
    direct+=additionalLights(c,s,n,i.positionWS);
    float3 H=safeNorm(L+V);
    float specRaw=pow(saturate(dot(n,H)),max(c.material.specularParams.x,0.001));
    float spec=styledBand(c,c.material.specularParams.y,c.material.specularParams.z,specRaw)
               *c.material.specularParams.w*shadow*step(0.0,NoL);
    float rimRaw=pow(1.0-saturate(dot(n,V)),max(c.material.rimParams.x,0.001));
    float rim=styledBand(c,c.material.rimParams.y,c.material.rimParams.z,rimRaw)*c.material.rimParams.w;
    rim*=lerp(1.0,lo,saturate(c.style.styleParams.z));
    return styledAlbedo(c,s.albedo,lo)*max(indirect(c,s),direct)+emission(c,s)
        +spec*c.material.specularColor.rgb*saturate(c.frame.mainColor.rgb)
        +rim*c.material.rimColor.rgb;
}

[shader("fragment")] float4 fragTutorial(Varyings i) : SV_Target0 {
    Ctx c=loadCtx(); Surface s=getSurface(c,i);
    return float4(fog(c,shadeTutorial(c,s,i),i.fogDistance),s.alpha);
}
[shader("fragment")] float4 fragTutorialOutline(Varyings i) : SV_Target0 {
    Ctx c=loadCtx(); Surface s=getSurface(c,i);
    return float4(fog(c,shadeTutorial(c,s,i)*c.material.outlineColorWidth.rgb,i.fogDistance),s.alpha);
}
[shader("fragment")] float4 fragExtended(Varyings i) : SV_Target0 {
    Ctx c=loadCtx(); Surface s=getSurface(c,i);
    return float4(fog(c,shadeExtended(c,s,i),i.fogDistance),s.alpha);
}
[shader("fragment")] float4 fragExtendedOutline(Varyings i) : SV_Target0 {
    Ctx c=loadCtx(); Surface s=getSurface(c,i);
    return float4(fog(c,shadeExtended(c,s,i)*c.material.outlineColorWidth.rgb,i.fogDistance),s.alpha);
}

// Depth-only fragment stage (alpha clip only). Three uses:
//   vertMain    + fragDepth   depth prepass of the surface
//   vertOutline + fragDepth   depth prepass that includes the outline hull;
//                             upstream's DepthOnly pass defines
//                             ToonShaderIsOutline for exactly this reason, so
//                             depth-based effects (rim, SSAO) see the hull
//   vertShadow  + fragDepth   shadow map
[shader("fragment")] void fragDepth(Varyings i) {
    Ctx c=loadCtx();
    float a=sampleLinearOr(c,c.maps.baseMap,i.uv,float4(1,1,1,1)).a*c.material.baseColor.a;
    if(c.material.alphaParams.x>0.5 && a<c.material.alphaParams.y) discard;
}
[shader("fragment")] float4 fragNormals(Varyings i) : SV_Target0 {
    Ctx c=loadCtx();
    float a=sampleLinearOr(c,c.maps.baseMap,i.uv,float4(1,1,1,1)).a*c.material.baseColor.a;
    if(c.material.alphaParams.x>0.5 && a<c.material.alphaParams.y) discard;
    return float4(safeNorm(i.normalWS),0); // use float/SNORM target, not UNORM
}
