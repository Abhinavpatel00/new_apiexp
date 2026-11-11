// simulate.slang — compSimulate. See particles_abi.slang.
#include "shaders/noise.slang"
#include "particles_abi.slang"

// The storage buffer of the bindless set, writable.
[[vk::binding(3, 0)]] RWByteAddressBuffer muSceneRW;

[[vk::push_constant]] ConstantBuffer<SimPush> pc;

[shader("compute")]
[numthreads(64, 1, 1)]
void compSimulate(uint3 tid : SV_DispatchThreadID)
{
    uint i = tid.x;
    if (i >= pc.count) return;
    uint   base   = pc.particles + i * PARTICLE_SIZE;
    uint   stored = (pc.flags & 1u) != 0u ? 0u : muSceneRW.Load<uint>(base + 12u);
    Cycle  c      = cycleOf(i, pc.now_ms);
    float3 pos    = muSceneRW.Load<float3>(base);
    float3 vel    = muSceneRW.Load<float3>(base + 16u);

    if (c.phase < c.life && c.phase < pc.elapsed_ms)
    {
        // Phase 0 passed during this frame: born. Where and how fast is a
        // function of (particle, cycle) alone.
        float3 r   = mu_hash03(uint3(i, c.cycle, 7u));
        float  z   = 2.0 * r.x - 1.0;
        float  a   = 6.2831853 * r.y;
        float3 dir = float3(sqrt(max(1.0 - z * z, 0.0)) * float2(cos(a), sin(a)), z).xzy;
        pos    = float3(0.0, 0.3, 0.0) + dir * pc.emit_radius * pow(r.z, 1.0 / 3.0);
        vel    = dir * pc.emit_speed * (0.4 + r.z) + float3(0.0, pc.emit_speed, 0.0);
        stored = c.cycle + 1u;
    }
    bool alive = c.phase < c.life && stored == c.cycle + 1u;
    if (alive)
    {
        // Divergence-free flow (mu_curl_noise3) plus buoyancy; the particle
        // relaxes towards the flow's velocity.
        float3 q    = pos * pc.curl_scale + float3(0.0, -0.12 * pc.time, 0.0);
        float3 flow = mu_curl_noise3(q, 0.01) * pc.curl_strength + float3(0.0, pc.buoyancy, 0.0);
        vel += (flow - vel) * (1.0 - exp(-pc.drag * pc.dt));
        pos += vel * pc.dt;
        muSceneRW.Store<float3>(base, pos);
        muSceneRW.Store<float3>(base + 16u, vel);
        muSceneRW.Store<float>(base + 28u, (float)c.phase / (float)c.life);

        // Compaction: claim the next six vertices of the indirect draw.
        uint before;
        muSceneRW.InterlockedAdd(pc.args, 6u, before);
        muSceneRW.Store<uint>(pc.compact + (before / 6u) * 4u, i);
    }
    muSceneRW.Store<uint>(base + 12u, stored);
}
