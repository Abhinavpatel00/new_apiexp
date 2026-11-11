// mpm.slang — water and jelly as particles that talk through a grid.
//
// The moving least squares material point method (Hu et al., SIGGRAPH 2018),
// in two dimensions. Lengths are in grid cells. A particle carries position,
// velocity, the velocity gradient C around it and its deformation F. One
// time step is three passes:
//
//   compClear   empty the grid
//   compP2G     every particle adds its mass and momentum to the 3 x 3 nodes
//               round it, with quadratic weights. The momentum includes what
//               its stress does in this step: -4 dt (stress / density) (node - particle)
//   compG2P     every node's momentum / mass is its velocity; gravity and the
//               walls change it; every particle takes the weighted velocity
//               and its gradient back, moves, and deforms: F <- (1 + dt C) F
//
// Many particles add to one node at once, so the grid holds integers and the
// adds are atomic: mass in units of 2^-24 of a particle, momentum in units
// of 2^-14 particle-cell per second. (A coarser unit for mass loses momentum:
// a node whose mass rounds to nothing has no velocity to give back.) Integer adds give the same sum
// in any order, so a run repeats exactly.
//
// Stress / density: water  K (J - 1) 1               J = det F
//                   jelly  mu (F F^T - 1) + lambda ln(J) 1     (neo-Hookean)
// Water forgets its shape: after each step its F becomes sqrt(J) 1.

[[vk::binding(3, 0)]] RWByteAddressBuffer muSceneRW;

struct Push
{
    uint  particles, grid, count, n;      // byte offsets, particle count, nodes along a side
    float dt, gravity, bulk, mu;
    float lambda;
    uint  walls;                          // nodes along each side that are wall: 0 .. walls - 1 and n - walls .. n - 1
    uint  pad0, pad1;
};
[[vk::push_constant]] ConstantBuffer<Push> pc;

static const uint  kParticleSize  = 64u;  // pos 0, vel 8, C 16, F 32, material 48
static const uint  kNodeSize      = 16u;  // mass 0, momentum x 4, y 8
static const float kMassScale     = 16777216.0;
static const float kMomentumScale = 16384.0;

[shader("compute")]
[numthreads(64, 1, 1)]
void compClear(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= pc.n * pc.n) return;
    muSceneRW.Store<uint4>(pc.grid + id.x * kNodeSize, uint4(0u, 0u, 0u, 0u));
}

// The lowest of the 3 x 3 nodes round a particle, and the weights of the
// three along each axis.
int2 stencil(float2 pos, out float2 w[3], out float2 fx)
{
    int2 base = (int2)floor(pos - 0.5);
    fx   = pos - (float2)base;
    w[0] = 0.5 * (1.5 - fx) * (1.5 - fx);
    w[1] = 0.75 - (fx - 1.0) * (fx - 1.0);
    w[2] = 0.5 * (fx - 0.5) * (fx - 0.5);
    return base;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void compP2G(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= pc.count) return;
    uint   at  = pc.particles + id.x * kParticleSize;
    float2 pos = muSceneRW.Load<float2>(at);
    float2 vel = muSceneRW.Load<float2>(at + 8u);
    float4 c   = muSceneRW.Load<float4>(at + 16u);   // c.xy = dv_x / d(x, y), c.zw = dv_y / d(x, y)
    float4 f   = muSceneRW.Load<float4>(at + 32u);
    uint   material = muSceneRW.Load<uint>(at + 48u);

    float  j = f.x * f.w - f.y * f.z;
    float4 stress;
    if (material == 0u)
    {
        stress = float4(pc.bulk * (j - 1.0), 0.0, 0.0, pc.bulk * (j - 1.0));
    }
    else
    {
        float spring = pc.lambda * log(max(j, 0.1));
        stress = float4(pc.mu * (f.x * f.x + f.y * f.y - 1.0) + spring, pc.mu * (f.x * f.z + f.y * f.w),
                        pc.mu * (f.x * f.z + f.y * f.w), pc.mu * (f.z * f.z + f.w * f.w - 1.0) + spring);
    }
    float4 affine = c - 4.0 * pc.dt * stress;

    float2 w[3], fx;
    int2   base = stencil(pos, w, fx);
    for (int gy = 0; gy < 3; ++gy)
        for (int gx = 0; gx < 3; ++gx)
        {
            float2 away     = float2((float)gx, (float)gy) - fx;
            float  weight   = w[gx].x * w[gy].y;
            float2 momentum = weight * (vel + float2(dot(affine.xy, away), dot(affine.zw, away)));
            uint   node     = pc.grid + (uint)((base.y + gy) * (int)pc.n + base.x + gx) * kNodeSize;
            uint   before;
            muSceneRW.InterlockedAdd(node, (uint)(int)floor(weight * kMassScale + 0.5), before);
            muSceneRW.InterlockedAdd(node + 4u, (uint)(int)floor(momentum.x * kMomentumScale + 0.5), before);
            muSceneRW.InterlockedAdd(node + 8u, (uint)(int)floor(momentum.y * kMomentumScale + 0.5), before);
        }
}

