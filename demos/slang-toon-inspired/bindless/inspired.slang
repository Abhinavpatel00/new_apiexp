// Bindless port of inspired.slang for the mu_gfx shader ABI.
//
// Includes the bindless base (./toon.slang) and keeps the same inspired
// feature set: face SDF, screen hair mask, GGX, hair glint, MatCap, normal
// map, screen-space rim, and dissolve. Only the resource binding model changed.
// See BINDLESS.md for the storage layout and texture indices.
//
// It is NOT NiloToonURP and contains no recovered proprietary implementation.

// The bindless base lives in the sibling package in this repository. Keeping
// one copy avoids the two ports drifting apart; see BINDLESS.md.
#include "../../slang-toon/bindless/toon.slang"

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

InspiredData neutralInspired() {
    InspiredData d;
    d.faceRight = float4(1,0,0,0);
    d.faceForward = float4(0,0,1,0);
    d.faceParams = float4(0,0.03,0,0);
    d.materialParams = float4(0,1,0.5,0);
    d.pbrParams = float4(0,0.1,0.03,0);
    d.pbrColor = float4(1,1,1,0);
    d.matcapParams = float4(0,0,0,0);
    d.hairParams = float4(0,16,0,0.7);
    d.hairColor = float4(1,1,1,0);
    d.screenParams = float4(1,1,2,0.02);
    d.screenEffects = float4(0,0,0,0);
    d.fadeParams = float4(1,0,0.05,1);
    d.dissolveColor = float4(1,0.3,0.05,0);
    d.tint = float4(1,1,1,1);
    return d;
}

InspiredData loadInspired() {
    if ((pc.flags & BINDLESS_HAS_INSPIRED) == 0u) return neutralInspired();
    return scene.Load<InspiredData>(pc.inspiredOffset);
}

float3 normalFromMap(Ctx c, InspiredData ins, Varyings i) {
    float3 N=safeNorm(i.normalWS);
    if(ins.materialParams.x < 0.5) return N;
    float3 dp1=ddx(i.positionWS), dp2=ddy(i.positionWS);
    float2 dt1=ddx(i.uv), dt2=ddy(i.uv);
    float det=dt1.x*dt2.y-dt1.y*dt2.x;
    if(abs(det)<1e-8) return N;
    float3 T=(dp1*dt2.y-dp2*dt1.y)/det;
    T=safeNorm(T-N*dot(N,T));
    float3 rawB=(-dp1*dt2.x+dp2*dt1.x)/det;
    float handed=dot(cross(N,T),rawB)<0 ? -1.0 : 1.0;
    float3 B=safeNorm(cross(N,T))*handed;
    float3 m=sampleLinearOr(c,c.maps.normalMap,i.uv,float4(0.5,0.5,1,1)).xyz*2.0-1.0;
    m.xy*=ins.materialParams.y;
    return safeNorm(T*m.x+B*m.y+N*m.z);
}

float pixelHash(float2 pixel) {
    // Deterministic spatial noise: same test in forward, depth and normal passes.
    return frac(52.9829189*frac(dot(floor(pixel),float2(0.06711056,0.00583715))));
}

float dissolveValue(Ctx c, float2 uv) {
    return sampleLinearOr(c,c.maps.noiseMap,uv,float4(1,1,1,1)).r;
}

void inspiredClip(Ctx c, InspiredData ins, Varyings i) {
    float alpha=sampleLinearOr(c,c.maps.baseMap,i.uv,float4(1,1,1,1)).a
               *c.material.baseColor.a*ins.tint.a;
    if(c.material.alphaParams.x>0.5 && alpha<c.material.alphaParams.y) discard;
    if(ins.fadeParams.x <= 0.0
       || (ins.fadeParams.x < 1.0 && pixelHash(i.position.xy)>=ins.fadeParams.x)) discard;
    if(dissolveValue(c,i.uv)<ins.fadeParams.y) discard;
}

