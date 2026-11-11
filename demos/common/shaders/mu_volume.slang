// mu_volume.slang — 3D textures in the bindless tables.
//
// A volume (demo_volume(), TargetDesc.depth > 1) takes one slot in the same
// descriptor arrays as 2D images: binding 0 for sampling, binding 1 for
// storage. A descriptor array has one type in the shader, so the 3D ones are
// a second pair of arrays at the same bindings. Index them with the bindless
// index the volume was given, and only with that: indexing a 2D image
// through these, or a volume through the 2D arrays, is undefined.
//
// #include after the 2D tables are declared (mu_gfx, or a compute shader's
// own declarations). slangc reports the shared bindings as warning E39001.
[[vk::binding(0, 0)]] Texture3D<float4> muSampledVolumes[];
[[vk::binding(1, 0)]] [[vk::image_format("rgba16f")]] RWTexture3D<float4> muStorageVolumes[];