// A node's velocity after gravity and the walls. A wall stops what moves into it and lets go of what leaves.
float2 nodeVelocity(int2 node)
{
    int3 sums = muSceneRW.Load<int3>(pc.grid + (uint)(node.y * (int)pc.n + node.x) * kNodeSize);
    if (sums.x <= 0) return float2(0.0, 0.0);
    float2 v = float2((float)sums.y, (float)sums.z) / kMomentumScale / ((float)sums.x / kMassScale);
    v.y -= pc.dt * pc.gravity;
    int low = (int)pc.walls, high = (int)pc.n - 1 - (int)pc.walls;   // nodes run from 0 to n - 1
    if (node.x < low && v.x < 0.0) v.x = 0.0;
    if (node.x > high && v.x > 0.0) v.x = 0.0;
    if (node.y < low && v.y < 0.0) v.y = 0.0;
    if (node.y > high && v.y > 0.0) v.y = 0.0;
    return v;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void compG2P(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= pc.count) return;
    uint   at  = pc.particles + id.x * kParticleSize;
    float2 pos = muSceneRW.Load<float2>(at);
    float4 f   = muSceneRW.Load<float4>(at + 32u);
    uint   material = muSceneRW.Load<uint>(at + 48u);

    float2 w[3], fx;
    int2   base = stencil(pos, w, fx);
    float2 vel = float2(0.0, 0.0);
    float4 c   = float4(0.0, 0.0, 0.0, 0.0);
    for (int gy = 0; gy < 3; ++gy)
        for (int gx = 0; gx < 3; ++gx)
        {
            float2 away   = float2((float)gx, (float)gy) - fx;
            float  weight = w[gx].x * w[gy].y;
            float2 v      = nodeVelocity(base + int2(gx, gy));
            vel += weight * v;
            c   += 4.0 * weight * float4(v.x * away, v.y * away);
        }
    pos = clamp(pos + pc.dt * vel, 1.0, (float)pc.n - 2.0);   // keeps the 3 x 3 nodes inside the grid whatever happens

    float4 grown = float4(1.0 + pc.dt * c.x, pc.dt * c.y, pc.dt * c.z, 1.0 + pc.dt * c.w);
    f = float4(grown.x * f.x + grown.y * f.z, grown.x * f.y + grown.y * f.w,
               grown.z * f.x + grown.w * f.z, grown.z * f.y + grown.w * f.w);
    if (material == 0u)
    {
        float side = sqrt(max(f.x * f.w - f.y * f.z, 0.01));
        f = float4(side, 0.0, 0.0, side);
    }
    muSceneRW.Store<float2>(at, pos);
    muSceneRW.Store<float2>(at + 8u, vel);
    muSceneRW.Store<float4>(at + 16u, c);
    muSceneRW.Store<float4>(at + 32u, f);
}
