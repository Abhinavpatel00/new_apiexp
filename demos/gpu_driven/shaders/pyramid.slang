// pyramid.slang — one level of the depth pyramid: each texel is the FARTHEST
// depth of the texels below it (depth 0 is near, 1 is far). A box whose
// nearest point is beyond that is behind everything drawn there.
//
// A level is half the one below, rounded down. Where the size below is odd,
// a texel takes a third column or row, so no texel below is left out; the
// last texel of a level therefore reaches to the edge.
import mu_gfx;

[[vk::binding(1, 0)]] [[vk::image_format("r32f")]] RWTexture2D<float> muStorageR32[];

struct Push
{
    uint src, dst, src_w, src_h;
    uint dst_w, dst_h, pad0, pad1;
};
[[vk::push_constant]] ConstantBuffer<Push> pc;

[shader("compute")]
[numthreads(8, 8, 1)]
void compReduce(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= pc.dst_w || id.y >= pc.dst_h) return;
    int2  base = (int2)id.xy * 2;
    int2  last = int2((int)pc.src_w, (int)pc.src_h) - 1;
    int   nx   = (pc.src_w & 1u) != 0u ? 3 : 2, ny = (pc.src_h & 1u) != 0u ? 3 : 2;
    float farthest = 0.0;
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i)
            farthest = max(farthest, muSampledImages[NonUniformResourceIndex(pc.src)].Load(
                                         int3(min(base + int2(i, j), last), 0)).r);
    muStorageR32[NonUniformResourceIndex(pc.dst)][id.xy] = farthest;
}
