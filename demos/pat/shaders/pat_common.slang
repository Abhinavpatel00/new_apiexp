// pat_common.slang — the signed distance of Points as Tori (Feng, Gkioulekas,
// Crane 2026), in float. method.c is the same in double; equation numbers are
// the paper's.
//
// #include after defining PAT_SCENE, the byte buffer the points are in.
//
//   phi(x) = sum_i g_i(x) w_i / sum_i w_i,   w_i = exp(-lambda (|x - p_i| - shift))      (25)
//
// over the 32 points nearest to x, with shift half the distance to the
// farthest of them and lambda = 64 / shift (26); g_i is the signed distance to
// the torus fitted at p_i (23, 24).

static const uint  NEAREST    = 32u;
static const float PAT_C      = 64.0;
static const uint  POINT_SIZE = 80u;

struct Cloud
{
    uint   points, cells, count, grid;   // byte offsets of the points and of the grid's prefix sums; grid: cells along an edge
    float3 origin;
    float  cell;
};

// One point and its torus; PatPoint in method.h.
struct PatPoint
{
    float3 pos;    float kmin;
    float3 normal; float kmax;
    float3 anchor; float sign;
    float3 axis;   float side;
    float3 spoke;  float pad;
};

// (23) with the sign of (24), written around the point the torus passes
// through so that a cylinder (kmin = 0) or a plane (kmax = 0) is an ordinary
// input; the derivation is with pat_torus_distance() in method.c.
float torusDistance(PatPoint p, float3 x, out float3 gradient)
{
    float3 y      = x - p.anchor;
    float  a      = dot(y, p.axis);
    float3 f      = y - a * p.axis;
    float  b      = dot(f, p.spoke), ff = dot(f, f);
    float  radial = sqrt(max(1.0 + 2.0 * p.kmin * b + p.kmin * p.kmin * ff, 0.0));
    float  delta  = (2.0 * b + p.kmin * ff) / (radial + 1.0);
    float  along  = p.kmax * delta + p.side;
    float  tube   = sqrt(along * along + p.kmax * p.kmax * a * a);
    gradient = p.sign * (along * (p.spoke + p.kmin * f) / max(radial, 1e-20) + p.kmax * a * p.axis) / max(tube, 1e-20);
    return p.sign * (p.kmax * (delta * delta + a * a) + 2.0 * p.side * delta) / (tube + 1.0);
}

// The nearest points found so far, unordered; `worst` is the farthest of them.
struct Nearest
{
    uint  id[NEAREST];
    float d[NEAREST];
    uint  used, worst;
    float reach;
};

// Points first .. last - 1 (they are stored cell by cell, so a run of cells is a run of points).
void consider(inout Nearest n, uint points, uint first, uint last, float3 x)
{
    for (uint i = first; i < last; ++i)
    {
        float r = distance(x, PAT_SCENE.Load<float3>(points + i * POINT_SIZE));
        if (n.used < NEAREST)
        {
            n.id[n.used] = i;
            n.d[n.used]  = r;
            if (r > n.reach) { n.reach = r; n.worst = n.used; }
            n.used++;
        }
        else if (r < n.reach)
        {
            n.id[n.worst] = i;
            n.d[n.worst]  = r;
            n.reach = 0.0;
            for (uint j = 0u; j < NEAREST; ++j)
                if (n.d[j] > n.reach) { n.reach = n.d[j]; n.worst = j; }
        }
    }
}

