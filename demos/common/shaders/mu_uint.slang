// mu_uint.slang — unsigned-integer view of the bindless image table.
//
// A render target with an integer format (FMT_R32_UINT: a packed G-buffer
// word) sits in the same table as every other texture (binding 0) but must be
// read through a `Texture2D<uint>`. Like mu_depth.slang this aliases the
// binding with another type; slangc's overlap warning 39001 is disabled in
// scripts/build_shaders.sh. Integer textures cannot be filtered: use Load().
module mu_uint;

[[vk::binding(0, 0)]] public Texture2D<uint> muUintImages[];
