// sky.slang — flat anime backdrop: a two-stop vertical gradient plus a soft sun
// disc. This is scenery, not part of the toon shader: it exists so the cel
// bands and the outline have something neutral to sit against.
//
// The vertex stage is the shared full-screen triangle
// (demos/common/shaders/fullscreen.slang -> sky.vert.spv).
import mu_gfx;

struct SkyPush
{
    float4 horizon; // rgb horizon colour, w gradient exponent
    float4 zenith;  // rgb zenith colour, w sun glow intensity
    float4 sun;     // xy sun position in uv, z sun radius, w pad
    float4 params;  // unused
};
[[vk::push_constant]] ConstantBuffer<SkyPush> pc;

[shader("fragment")]
float4 fragMain(FullscreenVaryings i) : SV_Target0
{
    float  t     = saturate(i.uv.y);
    float3 color = lerp(pc.horizon.rgb, pc.zenith.rgb, pow(t, pc.horizon.w));

    float d        = length((i.uv - pc.sun.xy) * float2(1.0, 1.6));
    float sun_disc = smoothstep(pc.sun.z, pc.sun.z * 0.55, d);
    float halo     = smoothstep(pc.sun.z * 6.0, 0.0, d) * 0.35;
    color += (sun_disc * 1.4 + halo) * pc.zenith.w;

    return float4(color, 1.0);
}
