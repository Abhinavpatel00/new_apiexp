// tonemap.slang — HDR scene target to the window/output: exposure and a
// 1 - exp(-x) shoulder.
//
// muFlipV(): vk.c uses a negative-height viewport (NDC +y = image row 0 = top
// of the displayed image), so a full-screen triangle's uv is bottom-up relative
// to the scene target's rows. Without the flip the presented frame is
// vertically mirrored next to the headless readback of the same target.
import mu_gfx;

struct TonePush
{
    uint  texture_index;
    uint  sampler_index;
    float exposure;
    float pad;
};
[[vk::push_constant]] ConstantBuffer<TonePush> pc;

[shader("fragment")]
float4 fragMain(FullscreenVaryings i) : SV_Target0
{
    float3 hdr    = muSample(pc.texture_index, pc.sampler_index, muFlipV(i.uv)).rgb * pc.exposure;
    float3 mapped = float3(1.0, 1.0, 1.0) - exp(-hdr);
    return float4(mapped, 1.0);
}
