// resolve.slang — HDR scene target to the window/output.
//
// The toon shader produces LDR-ish values by construction (albedo * a light
// term that is at most 1 before the rim add), so there is no tone curve here:
// only exposure and a clamp. The swapchain format is sRGB, so the encode
// happens in the attachment, and the headless PPM writer does the same encode
// on the CPU (see write_ppm() in main.c).
//
// muFlipV(): vk.c's negative-height viewport makes the full-screen triangle's
// v = 1 the top of the screen while texture row 0 is v = 0; without the flip
// the window shows the frame vertically mirrored relative to the headless
// readback of the same target.
import mu_gfx;

struct ResolvePush
{
    uint  texture_index;
    uint  sampler_index;
    float exposure;
    float pad;
};
[[vk::push_constant]] ConstantBuffer<ResolvePush> pc;

[shader("fragment")]
float4 fragMain(FullscreenVaryings i) : SV_Target0
{
    float3 hdr = muSample(pc.texture_index, pc.sampler_index, muFlipV(i.uv)).rgb;
    return float4(saturate(hdr * pc.exposure), 1.0);
}
