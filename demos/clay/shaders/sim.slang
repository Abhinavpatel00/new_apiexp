// sim.slang — clay as particles: position-based dynamics on a uniform grid.
//
// Per frame:
//   compClear    empty the grid
//   compPredict  x' = x + (v + g dt) dt
//   compBin      put every particle into its cell's list (atomic counter)
//   compSort     order each cell's list by particle index
//   compSolve    xN: push overlapping pairs apart (Jacobi: all read the old
//                positions, each writes its own new one), then out of the
//                tray's walls and the paddle
//   compFinish   v = (x' - x) / dt, damped where it touched something
//   compClear + compBin + compSort again, on the final positions, for the bake
//
// Position-based dynamics: Müller et al. 2007. Averaging the pair corrections
// and scaling them (relax) is from Macklin et al., "Unified Particle Physics",
// 2014. Both from memory. Claybook simulates its clay with particles and
// rebuilds the distance field from them; this is that idea at toy size, not
// its method.
[[vk::binding(3, 0)]] RWByteAddressBuffer muSceneRW;
#include "clay_common.slang"

[[vk::push_constant]] ConstantBuffer<Push> pc;

[shader("compute")]
[numthreads(64, 1, 1)]
void compClear(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x < CELLS * CELLS * CELLS) muSceneRW.Store<uint>(pc.grid + tid.x * CELL_STRIDE, 0u);
}

// Out of the paddle first, then inside the tray: where the two disagree the tray wins.
float3 inTray(float3 p)
{
    float r = pc.radius;
    if (pc.paddle.w > 0.0)
    {
        float3 d = p - pc.paddle.xyz;
        float  l = length(d);
        if (l < pc.paddle.w + r) p = pc.paddle.xyz + (l > 1e-5 ? d / l : float3(0, 1, 0)) * (pc.paddle.w + r);
    }
    p.x = clamp(p.x, -HALF + r, HALF - r);
    p.z = clamp(p.z, -HALF + r, HALF - r);
    p.y = clamp(p.y, r, 3.2 - r);
    return p;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void compPredict(uint3 tid : SV_DispatchThreadID)
{
    uint i = tid.x;
    if (i >= pc.count) return;
    float3 x = muSceneRW.Load<float3>(pc.particles + i * PARTICLE_SIZE);
    float3 v = muSceneRW.Load<float3>(pc.particles + i * PARTICLE_SIZE + 16u);
    v.y -= 9.81 * pc.dt;
    muSceneRW.Store<float4>(pc.dst + i * PREDICTED_SIZE, float4(inTray(x + v * pc.dt), 0.0));
}

[shader("compute")]
[numthreads(64, 1, 1)]
void compBin(uint3 tid : SV_DispatchThreadID)
{
    uint i = tid.x;
    if (i >= pc.count) return;
    uint cell = cellOffset(pc.grid, cellOf(muSceneRW.Load<float3>(pc.src + i * PREDICTED_SIZE)));
    uint slot;
    muSceneRW.InterlockedAdd(cell, 1u, slot);
    if (slot < CELL_CAP) muSceneRW.Store<uint>(cell + 4u + slot * 4u, i);   // a full cell loses the rest: counted by --check
}

// Which slot a particle got depends on which thread came first. Sorting each
// cell's list makes every later sum run in the same order, so a run repeats
// bit for bit.
[shader("compute")]
[numthreads(64, 1, 1)]
void compSort(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= CELLS * CELLS * CELLS) return;
    uint cell  = pc.grid + tid.x * CELL_STRIDE;
    uint count = min(muSceneRW.Load<uint>(cell), CELL_CAP);
    if (count < 2u) return;
    uint list[8];
    for (uint s = 0u; s < count; ++s) list[s] = muSceneRW.Load<uint>(cell + 4u + s * 4u);
    for (uint a = 1u; a < count; ++a)
    {
        uint v = list[a];
        uint b = a;
        while (b > 0u && list[b - 1u] > v) { list[b] = list[b - 1u]; --b; }
        list[b] = v;
    }
    for (uint s = 0u; s < count; ++s) muSceneRW.Store<uint>(cell + 4u + s * 4u, list[s]);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void compSolve(uint3 tid : SV_DispatchThreadID)
{
    uint i = tid.x;
    if (i >= pc.count) return;
    float3 p     = muSceneRW.Load<float3>(pc.src + i * PREDICTED_SIZE);
    int3   c     = cellOf(p);
    float3 push  = float3(0.0);
    float  pairs = 0.0;
    float  both  = 2.0 * pc.radius;
    for (int z = -1; z <= 1; ++z)
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x)
            {
                int3 n = c + int3(x, y, z);
                if (any(n < 0) || any(n >= int(CELLS))) continue;
                uint cell  = cellOffset(pc.grid, n);
                uint count = min(muSceneRW.Load<uint>(cell), CELL_CAP);
                for (uint s = 0u; s < count; ++s)
                {
                    uint j = muSceneRW.Load<uint>(cell + 4u + s * 4u);
                    if (j == i) continue;
                    float3 d = p - muSceneRW.Load<float3>(pc.src + j * PREDICTED_SIZE);
                    float  l = length(d);
                    if (l >= both) continue;
                    // Two particles in the same place: part them along an axis picked by their indices.
                    float3 dir = l > 1e-6 ? d / l : (i < j ? float3(0, 1, 0) : float3(0, -1, 0));
                    push  += dir * 0.5 * (both - l);
                    pairs += 1.0;
                }
            }
    float3 q = inTray(p + push * (pc.relax / max(pairs, 1.0)));
    bool wall = any(abs(q - (p + push * (pc.relax / max(pairs, 1.0)))) > 1e-7);
    muSceneRW.Store<float4>(pc.dst + i * PREDICTED_SIZE, float4(q, pairs + (wall ? 1.0 : 0.0)));
}

[shader("compute")]
[numthreads(64, 1, 1)]
void compFinish(uint3 tid : SV_DispatchThreadID)
{
    uint i = tid.x;
    if (i >= pc.count) return;
    float3 x = muSceneRW.Load<float3>(pc.particles + i * PARTICLE_SIZE);
    float4 q = muSceneRW.Load<float4>(pc.src + i * PREDICTED_SIZE);
    float3 v = (q.xyz - x) / pc.dt;
    if (q.w > 0.0) v *= 1.0 - pc.friction;          // sticky: whatever it touches slows it
    float speed = length(v);
    if (speed > 3.0) v *= 3.0 / speed;              // 5 cm a step: half a cell
    muSceneRW.Store<float4>(pc.particles + i * PARTICLE_SIZE, q);
    muSceneRW.Store<float4>(pc.particles + i * PARTICLE_SIZE + 16u, float4(v, 0.0));
}
