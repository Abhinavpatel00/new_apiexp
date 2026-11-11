// mu_depth.slang — depth-texture and comparison-sampler views of the bindless
// tables.
//
// vk.h has one sampled-image table (binding 0) and one sampler table (binding
// 2). A shadow map and a comparison sampler live in those same tables; they
// only need a different *type* on the shader side: `Texture2D<float>` so the
// mu_lib shadow filters accept it, and `SamplerComparisonState` for hardware
// depth comparison (SamplerDesc.compare). These declarations alias the two
// bindings with those types. slangc reports the overlap as warning 39001,
// which scripts/build_shaders.sh disables: the overlap is the point.
module mu_depth;

[[vk::binding(0, 0)]] public Texture2D<float>        muDepthImages[];
[[vk::binding(2, 0)]] public SamplerComparisonState  muCompareSamplers[];
