// fullscreen.slang — the shared full-screen-triangle vertex stage.
//
// Compiled once per demo as `sky.vert.spv` (toon) / `fullscreen.vert.spv`
// (galaxy); both are this entry point. See muFullscreenTriangle() for the uv
// convention.
import mu_gfx;

[shader("vertex")]
FullscreenVaryings vertFullscreen(uint vertexIndex : SV_VertexID)
{
    return muFullscreenTriangle(vertexIndex);
}
