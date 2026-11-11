// clay_common.slang — the layout shared by clay's shaders and main.c.
//
//   particle   32 B: position (xyz), contacts last step; velocity (xyz), pad
//   predicted  16 B: position (xyz), contacts
//   grid       32^3 cells of 10 cm, 36 B each: count, then up to 8 particle indices
//   volume     64^3 voxels of 5 cm, RGBA16F: r = distance to the nearest particle's surface,
//              exact up to REACH from a centre, clamped beyond
static const uint  PARTICLE_SIZE = 32u, PREDICTED_SIZE = 16u;
static const uint  CELLS = 32u, CELL_CAP = 8u, CELL_STRIDE = 36u;
static const float CELL = 0.1;
static const uint  VOXELS = 64u;
static const float VOXEL = 0.05;
static const float3 ORIGIN = float3(-1.6, 0.0, -1.6);   // the domain's low corner; 3.2 m each way
static const float HALF  = 1.2;                         // the tray's inner half-width in x and z
static const float REACH = 0.1;                         // a voxel sees every particle centre this near

struct Push
{
    uint   particles, src, dst, grid;   // byte offsets
    uint   count, volume, sampler, view;
    float  dt, radius, relax, friction;
    float4 paddle;                      // xyz centre, w radius (0: none)
    float4 look;                        // x smooth-union width, y how far the drawn surface is pushed out
    float4 eye, right, up, forward;     // draw only
    float4 size;
};

int3 cellOf(float3 p) { return clamp(int3(floor((p - ORIGIN) / CELL)), int3(0), int3(CELLS - 1u)); }
uint cellOffset(uint grid, int3 c) { return grid + (uint(c.x) + CELLS * (uint(c.y) + CELLS * uint(c.z))) * CELL_STRIDE; }
