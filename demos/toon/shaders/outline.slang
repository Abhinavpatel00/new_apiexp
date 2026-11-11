// outline.slang — inverted-hull outline (URP pass #1 of the tutorial shader).
//
// Two pieces of NiloOutlineUtil.hlsl / NiloZOffset.hlsl, ported:
//
//   inflate = _OutlineWidth * abs(viewSpaceZ) * cameraFOV * 0.00005
//     keeps the outline the same pixel width at any distance and (roughly) any
//     fov. Upstream clamps abs(viewSpaceZ) to [0,1] so the outline thins out
//     beyond one unit; this demo keeps it constant on screen instead.
//
//   z-offset moves an imaginary vertex along the view axis, away from the
//   camera, and overwrites only clip.z, so the hull can be pushed behind the
//   surface by fractions of a view-space unit without moving on screen. Raising
//   it removes outline where the hull is close behind another surface (inner
//   lines first, e.g. around eyes and mouth), exactly what upstream uses it for.
//
// The pass itself is CULL_FRONT + depth test LESS: the inflated hull's back
// faces only survive where the object does not cover them, i.e. on the
// silhouette. That is why the pass must run after the opaque pass.
//
// Push (192 B) must match OutlinePush in demos/toon/main.c.
import mu_gfx;
import toon_common;

struct OutlinePush
{
    column_major float4x4 view_proj;
    column_major float4x4 model;
    float4 params; // x width, y z-offset, z near, w far
    float4 camera; // xyz eye, w vertical fov in degrees
    float4 color;  // rgb outline colour, a alpha
    float4 meta;   // x vertex byte offset, y outline width scale
};
[[vk::push_constant]] ConstantBuffer<OutlinePush> pc;

[shader("vertex")]
float4 vertMain(uint vertexIndex : SV_VertexID) : SV_Position
{
    ToonVertex v = toonLoadVertex(uint(pc.meta.x), vertexIndex);

    float3 position_ws = mul(pc.model, float4(v.pos.xyz, 1.0)).xyz;
    float3 normal_ws   = normalize(mul((float3x3)pc.model, v.nrm.xyz));

    float4 clip = mul(pc.view_proj, float4(position_ws, 1.0));

    // Right-handed view space, looking down -Z: clip.w == -viewZ, so a positive
    // clip.w means the vertex is in front of the camera.
    float view_z = -clip.w;
    float width  = pc.params.x * pc.meta.y * 0.00005;

    // Keep the outline similar width on screen across distance and fov.
    float camera_mul_fix = clamp(abs(view_z), 0.0, 1.0e4) * pc.camera.w;
    position_ws += normal_ws * (width * camera_mul_fix);
    clip = mul(pc.view_proj, float4(position_ws, 1.0));

    float z_offset = pc.params.y;
    if (z_offset > 0.0)
    {
        // Projection rows for depth, rebuilt from near/far (see
        // mu_mat4_perspective_zo: P22 = far/(near-far), P23 = far*near/(near-far)).
        float p22 = pc.params.w / (pc.params.z - pc.params.w);
        float p23 = pc.params.w * pc.params.z / (pc.params.z - pc.params.w);
        // Upstream (NiloZOffset.hlsl): modifiedPositionVS_Z =
        //   -originalPositionCS.w + -viewSpaceZOffsetAmount
        // i.e. the imaginary vertex moves AWAY from the camera (view z is
        // negative in front of it), which hides the hull behind the surface it
        // belongs to. The GLSL version of this file added the offset instead,
        // following upstream's "push towards camera" comment rather than its
        // code; that pulled the hull forward and made inner lines appear as the
        // slider went up.
        float modified_view_z = view_z - z_offset;
        float modified_w      = -modified_view_z;
        float modified_clip_z = p22 * modified_view_z + p23;
        clip.z = modified_clip_z * (clip.w / modified_w);
    }

    return clip;
}

// The tutorial's outline pass reuses the lit fragment shader and multiplies the
// result by _OutlineColor (ConvertSurfaceColorToOutlineColor). This demo uses a
// constant colour instead, so the pass does not pay for a lighting evaluation.
[shader("fragment")]
float4 fragMain() : SV_Target0
{
    return float4(pc.color.rgb, pc.color.a);
}
