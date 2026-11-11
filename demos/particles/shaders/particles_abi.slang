// particles_abi.slang — layout shared by simulate.slang and draw.slang.
//
//   compSimulate   one thread per particle: birth, curl-noise advection,
//                  and compaction of the live ones into an index list
//   vertDraw       vertex pulling: six vertices per live particle
//   fragDraw       soft additive sprite
//
// The CPU never reads or writes a particle. It uploads 16 bytes a frame: the
// indirect arguments with the vertex count reset to zero.
//
// Layout in the storage buffer:
//   particle (32 B)   float3 position, uint cycle, float3 velocity, float age (0..1)
//   compact  (4 B)    index of the n-th live particle this frame
//   args     (16 B)   VkDrawIndirectCommand {vertexCount, 1, 0, 0}
static const uint PARTICLE_SIZE = 32u;

// ---- life cycle, in integer milliseconds (mirrored in main.c) --------------
// Every particle repeats: alive for `life`, then absent for `rest`. Integer
// arithmetic only, so the CPU can predict the live count exactly.
uint hash32(uint x)
{
    x ^= x >> 16u; x *= 0x7feb352du;
    x ^= x >> 15u; x *= 0x846ca68bu;
    x ^= x >> 16u;
    return x;
}
struct Cycle { uint life, period, cycle, phase; };
Cycle cycleOf(uint i, uint now_ms)
{
    Cycle c;
    c.life   = 2500u + hash32(i * 3u + 1u) % 3500u;
    c.period = c.life + 200u + hash32(i * 3u + 2u) % 1800u;
    uint t   = now_ms + hash32(i * 3u + 3u) % c.period;
    c.cycle  = t / c.period;
    c.phase  = t % c.period;
    return c;
}

struct SimPush
{
    uint  particles, compact, args, count;
    uint  now_ms, elapsed_ms, flags, pad;      // flags bit 0: reset
    float dt, time, curl_strength, curl_scale;
    float drag, buoyancy, emit_radius, emit_speed;
};

struct DrawPush
{
    float4 vp0, vp1, vp2, vp3;   // view-projection, columns
    float4 right_size;           // xyz camera right, w sprite half-size (world)
    float4 up_gain;              // xyz camera up,    w brightness
    uint   particles, compact, view, width;
    uint   height, pad0, pad1, pad2;
};