// The exact 32 nearest: cubes of cells around x's cell, one shell at a time,
// until the farthest point kept is nearer than anything not yet looked at.
void findNearest(Cloud c, float3 x, out Nearest n)
{
    n.used  = 0u;
    n.worst = 0u;
    n.reach = 0.0;
    int  G    = int(c.grid);
    int3 home = clamp(int3(floor((x - c.origin) / c.cell)), int3(0), int3(G - 1));
    for (int ring = 0; ring < G; ++ring)
    {
        int3 lo = home - ring, hi = home + ring;
        int3 clo = max(lo, int3(0)), chi = min(hi, int3(G - 1));
        for (int z = clo.z; z <= chi.z; ++z)
            for (int y = clo.y; y <= chi.y; ++y)
            {
                uint row = c.cells + uint(G * (y + G * z)) * 4u;
                if (z == lo.z || z == hi.z || y == lo.y || y == hi.y)
                    // A row on one of the shell's four long faces: every cell of it.
                    consider(n, c.points, PAT_SCENE.Load<uint>(row + uint(clo.x) * 4u), PAT_SCENE.Load<uint>(row + uint(chi.x + 1) * 4u), x);
                else
                {
                    // A row through the middle: its two end cells.
                    if (lo.x >= 0)
                        consider(n, c.points, PAT_SCENE.Load<uint>(row + uint(lo.x) * 4u), PAT_SCENE.Load<uint>(row + uint(lo.x + 1) * 4u), x);
                    if (hi.x < G)
                        consider(n, c.points, PAT_SCENE.Load<uint>(row + uint(hi.x) * 4u), PAT_SCENE.Load<uint>(row + uint(hi.x + 1) * 4u), x);
                }
            }
        if (all(lo <= int3(0)) && all(hi >= int3(G - 1))) break;   // the whole grid has been read
        if (n.used == NEAREST)
        {
            // What is left lies beyond one of the cube's faces that are inside the grid.
            float3 below = x - (c.origin + float3(lo) * c.cell), above = c.origin + float3(hi + 1) * c.cell - x;
            float  bound = 1e30;
            for (int axis = 0; axis < 3; ++axis)
            {
                if (lo[axis] > 0) bound = min(bound, below[axis]);
                if (hi[axis] < G - 1) bound = min(bound, above[axis]);
            }
            if (n.reach <= bound) break;
        }
    }
}

// method 0: tori (the paper). 1: tangent planes (29, the baseline). 2: the nearest point's torus alone.
float patDistance(Cloud c, float3 x, uint method, out float3 gradient)
{
    Nearest n;
    findNearest(c, x, n);
    if (method == 2u)
    {
        uint best = 0u;
        for (uint j = 1u; j < n.used; ++j)
            if (n.d[j] < n.d[best]) best = j;
        return torusDistance(PAT_SCENE.Load<PatPoint>(c.points + n.id[best] * POINT_SIZE), x, gradient);
    }
    float  shift = 0.5 * n.reach, lambda = PAT_C / shift;
    float  total = 0.0, value = 0.0;
    float3 lean = float3(0.0), pull = float3(0.0);
    for (uint j = 0u; j < n.used; ++j)
    {
        PatPoint p = PAT_SCENE.Load<PatPoint>(c.points + n.id[j] * POINT_SIZE);
        float3   d = x - p.pos, dg;
        float    w = exp(-lambda * (n.d[j] - shift)), g;
        if (method == 1u) { g = dot(d, p.normal); dg = p.normal; }
        else g = torusDistance(p, x, dg);
        float3 toward = n.d[j] > 1e-20 ? d / n.d[j] : float3(0.0);
        total += w;
        value += w * g;
        lean  += w * (dg - lambda * toward * g);      // appendix D, lambda held fixed
        pull  += w * lambda * toward;
    }
    value /= total;
    gradient = lean / total + value * pull / total;
    return value;
}

// Two clouds and what to make of them: 0 the first alone, 1 union, 2 intersection, 3 the first minus the second.
float patScene(uint header, uint combine, float3 x, uint method, out float3 gradient)
{
    float a = patDistance(PAT_SCENE.Load<Cloud>(header), x, method, gradient);
    if (combine == 0u) return a;
    float3 other;
    float  b = patDistance(PAT_SCENE.Load<Cloud>(header + 32u), x, method, other);
    if (combine == 3u) { b = -b; other = -other; }
    if ((combine == 1u) == (b < a)) { gradient = other; return b; }   // union: the smaller; otherwise the larger
    return a;
}
