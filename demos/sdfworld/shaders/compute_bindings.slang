// compute_bindings.slang — the bindless tables as the compute shaders see them.
[[vk::binding(0, 0)]] Texture2D<float4> muSampledImages[];
[[vk::binding(2, 0)]] SamplerState      muSamplers[];
[[vk::binding(1, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> muStorageImages[];
[[vk::binding(3, 0)]] RWByteAddressBuffer muSceneRW;
#include "mu_volume.slang"
