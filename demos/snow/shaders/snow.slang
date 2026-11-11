// snow.slang — snow as particles that talk through a grid.
//
// "A material point method for snow simulation" (Stomakhin, Schroeder, Chai,
// Teran, Selle, SIGGRAPH 2013), in three dimensions, with the explicit time
// step (beta = 0 in the paper's equation 9). Lengths are grid cells. A
// particle carries position, velocity, its elastic deformation F_E and the
// determinant J_P of its plastic deformation. One time step is three passes:
//
//   compClear   empty the grid
//   compP2G     every particle adds mass, momentum and dt x force to the
//               4 x 4 x 4 nodes round it, with cubic B-spline weights.
//               force on node i = -V (dPsi/dF_E) F_E^T grad w_i
//   compG2P     a node's velocity is momentum / mass; forces, gravity and
//               collisions change it. Every particle takes back a blend of
//               the new velocity (PIC) and its own plus the change (FLIP),
//               and the velocity gradient, which deforms it:
//               F_E <- (1 + dt grad v) F_E, then the singular values of F_E
//               are kept inside [1 - theta_c, 1 + theta_s]. What is cut off
//               is permanent: it multiplies J_P.
//
// Energy density: Psi = mu |F_E - R_E|^2 + lambda / 2 (det F_E - 1)^2, with
// R_E the rotation nearest to F_E, and mu, lambda times exp(xi (1 - J_P)):
// packed snow is stiffer, torn snow weaker.
//
// The grid holds integers and the adds are atomic, as in demos/mpm: mass in
// units of 2^-24 particle, momentum 2^-14 and dt x force 2^-18
// particle-cells per second.

[[vk::binding(3, 0)]] RWByteAddressBuffer muSceneRW;

struct Push
{
    uint   particles, grid, count, n;     // byte offsets, particle count, nodes along a side
    float  dt, gravity, mu, lambda;
    float  hardening, compress, stretch, flip;
    float  friction, walls;
    uint   sticky;                        // side walls and ceiling hold whatever touches them
    float  pad0;
    float4 ball;                          // a ball that snow cannot enter: centre, radius (0: none)
    float4 ball_velocity;
};
[[vk::push_constant]] ConstantBuffer<Push> pc;

static const uint  kParticleSize  = 80u;  // pos 0, volume 12, vel 16, jp 28, F_E rows 32, 48, 64
static const uint  kNodeSize      = 32u;  // mass 0, momentum 4, dt x force 16
static const float kMassScale     = 16777216.0;
static const float kMomentumScale = 16384.0;
static const float kImpulseScale  = 262144.0;

[shader("compute")]
[numthreads(64, 1, 1)]
void compClear(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= pc.n * pc.n * pc.n) return;
    muSceneRW.Store<uint4>(pc.grid + id.x * kNodeSize, uint4(0u, 0u, 0u, 0u));
    muSceneRW.Store<uint4>(pc.grid + id.x * kNodeSize + 16u, uint4(0u, 0u, 0u, 0u));
}

// The lowest of the 4 x 4 x 4 nodes round a particle; the cubic B-spline and its slope along each axis.
int3 stencil(float3 pos, out float3 w[4], out float3 dw[4])
{
    float3 cell = floor(pos), t = pos - cell, u = 1.0 - t;
    w[0]  = u * u * u / 6.0;
    w[1]  = 0.5 * t * t * t - t * t + 2.0 / 3.0;
    w[2]  = 0.5 * u * u * u - u * u + 2.0 / 3.0;
    w[3]  = t * t * t / 6.0;
    dw[0] = -0.5 * u * u;
    dw[1] = 1.5 * t * t - 2.0 * t;
    dw[2] = -1.5 * u * u + 2.0 * u;
    dw[3] = 0.5 * t * t;
    return (int3)cell - 1;
}

