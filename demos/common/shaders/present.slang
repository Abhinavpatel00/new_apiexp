// present.slang — the last pass of a demo (demo_present_pass() in demo.c).
//
// Samples a scene-linear target, applies exposure and one of the mu_lib tone
// mappers, and writes display colour. When the attachment is not an sRGB
// format the shader encodes; otherwise the hardware does.
import mu_gfx;
#include "shaders/post.slang"
#include "shaders/format.slang"

struct PresentPush
{
    uint  source_index;
    uint  sampler_index;
    uint  tonemap;      // DemoTonemap in demo.h
    uint  flags;        // bit 0: encode sRGB, bit 1: dither
    float exposure_ev;
    float pad0, pad1, pad2;
};
[[vk::push_constant]] ConstantBuffer<PresentPush> pc;

float3 applyTonemap(float3 c, uint mode)
{
    switch (mode)
    {
    case 1:  return mu_tonemap_reinhard(c);
    case 2:  return mu_tonemap_aces_fitted(c);
    case 3:  return mu_tonemap_aces(c);
    case 4:  return mu_tonemap_neutral(c);
    case 5:  return mu_tonemap_uchimura(c);
    case 6:  return mu_tonemap_hable(c);
    case 7:  return mu_tonemap_agx(c);
    default: return c;
    }
}

[shader("fragment")]
float4 fragPresent(FullscreenVaryings i) : SV_Target0
{
    float3 c = muSampleLevel(pc.source_index, pc.sampler_index, muFlipV(i.uv), 0.0).rgb;
    c = max(c, 0.0) * exp2(pc.exposure_ev);
    c = saturate(applyTonemap(c, pc.tonemap));
    const bool encode = (pc.flags & 1u) != 0u;
    const bool dither = (pc.flags & 2u) != 0u;
    if (!encode && !dither)
        return float4(c, 1.0);
    // Dither in display space, so the noise is one code value wide after the
    // 8-bit quantization whichever side (shader or attachment) encodes.
    c = mu_linear_to_srgb(c);
    if (dither)
        c = saturate(mu_dither_ign(c, i.position.xy, 1.0 / 255.0));
    return float4(encode ? c : mu_srgb_to_linear(c), 1.0);
}
