// Independent anime-style extension of the public tutorial adaptation in toon.slang.
// It is NOT NiloToonURP and contains no recovered proprietary implementation.
#include "toon.slang"

struct InspiredData {
    float4 faceRight;          // xyz head right WS, normalized
    float4 faceForward;        // xyz head forward WS, normalized
    float4 faceParams;         // x enable face SDF, y softness, z orientation invert, w unused
    float4 materialParams;     // x enable normal map, y normal strength, z roughness, w metallic
    float4 pbrParams;          // x specular intensity, y specular threshold, z softness, w quantize 0/1
    float4 pbrColor;           // rgb F0 tint multiplier, w unused
    float4 matcapParams;       // x additive, y multiply strength, z shadow-only blend, w replace strength
    float4 hairParams;         // x strength, y exponent, z shift, w threshold
    float4 hairColor;          // rgb highlight color
    float4 screenParams;       // xy render target dimensions, z rim width pixels, w rim depth threshold (view units)
    float4 screenEffects;      // x screen rim strength, y hair shadow strength, z screen effects on/off, w unused
    float4 fadeParams;         // x visibility 0..1, y dissolve amount 0..1, z dissolve edge width, w edge strength
    float4 dissolveColor;      // rgb emissive edge
    float4 tint;               // rgba multiplier; alpha influences clip
};
[[vk::binding(12,0)]] ConstantBuffer<InspiredData> inspired;
[[vk::binding(13,0)]] Texture2D<float4> normalMap;
[[vk::binding(14,0)]] Texture2D<float4> materialMap;   // R roughness multiplier; G metallic multiplier
[[vk::binding(15,0)]] Texture2D<float4> matcapMap;
[[vk::binding(16,0)]] Texture2D<float4> noiseMap;
[[vk::binding(17,0)]] Texture2D<float> sceneLinearDepth; // positive abs(view z), nearest
[[vk::binding(18,0)]] Texture2D<float> hairShadowMask;   // receiver-aligned screen mask, 0..1