// c = v diag(lam) v^T for a symmetric c, by Jacobi rotations.
void eigen(float3x3 c, out float3x3 v, out float3 lam)
{
    v = float3x3(1, 0, 0, 0, 1, 0, 0, 0, 1);
    for (int sweep = 0; sweep < 5; ++sweep)
        for (int pair = 0; pair < 3; ++pair)
        {
            int i = pair == 2 ? 1 : 0, j = pair == 0 ? 1 : 2;
            if (c[i][j] == 0.0) continue;
            float theta = (c[j][j] - c[i][i]) / (2.0 * c[i][j]);
            float t  = (theta < 0.0 ? -1.0 : 1.0) / (abs(theta) + sqrt(theta * theta + 1.0));
            float cs = 1.0 / sqrt(t * t + 1.0), sn = t * cs;
            for (int k = 0; k < 3; ++k) { float a = c[k][i], b = c[k][j]; c[k][i] = cs * a - sn * b; c[k][j] = sn * a + cs * b; }
            for (int k = 0; k < 3; ++k) { float a = c[i][k], b = c[j][k]; c[i][k] = cs * a - sn * b; c[j][k] = sn * a + cs * b; }
            for (int k = 0; k < 3; ++k) { float a = v[k][i], b = v[k][j]; v[k][i] = cs * a - sn * b; v[k][j] = sn * a + cs * b; }
        }
    lam = float3(c[0][0], c[1][1], c[2][2]);
}

// v diag(d) v^T
float3x3 spectral(float3x3 v, float3 d)
{
    float3x3 scaled = float3x3(v[0] * d, v[1] * d, v[2] * d);
    return mul(scaled, transpose(v));
}

// The paper's collision rule for one surface with unit normal n. Moving away: nothing. Pressed
// hard enough for friction to hold: it sticks. Else it slides, and friction slows it.
float3 contact(float3 v, float3 n)
{
    float vn = dot(v, n);
    if (vn >= 0.0) return v;
    float3 vt  = v - vn * n;
    float  len = length(vt);
    if (len <= -pc.friction * vn) return float3(0.0, 0.0, 0.0);
    return vt + pc.friction * vn * vt / len;
}

// The six walls, then the ball; for a node or a particle at x. The ball may move: its rule is
// applied to the velocity relative to it.
float3 collide(float3 x, float3 v)
{
    float low = pc.walls, high = (float)pc.n - 1.0 - pc.walls;
    bool  hold = pc.sticky != 0u;
    if (x.x < low)  v = hold ? float3(0, 0, 0) : contact(v, float3(1, 0, 0));
    if (x.x > high) v = hold ? float3(0, 0, 0) : contact(v, float3(-1, 0, 0));
    if (x.y < low)  v = contact(v, float3(0, 1, 0));
    if (x.y > high) v = hold ? float3(0, 0, 0) : contact(v, float3(0, -1, 0));
    if (x.z < low)  v = hold ? float3(0, 0, 0) : contact(v, float3(0, 0, 1));
    if (x.z > high) v = hold ? float3(0, 0, 0) : contact(v, float3(0, 0, -1));
    float3 d   = x - pc.ball.xyz;
    float  len = length(d);
    if (len < pc.ball.w && len > 0.0) v = pc.ball_velocity.xyz + contact(v - pc.ball_velocity.xyz, d / len);
    return v;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void compP2G(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= pc.count) return;
    uint     at  = pc.particles + id.x * kParticleSize;
    float4   pos = muSceneRW.Load<float4>(at);            // w: volume
    float4   vel = muSceneRW.Load<float4>(at + 16u);      // w: J_P
    float3x3 f   = float3x3(muSceneRW.Load<float3>(at + 32u), muSceneRW.Load<float3>(at + 48u), muSceneRW.Load<float3>(at + 64u));

    // R_E = F_E (F_E^T F_E)^-1/2
    float3x3 v;
    float3   lam;
    eigen(mul(transpose(f), f), v, lam);
    float3x3 r = mul(f, spectral(v, 1.0 / sqrt(max(lam, 1e-12))));
    float    j = determinant(f);
    float    harden = clamp(exp(pc.hardening * (1.0 - vel.w)), 0.1, 5.0);
    // (dPsi/dF_E) F_E^T = 2 mu (F_E - R_E) F_E^T + lambda (J - 1) J 1
    float3x3 tau = harden * (2.0 * pc.mu * mul(f - r, transpose(f)) + pc.lambda * (j - 1.0) * j * float3x3(1, 0, 0, 0, 1, 0, 0, 0, 1));

    float3 w[4], dw[4];
    int3   base = stencil(pos.xyz, w, dw);
    for (int gz = 0; gz < 4; ++gz)
        for (int gy = 0; gy < 4; ++gy)
            for (int gx = 0; gx < 4; ++gx)
            {
                float  weight  = w[gx].x * w[gy].y * w[gz].z;
                float3 slope   = float3(dw[gx].x * w[gy].y * w[gz].z, w[gx].x * dw[gy].y * w[gz].z, w[gx].x * w[gy].y * dw[gz].z);
                float3 impulse = -pc.dt * pos.w * mul(tau, slope);
                int3   m       = (int3)floor(weight * vel.xyz * kMomentumScale + 0.5);
                int3   i       = (int3)floor(impulse * kImpulseScale + 0.5);
                uint   node    = pc.grid + (uint)(((base.z + gz) * (int)pc.n + base.y + gy) * (int)pc.n + base.x + gx) * kNodeSize;
                uint   before;
                muSceneRW.InterlockedAdd(node, (uint)(int)floor(weight * kMassScale + 0.5), before);
                muSceneRW.InterlockedAdd(node + 4u, (uint)m.x, before);
                muSceneRW.InterlockedAdd(node + 8u, (uint)m.y, before);
                muSceneRW.InterlockedAdd(node + 12u, (uint)m.z, before);
                muSceneRW.InterlockedAdd(node + 16u, (uint)i.x, before);
                muSceneRW.InterlockedAdd(node + 20u, (uint)i.y, before);
                muSceneRW.InterlockedAdd(node + 24u, (uint)i.z, before);
            }
}

