// world_common.slang — FrameData offsets and the Volume record built from it.
static const uint F_EYE = 0u, F_RIGHT = 16u, F_UP = 32u, F_FORWARD = 48u, F_SUN = 64u, F_SIZE = 80u, F_VOL = 96u, F_RELAX = 112u,
                  F_INDEX = 128u, F_OPTIONS = 144u, F_MOUSE = 160u, F_BALLS = 176u;
static const uint BALL_SIZE = 32u;   // float4 position + radius, float4 velocity + state

// The colour of ball i; odd balls are clay (they stick), even ones are
// stones (they dig).
float3 ballColor(uint i)
{
    if ((i & 1u) == 0u) return float3(0.16, 0.16, 0.18);
    float h = frac((float)i * 0.618034);
    return saturate(abs(frac(h + float3(0.0, 1.0 / 3.0, 2.0 / 3.0)) * 6.0 - 3.0) - 1.0) * 0.75 + 0.15;
}