float faceSDFVisibility(Ctx c, InspiredData ins, Varyings i, float3 L) {
    float3 R=safeNorm(ins.faceRight.xyz), F=safeNorm(ins.faceForward.xyz);
    float side=dot(L,R), forward=dot(L,F);
    float2 uv=i.uv;
    if ((side < 0.0) != (ins.faceParams.z > 0.5)) uv.x=1.0-uv.x;
    float sdf=sampleLinearOr(c,c.maps.faceMap,uv,float4(0.5,0.5,0.5,1)).r;
    // Horizontal angle, 0 at the face front and 1 at its back.
    float a=atan2(abs(side),forward)/3.14159265359;
    return smoothBand(a,ins.faceParams.y,sdf);
}

float screenHair(Ctx c, InspiredData ins, Varyings i) {
    if(ins.screenEffects.z<0.5) return 0;
    float2 dims=max(ins.screenParams.xy,float2(1,1));
    float2 uv=i.position.xy/dims;
    if(any(uv<0) || any(uv>1)) return 0;
    return saturate(sampleShadowOr(c,c.maps.hairShadowMask,uv,float4(0,0,0,1)).r
                    *ins.screenEffects.y);
}

float screenRim(Ctx c, InspiredData ins, Varyings i,float3 N) {
    if(ins.screenEffects.z<0.5 || ins.screenEffects.x<=0) return 0;
    float2 dims=max(ins.screenParams.xy,float2(1,1));
    float3 nv=mul(c.frame.worldToView,float4(N,0)).xyz;
    float2 dir=float2(nv.x,-nv.y);
    dir=dir*rsqrt(max(dot(dir,dir),1e-8));
    float2 uv=(i.position.xy+dir*ins.screenParams.z)/dims;
    if(any(uv<0) || any(uv>1)) return 0;
    float farDepth=sampleShadowOr(c,c.maps.sceneLinearDepth,uv,float4(1e6,1e6,1e6,1)).r;
    return (farDepth > i.fogDistance+ins.screenParams.w ? 1.0 : 0.0)*ins.screenEffects.x;
}

float3 ggx(Ctx c, InspiredData ins, float3 N,float3 L,float3 V,float3 albedo,
           float roughness,float metallic) {
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
    float3 F0=lerp(float3(0.04,0.04,0.04),albedo,metallic)*ins.pbrColor.rgb;
    float3 F=F0+(1.0-F0)*pow(1.0-VoH,5.0);
    float3 spec=D*Gv*Gl*F/max(4.0*NoV*NoL,1e-6)*NoL;
    if(ins.pbrParams.w>0.5) {
        float lum=dot(spec,float3(0.2126,0.7152,0.0722));
        spec=F*smoothBand(ins.pbrParams.y,ins.pbrParams.z,lum);
    }
    return spec*ins.pbrParams.x;
}