[shader("compute")]
[numthreads(64, 1, 1)]
void compG2P(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= pc.count) return;
    uint     at  = pc.particles + id.x * kParticleSize;
    float4   pos = muSceneRW.Load<float4>(at);
    float4   vel = muSceneRW.Load<float4>(at + 16u);
    float3x3 f   = float3x3(muSceneRW.Load<float3>(at + 32u), muSceneRW.Load<float3>(at + 48u), muSceneRW.Load<float3>(at + 64u));

    float3 w[4], dw[4];
    int3   base = stencil(pos.xyz, w, dw);
    float3 pic = float3(0, 0, 0), change = float3(0, 0, 0);
    float3x3 grad = float3x3(0, 0, 0, 0, 0, 0, 0, 0, 0);
    for (int gz = 0; gz < 4; ++gz)
        for (int gy = 0; gy < 4; ++gy)
            for (int gx = 0; gx < 4; ++gx)
            {
                int3 at_node = base + int3(gx, gy, gz);
                uint node    = pc.grid + (uint)((at_node.z * (int)pc.n + at_node.y) * (int)pc.n + at_node.x) * kNodeSize;
                int4 a = muSceneRW.Load<int4>(node);              // mass, momentum
                int3 b = muSceneRW.Load<int3>(node + 16u);        // dt x force
                if (a.x <= 0) continue;
                // The node's velocity before, and after forces, gravity and collisions.
                float  mass   = (float)a.x / kMassScale;
                float3 before = float3((float)a.y, (float)a.z, (float)a.w) / kMomentumScale / mass;
                float3 after  = before + float3((float)b.x, (float)b.y, (float)b.z) / kImpulseScale / mass;
                after.y -= pc.dt * pc.gravity;
                after = collide((float3)at_node, after);

                float  weight = w[gx].x * w[gy].y * w[gz].z;
                float3 slope  = float3(dw[gx].x * w[gy].y * w[gz].z, w[gx].x * dw[gy].y * w[gz].z, w[gx].x * w[gy].y * dw[gz].z);
                pic    += weight * after;
                change += weight * (after - before);
                grad   += float3x3(after.x * slope, after.y * slope, after.z * slope);
            }

    // F_E <- (1 + dt grad v) F_E, singular values clamped; what is cut off goes to J_P.
    f += pc.dt * mul(grad, f);
    float3x3 v;
    float3   lam;
    eigen(mul(transpose(f), f), v, lam);
    float3 sigma = sqrt(max(lam, 1e-12));
    float3 kept  = clamp(sigma, 1.0 - pc.compress, 1.0 + pc.stretch);
    f = mul(f, spectral(v, kept / sigma));
    float jp = min(vel.w * sigma.x * sigma.y * sigma.z / (kept.x * kept.y * kept.z), 1e6);

    float3 velocity = (1.0 - pc.flip) * pic + pc.flip * (vel.xyz + change);
    velocity = collide(pos.xyz, velocity);
    // The clamp keeps the 4 x 4 x 4 nodes inside the grid whatever happens.
    float3 moved = clamp(pos.xyz + pc.dt * velocity, 1.0, (float)pc.n - 2.001);

    muSceneRW.Store<float4>(at, float4(moved, pos.w));
    muSceneRW.Store<float4>(at + 16u, float4(velocity, jp));
    muSceneRW.Store<float3>(at + 32u, f[0]);
    muSceneRW.Store<float3>(at + 48u, f[1]);
    muSceneRW.Store<float3>(at + 64u, f[2]);
}
