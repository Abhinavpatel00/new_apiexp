// mu_gfx.slang — the vk.h §14 shader ABI as one Slang module.
//
//   set 0, binding 0 : sampled images   [BINDLESS_TEXTURES]
//   set 0, binding 2 : samplers         [BINDLESS_SAMPLERS]
//   set 0, binding 3 : one storage buffer, addressed by byte offset
//   push constants   : <= 256 bytes, declared by each shader
//
// Every demo shader imports this module instead of re-declaring the bindings,
// so the contract lives in exactly one place on the shader side, the way
// `enum { BIND_SAMPLED_IMAGES = 0, ... }` is the one place on the C side.
// Globals an entry point does not use are stripped by slangc, so importing the
// module costs nothing (a shader that only reads the storage buffer does not
// drag the descriptor-indexing capabilities into its SPIR-V).
//
// Binding 1 (storage images) is intentionally not declared here: a storage
// image declaration carries a format, which is a per-shader decision.
module mu_gfx;

public static const uint MU_BIND_SAMPLED_IMAGES = 0;
public static const uint MU_BIND_STORAGE_IMAGES = 1;
public static const uint MU_BIND_SAMPLERS       = 2;
public static const uint MU_BIND_STORAGE_BUFFER = 3;

[[vk::binding(0, 0)]] public Texture2D<float4> muSampledImages[];
[[vk::binding(2, 0)]] public SamplerState      muSamplers[];

// The single device storage buffer. Geometry and scene records are loaded by
// *byte offset* (`muScene.Load<T>(offset)`), which is what arena_alloc() and
// BufSlice.offset hand out. A typed `T verts[]` view would need
// `offset % sizeof(T) == 0`; the arena only guarantees its own alignment
// (64 for ARENA_DEVICE), so a 48-byte record at offset 64 cannot be addressed
// as an array element. Byte addressing has no such constraint.
[[vk::binding(3, 0)]] public ByteAddressBuffer muScene;

// ---- bindless sampling ----------------------------------------------------

/// Implicit-LOD sample; fragment stage only.
public float4 muSample(uint textureIndex, uint samplerIndex, float2 uv)
{
    return muSampledImages[NonUniformResourceIndex(textureIndex)]
        .Sample(muSamplers[NonUniformResourceIndex(samplerIndex)], uv);
}

/// Explicit-LOD sample; legal in every stage.
public float4 muSampleLevel(uint textureIndex, uint samplerIndex, float2 uv, float lod)
{
    return muSampledImages[NonUniformResourceIndex(textureIndex)]
        .SampleLevel(muSamplers[NonUniformResourceIndex(samplerIndex)], uv, lod);
}

// ---- full-screen triangle -------------------------------------------------

public struct FullscreenVaryings
{
    public float4 position : SV_Position;
    [[vk::location(0)]] public float2 uv : TEXCOORD0;
};

/// One triangle covering the viewport: (-1,-1), (3,-1), (-1,3).
/// `uv = p * 0.5 + 0.5`, so with vk.c's negative-height viewport (NDC +y is
/// image row 0) v = 1 is the TOP of the image. Use muFlipV() before sampling a
/// render target with it.
public FullscreenVaryings muFullscreenTriangle(uint vertexIndex)
{
    float2 p = vertexIndex == 0 ? float2(-1.0, -1.0)
             : vertexIndex == 1 ? float2( 3.0, -1.0)
                                : float2(-1.0,  3.0);
    FullscreenVaryings o;
    o.uv       = p * 0.5 + 0.5;
    o.position = float4(p, 0.0, 1.0);
    return o;
}

/// Full-screen uv -> texture uv under vk.c's viewport convention (see above).
public float2 muFlipV(float2 uv) { return float2(uv.x, 1.0 - uv.y); }

// ---- GLSL-compatible helpers ----------------------------------------------

/// GLSL mod(): result has the sign of y. HLSL/Slang fmod() has the sign of x.
public float muMod(float x, float y) { return x - y * floor(x / y); }