float3 inspiredShade(Ctx c, InspiredData ins, Varyings i, Surface s) {
    float3 N=normalFromMap(c,ins,i), L=safeNorm(c.frame.mainDirectionShadow.xyz);
    float3 V=safeNorm(c.frame.cameraPosition.xyz-i.positionWS);
    float NoL=dot(N,L);
    // styledBand / styledShadow / styledAlbedo: ABI v2 style block, identities
    // for a material without TOON_FLAG_MATERIAL_STYLE (see the bindless base).
    float lo=styledBand(c,c.material.celParams.x,c.material.celParams.y,NoL);
    if(ins.faceParams.x>0.5) lo=faceSDFVisibility(c,ins,i,L);
    float hi=styledBand(c,max(c.material.celParams.x,c.material.bandParams.x),
                        c.material.bandParams.y,NoL);
    float shadow=lerp(1.0,styledShadow(c,mainShadow(c,i.positionWS)),c.material.celParams.w);
    float hairShadow=screenHair(c,ins,i);
    lo*=s.ao*shadow*(1-hairShadow); hi*=s.ao*shadow*(1-hairShadow);
    float3 tint=lerp(c.material.shadowColor.rgb,c.material.midColor.rgb,lo);
    tint=lerp(tint,float3(1,1,1),hi);
    float3 direct=saturate(c.frame.mainColor.rgb)*tint;
    direct+=additionalLights(c,s,N,i.positionWS);
    float3 color=styledAlbedo(c,s.albedo,lo)*max(indirect(c,s),direct)+emission(c,s);

    float4 mr=sampleLinearOr(c,c.maps.materialMap,i.uv,float4(1,1,1,1));
    float roughness=clamp(ins.materialParams.z*mr.r,0.04,1.0);
    float metallic=saturate(ins.materialParams.w*mr.g);
    color+=ggx(c,ins,N,L,V,s.albedo,roughness,metallic)
          *saturate(c.frame.mainColor.rgb)*shadow*(1-hairShadow);

    // Hair ring is an approximate anisotropic lobe about a head-up strand direction.
    float3 up=safeNorm(cross(ins.faceForward.xyz,ins.faceRight.xyz));
    float3 T=safeNorm(up-N*dot(up,N)+N*ins.hairParams.z);
    float3 H=safeNorm(L+V);
    float th=clamp(dot(T,H),-1.0,1.0);
    float lobe=pow(sqrt(saturate(1.0-th*th)),max(ins.hairParams.y,0.001));
    float ring=smoothBand(ins.hairParams.w,0.03,lobe)
              *ins.hairParams.x*shadow*step(0.0,NoL);
    color+=ring*ins.hairColor.rgb*saturate(c.frame.mainColor.rgb);

    float3 nv=safeNorm(mul(c.frame.worldToView,float4(N,0)).xyz);
    float2 matUV=float2(nv.x*0.5+0.5,0.5-nv.y*0.5);
    float3 mc=sampleLinearOr(c,c.maps.matcapMap,matUV,float4(1,1,1,1)).rgb;
    float matMask=lerp(1.0,1.0-lo,saturate(ins.matcapParams.z));
    color*=lerp(float3(1,1,1),mc,saturate(ins.matcapParams.y)*matMask);
    color+=mc*ins.matcapParams.x*matMask;
    color=lerp(color,mc,saturate(ins.matcapParams.w)*matMask);

    float rim=screenRim(c,ins,i,N)*lerp(1.0,lo,saturate(c.style.styleParams.z));
    color+=rim*c.material.rimColor.rgb;

    float noise=dissolveValue(c,i.uv);
    float edge=(1.0-smoothBand(ins.fadeParams.y+ins.fadeParams.z*0.5,
                               max(ins.fadeParams.z*0.5,0.00001),noise));
    if(ins.fadeParams.y>0 && ins.fadeParams.z>0)
        color+=edge*ins.fadeParams.w*ins.dissolveColor.rgb;
    return color;
}

[shader("fragment")] float4 fragInspired(Varyings i) : SV_Target0 {
    Ctx c=loadCtx(); InspiredData ins=loadInspired();
    inspiredClip(c,ins,i);
    Surface s=getSurface(c,i); s.albedo*=ins.tint.rgb; s.alpha*=ins.tint.a;
    return float4(fog(c,inspiredShade(c,ins,i,s),i.fogDistance),s.alpha);
}
[shader("fragment")] float4 fragInspiredOutline(Varyings i) : SV_Target0 {
    Ctx c=loadCtx(); InspiredData ins=loadInspired();
    inspiredClip(c,ins,i);
    Surface s=getSurface(c,i); s.albedo*=ins.tint.rgb; s.alpha*=ins.tint.a;
    return float4(fog(c,inspiredShade(c,ins,i,s)*c.material.outlineColorWidth.rgb,
                      i.fogDistance),s.alpha);
}
[shader("fragment")] void fragInspiredDepth(Varyings i) {
    Ctx c=loadCtx(); InspiredData ins=loadInspired();
    inspiredClip(c,ins,i);
}
[shader("fragment")] float4 fragInspiredNormals(Varyings i) : SV_Target0 {
    Ctx c=loadCtx(); InspiredData ins=loadInspired();
    inspiredClip(c,ins,i);
    return float4(normalFromMap(c,ins,i),0);
}