float3 normalFromMap(Varyings i) {
    float3 N=safeNorm(i.normalWS);
    if(inspired.materialParams.x < 0.5) return N;
    float3 dp1=ddx(i.positionWS), dp2=ddy(i.positionWS);
    float2 dt1=ddx(i.uv), dt2=ddy(i.uv);
    float det=dt1.x*dt2.y-dt1.y*dt2.x;
    if(abs(det)<1e-8) return N;
    float3 T=(dp1*dt2.y-dp2*dt1.y)/det;
    T=safeNorm(T-N*dot(N,T));
    float3 rawB=(-dp1*dt2.x+dp2*dt1.x)/det;
    float handed=dot(cross(N,T),rawB)<0 ? -1.0 : 1.0;
    float3 B=safeNorm(cross(N,T))*handed;
    float3 m=normalMap.Sample(linearSampler,i.uv).xyz*2.0-1.0;
    m.xy*=inspired.materialParams.y;
    return safeNorm(T*m.x+B*m.y+N*m.z);
}
float pixelHash(float2 pixel) {
    // Deterministic spatial noise: same test in forward, depth and normal passes.
    return frac(52.9829189*frac(dot(floor(pixel),float2(0.06711056,0.00583715))));
}
float dissolveValue(float2 uv) { return noiseMap.Sample(linearSampler,uv).r; }
void inspiredClip(Varyings i) {
    float alpha=baseMap.Sample(linearSampler,i.uv).a*material.baseColor.a*inspired.tint.a;
    if(material.alphaParams.x>0.5 && alpha<material.alphaParams.y) discard;
    if(inspired.fadeParams.x <= 0.0 || (inspired.fadeParams.x < 1.0 && pixelHash(i.position.xy)>=inspired.fadeParams.x)) discard;
    if(dissolveValue(i.uv)<inspired.fadeParams.y) discard;
}
float faceSDFVisibility(Varyings i, float3 L) {
    float3 R=safeNorm(inspired.faceRight.xyz), F=safeNorm(inspired.faceForward.xyz);
    float side=dot(L,R), forward=dot(L,F);
    float2 uv=i.uv;
    if ((side < 0.0) != (inspired.faceParams.z > 0.5)) uv.x=1.0-uv.x;
    float sdf=faceMap.Sample(linearSampler,uv).r;
    // Horizontal angle, 0 at the face front and 1 at its back.
    float a=atan2(abs(side),forward)/3.14159265359;
    return smoothBand(a,inspired.faceParams.y,sdf);
}
float screenHair(Varyings i) {
    if(inspired.screenEffects.z<0.5) return 0;
    float2 dims=max(inspired.screenParams.xy,float2(1,1));
    float2 uv=i.position.xy/dims;
    if(any(uv<0) || any(uv>1)) return 0;
    return saturate(hairShadowMask.SampleLevel(shadowSampler,uv,0)*inspired.screenEffects.y);
}
float screenRim(Varyings i,float3 N) {
    if(inspired.screenEffects.z<0.5 || inspired.screenEffects.x<=0) return 0;
    float2 dims=max(inspired.screenParams.xy,float2(1,1));
    float3 nv=mul(frame.worldToView,float4(N,0)).xyz;
    float2 dir=float2(nv.x,-nv.y);
    dir=dir*rsqrt(max(dot(dir,dir),1e-8));
    float2 uv=(i.position.xy+dir*inspired.screenParams.z)/dims;
    if(any(uv<0) || any(uv>1)) return 0;
    float farDepth=sceneLinearDepth.SampleLevel(shadowSampler,uv,0);
    return (farDepth > i.fogDistance+inspired.screenParams.w ? 1.0 : 0.0)*inspired.screenEffects.x;
}
float3 ggx(float3 N,float3 L,float3 V,float3 albedo,float roughness,float metallic) {
    float NoL=saturate(dot(N,L)), NoV=saturate(dot(N,V));
    if(NoL<=0 || NoV<=0) return 0;
    float3 H=safeNorm(L+V);
    float NoH=saturate(dot(N,H)), VoH=saturate(dot(V,H));
    float a=max(roughness*roughness,0.0025), a2=a*a;
    float den=NoH*NoH*(a2-1.0)+1.0;
    float D=a2/max(3.14159265359*den*den,1e-7);
    float k=(roughness+1.0)*(roughness+1.0)/8.0;
    float Gv=NoV/max(NoV*(1.0-k)+k,1e-6);
    float Gl=NoL/max(NoL*(1.0-k)+k,1e-6);
    float3 F0=lerp(float3(0.04,0.04,0.04),albedo,metallic)*inspired.pbrColor.rgb;
    float3 F=F0+(1.0-F0)*pow(1.0-VoH,5.0);
    float3 spec=D*Gv*Gl*F/max(4.0*NoV*NoL,1e-6)*NoL;
    if(inspired.pbrParams.w>0.5) {
        float lum=dot(spec,float3(0.2126,0.7152,0.0722));
        spec=F*smoothBand(inspired.pbrParams.y,inspired.pbrParams.z,lum);
    }
    return spec*inspired.pbrParams.x;
}
float3 inspiredShade(Varyings i, Surface s) {
    float3 N=normalFromMap(i), L=safeNorm(frame.mainDirectionShadow.xyz);
    float3 V=safeNorm(frame.cameraPosition.xyz-i.positionWS);
    float NoL=dot(N,L);
    float lo=smoothBand(material.celParams.x,material.celParams.y,NoL);
    if(inspired.faceParams.x>0.5) lo=faceSDFVisibility(i,L);
    float hi=smoothBand(max(material.celParams.x,material.bandParams.x),material.bandParams.y,NoL);
    float shadow=lerp(1.0,mainShadow(i.positionWS),material.celParams.w);
    float hairShadow=screenHair(i);
    lo*=s.ao*shadow*(1-hairShadow); hi*=s.ao*shadow*(1-hairShadow);
    float3 tint=lerp(material.shadowColor.rgb,material.midColor.rgb,lo);
    tint=lerp(tint,float3(1,1,1),hi);
    float3 direct=saturate(frame.mainColor.rgb)*tint;
    for(uint k=0;k<min(lightCounts.x,8u);k++) {
        PointLight light=pointLights[k]; float a; float3 ld=pointDirection(light,i.positionWS,a);
        direct+=tutorialLight(s,N,ld,light.colorIntensity.rgb*light.colorIntensity.w,a,1,true);
    }
    float3 color=s.albedo*max(indirect(s),direct)+emission(s);
    float4 mr=materialMap.Sample(linearSampler,i.uv);
    float roughness=clamp(inspired.materialParams.z*mr.r,0.04,1.0);
    float metallic=saturate(inspired.materialParams.w*mr.g);
    color+=ggx(N,L,V,s.albedo,roughness,metallic)*saturate(frame.mainColor.rgb)*shadow*(1-hairShadow);
    // Hair ring is an approximate anisotropic lobe about a head-up strand direction.
    float3 up=safeNorm(cross(inspired.faceForward.xyz,inspired.faceRight.xyz));
    float3 T=safeNorm(up-N*dot(up,N)+N*inspired.hairParams.z);
    float3 H=safeNorm(L+V);
    float th=clamp(dot(T,H),-1.0,1.0);
    float lobe=pow(sqrt(saturate(1.0-th*th)),max(inspired.hairParams.y,0.001));
    float ring=smoothBand(inspired.hairParams.w,0.03,lobe)*inspired.hairParams.x*shadow*step(0.0,NoL);
    color+=ring*inspired.hairColor.rgb*saturate(frame.mainColor.rgb);
    float3 nv=safeNorm(mul(frame.worldToView,float4(N,0)).xyz);
    float2 matUV=float2(nv.x*0.5+0.5,0.5-nv.y*0.5);
    float3 mc=matcapMap.Sample(linearSampler,matUV).rgb;
    float matMask=lerp(1.0,1.0-lo,saturate(inspired.matcapParams.z));
    color*=lerp(float3(1,1,1),mc,saturate(inspired.matcapParams.y)*matMask);
    color+=mc*inspired.matcapParams.x*matMask;
    color=lerp(color,mc,saturate(inspired.matcapParams.w)*matMask);
    float rim=screenRim(i,N);
    color+=rim*material.rimColor.rgb;
    float noise=dissolveValue(i.uv);
    float edge=(1.0-smoothBand(inspired.fadeParams.y+inspired.fadeParams.z*0.5,
                               max(inspired.fadeParams.z*0.5,0.00001),noise));
    if(inspired.fadeParams.y>0 && inspired.fadeParams.z>0)
        color+=edge*inspired.fadeParams.w*inspired.dissolveColor.rgb;
    return color;
}
[shader("fragment")] float4 fragInspired(Varyings i) : SV_Target0 {
    inspiredClip(i); Surface s=getSurface(i); s.albedo*=inspired.tint.rgb; s.alpha*=inspired.tint.a;
    return float4(fog(inspiredShade(i,s),i.fogDistance),s.alpha);
}
[shader("fragment")] float4 fragInspiredOutline(Varyings i) : SV_Target0 {
    inspiredClip(i); Surface s=getSurface(i); s.albedo*=inspired.tint.rgb; s.alpha*=inspired.tint.a;
    return float4(fog(inspiredShade(i,s)*material.outlineColorWidth.rgb,i.fogDistance),s.alpha);
}
[shader("fragment")] void fragInspiredDepth(Varyings i) { inspiredClip(i); }
[shader("fragment")] float4 fragInspiredNormals(Varyings i) : SV_Target0 {
    inspiredClip(i); return float4(normalFromMap(i),0);
}
