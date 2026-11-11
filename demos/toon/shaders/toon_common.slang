// toon_common.slang — records shared by the toon demo's stages.
//
// ToonVertex is the 48-byte record of demos/toon/scene.h. It is loaded by byte
// offset from the storage buffer (set 0, binding 3): no vertex-input state and
// no index buffer, the vk.h §17 pattern.
module toon_common;

import mu_gfx;

public struct ToonVertex
{
    public float4 pos; // xyz object position
    public float4 nrm; // xyz object normal
    public float2 uv;
    public float2 pad; // oct-encoded smoothed outline normal (scene.c), unused here
};

public static const uint TOON_VERTEX_STRIDE = 48u;

public ToonVertex toonLoadVertex(uint byteOffset, uint vertexIndex)
{
    return muScene.Load<ToonVertex>(byteOffset + vertexIndex * TOON_VERTEX_STRIDE);
}
