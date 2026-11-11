// Public tutorial lighting port plus a separate, original extended toon model.
// See README.md for provenance, bindings, host obligations, and known differences.
// Vulkan SPIR-V; all colors are linear. Matrix/vector convention: mul(M, column).

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
    float4 shadowParams;            // y normalized depth bias, z receiver offset WS; x/w unused
    float4 outlineCamera;           // x vertical FOV degrees, y ortho half-height, z isOrtho, w unused
};
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
    float4 outlineParams;           // x depth offset in view units, y/z mask remap, w unused
    // Independent model controls (no claim of NiloToonURP equivalence).
    float4 bandParams;              // x second threshold, y second softness, z face-map enabled, w unused
    float4 midColor;                // rgb middle band tint
    float4 specularParams;          // x exponent, y threshold, z softness, w intensity
    float4 specularColor;           // rgb
    float4 rimParams;               // x power, y threshold, z softness, w intensity
    float4 rimColor;                // rgb
};
struct PointLight { float4 positionInvRange; float4 colorIntensity; };
[[vk::binding(0,0)]] ConstantBuffer<FrameData> frame;
[[vk::binding(1,0)]] ConstantBuffer<MaterialData> material;
[[vk::binding(2,0)]] Texture2D<float4> baseMap;
[[vk::binding(3,0)]] Texture2D<float4> emissionMap;
[[vk::binding(4,0)]] Texture2D<float4> occlusionMap;
[[vk::binding(5,0)]] Texture2D<float4> outlineMask;
[[vk::binding(6,0)]] Texture2D<float> shadowMap;
[[vk::binding(7,0)]] Texture2D<float4> faceMap;
[[vk::binding(8,0)]] SamplerState linearSampler;
[[vk::binding(9,0)]] StructuredBuffer<PointLight> pointLights;
[[vk::binding(10,0)]] ConstantBuffer<uint4> lightCounts; // x point lights, max 8
[[vk::binding(11,0)]] SamplerState shadowSampler; // nearest, non-comparison

struct VertexIn {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal : NORMAL;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
};
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
Varyings vertexCommon(VertexIn v, bool outline) {
    Varyings o;
    float3 p = mul(frame.objectToWorld, float4(v.position,1)).xyz;
    float3 n = safeNorm(mul(frame.normalToWorld, float4(v.normal,0)).xyz);
    float4 originalView = mul(frame.worldToView, float4(p,1));
    if (outline) {
        float cameraFix = frame.outlineCamera.z > 0.5
            ? saturate(abs(frame.outlineCamera.y))*50.0
            : saturate(abs(originalView.z))*frame.outlineCamera.x;
        p += n * material.outlineColorWidth.w * cameraFix * 0.00005;
    }
    float4 viewPos = mul(frame.worldToView, float4(p,1));
    float4 clipPos = mul(frame.viewToClip, viewPos);
    if (outline) {
        float m = 1.0-outlineMask.SampleLevel(linearSampler, v.uv, 0).r;
        m = remapClamp(material.outlineParams.y, material.outlineParams.z, m);
        float offset = material.outlineParams.x*m + 0.03*material.celParams.z;
        // Camera convention: view looks down -Z; negative Z moves away from camera.
        float4 shiftedView = viewPos; shiftedView.z -= offset;
        float4 shiftedClip = mul(frame.viewToClip, shiftedView);
        if (abs(shiftedClip.w) > 1e-6) clipPos.z = shiftedClip.z/shiftedClip.w*clipPos.w;
    }
    o.position=clipPos; o.positionWS=p; o.normalWS=n;
    o.uv=v.uv*material.baseUV.xy+material.baseUV.zw;
    o.fogDistance=abs(originalView.z);
    return o;
}
[shader("vertex")] Varyings vertMain(VertexIn v) { return vertexCommon(v,false); }
[shader("vertex")] Varyings vertOutline(VertexIn v) { return vertexCommon(v,true); }

