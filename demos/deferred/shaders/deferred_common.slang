// deferred_common.slang — layouts shared by cluster.slang and deferred.slang.
//
// Storage buffer:
//   FrameData (320 B)   camera, cluster grid, where everything else is
//   lights    (32 B)    float4 position + radius, float4 colour (linear, premultiplied by intensity)
//   clusters            per cluster: uint count, then `capacity` light indices
//
// Clusters: the view frustum cut into NX x NY screen tiles and NZ depth
// slices. Slices are exponential in view depth, so near and far clusters
// cover about the same number of pixels:
//     slice(d) = floor(log(d / near) / log(far / near) * NZ)
// Everything nearer than `near` belongs to slice 0.

static const uint F_VIEW_PROJ = 0u, F_INV_VIEW_PROJ = 64u, F_VIEW = 128u, F_EYE = 192u, F_FORWARD = 208u,
                  F_PROJ = 224u, F_SIZE = 240u, F_GRID = 256u, F_DATA = 272u, F_GBUFFER = 288u, F_MOON = 304u;
static const uint LIGHT_SIZE = 32u;

uint clusterSlice(float depth, float nearDepth, float farDepth, uint slices)
{
    float s = log(max(depth, nearDepth) / nearDepth) / log(farDepth / nearDepth) * (float)slices;
    return min((uint)s, slices - 1u);
}

// View depth at which slice k begins.
float sliceDepth(uint k, float nearDepth, float farDepth, uint slices)
{
    return nearDepth * pow(farDepth / nearDepth, (float)k / (float)slices);
}
