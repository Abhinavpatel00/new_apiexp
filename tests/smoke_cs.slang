// Headless smoke-test compute shader for vk.c.
// Uses the ABI: set 0 binding 1 = storage images, binding 3 = one storage buffer.
// Index 0 of binding 1 is the first texture the test creates.
[[vk::binding(3, 0)]]
RWStructuredBuffer<uint> g_storage;

[[vk::binding(1, 0)]]
RWTexture2D<float4> g_out;

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_main(uint3 tid : SV_DispatchThreadID)
{
    g_storage[tid.x] = tid.x * 2u + 1u;
    if (tid.x < 64u)
        g_out[uint2(tid.x, 0)] = float4(float(tid.x) / 63.0f, 0.5f, 0.25f, 1.0f);
}