struct Surface { float3 albedo; float alpha; float3 emission; float ao; };
Surface getSurface(Varyings i) {
    Surface s; float4 base = baseMap.Sample(linearSampler,i.uv)*material.baseColor;
    if (material.alphaParams.x > 0.5) { if(base.a < material.alphaParams.y) discard; }
    s.albedo=base.rgb; s.alpha=base.a;
    s.emission=0;
    if(material.emissionColor.w > 0.5)
        s.emission=emissionMap.Sample(linearSampler,i.uv).rgb*material.emissionChannel.rgb*material.emissionColor.rgb;
    s.ao=1;
    if(material.occlusionParams.x > 0.5) {
        float ao=dot(occlusionMap.Sample(linearSampler,i.uv),material.occlusionChannel);
        ao=lerp(1.0,ao,material.occlusionParams.y);
        s.ao=remapClamp(material.occlusionParams.z,material.occlusionParams.w,ao);
    }
    return s;
}
// Single directional shadow map, depth stored as conventional 0 near / 1 far.
// Explicit point sample 3x3 PCF: host supplies nearest non-comparison sampler.
float mainShadow(float3 p) {
    if (frame.mainDirectionShadow.w < 0.5) return 1;
    p += safeNorm(frame.mainDirectionShadow.xyz)*(frame.shadowParams.z+material.celParams.z);
    float4 q=mul(frame.worldToShadow,float4(p,1));
    if (q.w <= 0) return 1;
    float3 ndc=q.xyz/q.w;
    float2 uv=ndc.xy*0.5+0.5; // shadow projection must encode any Vulkan Y flip
    if(any(uv < 0.0) || any(uv > 1.0) || ndc.z < 0 || ndc.z > 1) return 1;
    uint width,height; shadowMap.GetDimensions(width,height);
    float2 texel=1.0/float2(width,height);
    float sum=0;
    for(int y=-1;y<=1;y++) for(int x=-1;x<=1;x++) {
        float2 suv=uv+float2(x,y)*texel;
        if(any(suv < 0.0)||any(suv > 1.0)) sum+=1;
        else sum += (ndc.z-frame.shadowParams.y <= shadowMap.SampleLevel(shadowSampler,suv,0)) ? 1.0 : 0.0;
    }
    return sum/9.0;
}
float3 tutorialLight(Surface s, float3 n, float3 L, float3 color, float attenuation, float shadow, bool additional) {
    float t=smoothBand(material.celParams.x,material.celParams.y,dot(n,L))*s.ao;
    if(material.celParams.z > 0.5) t=lerp(0.5,1.0,t);
    t*=lerp(1.0,shadow,material.celParams.w);
    return saturate(color)*lerp(material.shadowColor.rgb,float3(1,1,1),t)*min(4.0,attenuation)*(additional?0.25:1.0);
}
float3 indirect(Surface s) { return max(material.indirectMin.rgb,frame.indirectColor.rgb)*lerp(1.0,s.ao,0.5); }
float3 emission(Surface s) { return lerp(s.emission,s.emission*s.albedo,material.emissionChannel.w); }
float3 fog(float3 color,float distance) {
    if(frame.fogParams.z < 0.5) return color;
    float vis=1.0-remapClamp(frame.fogParams.x,frame.fogParams.y,distance);
    return lerp(frame.fogColor.rgb,color,vis);
}
float3 pointDirection(PointLight l,float3 p,out float attenuation) {
    float3 d=l.positionInvRange.xyz-p; float dist2=max(dot(d,d),1e-8);
    float rangeFactor=saturate(1.0-sqrt(dist2)*l.positionInvRange.w);
    attenuation=rangeFactor*rangeFactor/max(dist2,0.01);
    return d*rsqrt(dist2);
}
float3 shadeTutorial(Surface s,Varyings i) {
    float3 n=safeNorm(i.normalWS);
    float3 direct=tutorialLight(s,n,safeNorm(frame.mainDirectionShadow.xyz),frame.mainColor.rgb,1,mainShadow(i.positionWS),false);
    for(uint k=0;k<min(lightCounts.x,8u);k++) {
        PointLight l=pointLights[k]; float a; float3 L=pointDirection(l,i.positionWS,a);
        direct+=tutorialLight(s,n,L,l.colorIntensity.rgb*l.colorIntensity.w,a,1,true);
    }
    return s.albedo*max(indirect(s),direct)+emission(s);
}
// Original extended model: 3 diffuse bands, thresholded Blinn-Phong, rim,
// optional artist-painted face threshold. These additions are not a Nilo port.
float3 shadeExtended(Surface s,Varyings i) {
    float3 n=safeNorm(i.normalWS), L=safeNorm(frame.mainDirectionShadow.xyz);
    float3 V=safeNorm(frame.cameraPosition.xyz-i.positionWS);
    float NoL=dot(n,L);
    float threshold=material.celParams.x;
    if(material.bandParams.z > 0.5) threshold += faceMap.Sample(linearSampler,i.uv).r*2.0-1.0;
    float lo=smoothBand(threshold,material.celParams.y,NoL)*s.ao;
    float hi=smoothBand(max(threshold,material.bandParams.x),material.bandParams.y,NoL)*s.ao;
    float shadow=lerp(1.0,mainShadow(i.positionWS),material.celParams.w);
    lo*=shadow; hi*=shadow;
    float3 tint=lerp(material.shadowColor.rgb,material.midColor.rgb,lo);
    tint=lerp(tint,float3(1,1,1),hi);
    float3 direct=saturate(frame.mainColor.rgb)*tint;
    for(uint k=0;k<min(lightCounts.x,8u);k++) {
        PointLight l=pointLights[k]; float a; float3 lp=pointDirection(l,i.positionWS,a);
        direct+=tutorialLight(s,n,lp,l.colorIntensity.rgb*l.colorIntensity.w,a,1,true);
    }
    float3 H=safeNorm(L+V);
    float specRaw=pow(saturate(dot(n,H)),max(material.specularParams.x,0.001));
    float spec=smoothBand(material.specularParams.y,material.specularParams.z,specRaw)
               *material.specularParams.w*shadow*step(0.0,NoL);
    float rimRaw=pow(1.0-saturate(dot(n,V)),max(material.rimParams.x,0.001));
    float rim=smoothBand(material.rimParams.y,material.rimParams.z,rimRaw)*material.rimParams.w;
    return s.albedo*max(indirect(s),direct)+emission(s)
        +spec*material.specularColor.rgb*saturate(frame.mainColor.rgb)+rim*material.rimColor.rgb;
}
[shader("fragment")] float4 fragTutorial(Varyings i) : SV_Target0 {
    Surface s=getSurface(i); return float4(fog(shadeTutorial(s,i),i.fogDistance),s.alpha);
}
[shader("fragment")] float4 fragTutorialOutline(Varyings i) : SV_Target0 {
    Surface s=getSurface(i); return float4(fog(shadeTutorial(s,i)*material.outlineColorWidth.rgb,i.fogDistance),s.alpha);
}
[shader("fragment")] float4 fragExtended(Varyings i) : SV_Target0 {
    Surface s=getSurface(i); return float4(fog(shadeExtended(s,i),i.fogDistance),s.alpha);
}
[shader("fragment")] float4 fragExtendedOutline(Varyings i) : SV_Target0 {
    Surface s=getSurface(i); return float4(fog(shadeExtended(s,i)*material.outlineColorWidth.rgb,i.fogDistance),s.alpha);
}
// Pipeline with color writes disabled; also usable for shadow maps if the host supplies light view/projection.
[shader("fragment")] void fragDepth(Varyings i) {
    float a=baseMap.Sample(linearSampler,i.uv).a*material.baseColor.a;
    if(material.alphaParams.x>0.5 && a<material.alphaParams.y) discard;
}
[shader("fragment")] float4 fragNormals(Varyings i) : SV_Target0 {
    float a=baseMap.Sample(linearSampler,i.uv).a*material.baseColor.a;
    if(material.alphaParams.x>0.5 && a<material.alphaParams.y) discard;
    return float4(safeNorm(i.normalWS),0); // use float/SNORM target, not UNORM
}
