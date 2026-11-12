/* world.c — terrain, weather cells, airframe.
 *
 * fs_terrain_height() is the CPU mirror of fsTerrainHeight() in
 * shaders/fs_common.slang, term for term, in the same order, at the same
 * frequencies. It calls mu_shader_noise.h, which mirrors the Slang PCG3D noise
 * stack; the two are compared in --check with a tolerance rather than equality,
 * because Slang's lerp() is allowed to lower to a+(b-a)*t and differ by a ULP.
 *
 * Getting this right is worth the duplication. The alternative is to ask the GPU
 * for the terrain height under an aircraft, which means either a readback — at
 * least one frame late, so the aircraft has already flown through the hill — or
 * a shadow copy of the height field on the CPU, which is a megabyte of memory
 * that has to be kept in step with a shader. One function called twice per row
 * per tick is cheaper than both.
 */
#include "world.h"

#include "mu_easing.h"
#include "mu_math.h"
#include "mu_pcg.h"
#include "mu_shader_noise.h"

#include <math.h>
#include <string.h>

/* ---- terrain -------------------------------------------------------------- */

float fs_terrain_height(float x, float z)
{
    const float px    = x * (1.0f / 60000.0f);
    const float pz    = z * (1.0f / 60000.0f);
    const float cont  = mu_fbm4(px, pz, 2.0f, 0.5f);
    const float mask  = mu_clamp(cont * 2.1f + 0.28f, 0.0f, 1.0f);
    const float ridge = mu_ridged4(px * 2.7f + 13.1f, pz * 2.7f + 13.1f, 2.0f, 0.5f);
    float h = cont * 240.0f + mask * ridge * 2250.0f;

    h += mu_fbm4(x * (1.0f / 3000.0f) + 41.3f, z * (1.0f / 3000.0f) + 41.3f, 2.0f, 0.52f) * 105.0f;
    h += mu_fbm4(x * (1.0f / 400.0f) + 7.7f,  z * (1.0f / 400.0f) + 7.7f,  2.0f, 0.55f) * 10.0f;

    /* Ocean floor: clamp the basin so deep water does not run away to -inf and
     * so the shoreline stays a single crossing of y = 0. */
    return fmaxf(h, -420.0f);
}

void fs_terrain_normal(float x, float z, float step, float out[3])
{
    const float hL = fs_terrain_height(x - step, z);
    const float hR = fs_terrain_height(x + step, z);
    const float hD = fs_terrain_height(x, z - step);
    const float hU = fs_terrain_height(x, z + step);
    const float n[3] = {hL - hR, 2.0f * step, hD - hU};
    mu_vec3_normalize(out, n);
}

float fs_ground_height(float x, float z)
{
    return fmaxf(fs_terrain_height(x, z), FS_SEA_LEVEL);
}

void fs_world_ground(const Sim *s, GroundHeights *out)
{
    /* The player first because it is the one row whose ground height decides
     * whether the run ends. Same transform, two tables, and the row count that
     * goes with each. */
    out->player = fs_ground_height(s->player.pos[0], s->player.pos[2]);
    for (uint32_t i = 0u; i < s->traffic_count; ++i)
        out->traffic[i] = fs_ground_height(s->traffic[i].pos[0], s->traffic[i].pos[2]);
}

/* ---- weather -------------------------------------------------------------- */

void fs_weather_reset(Weather *w, const float *centre, float coverage, uint64_t seed)
{
    memset(w, 0, sizeof *w);
    w->centre    = centre;
    w->coverage  = mu_clamp(coverage, 0.0f, 1.0f);
    w->dirty     = true;
    w->twist     = 0.30f;
    w->type_bias = 0.15f;
    w->precip    = 0.10f;
    /* The map is scrolled in map units, so metres per second becomes tiles per
     * second here, once, rather than in the shader at every dispatch. */
    w->wind_u = 9.0f / FS_WEATHER_TILE;
    w->wind_v = -3.5f / FS_WEATHER_TILE;

    /* Cells are seeded around the aircraft, in a disc the wind will carry
     * across the field. The seed decides which, so a run is reproducible. */
    mu_pcg32 rng;
    mu_pcg32_init(&rng, seed ? seed : 7u, 0xd1b54a32d192ed03ULL);
    const uint32_t want = 6u + (uint32_t)(w->coverage * 14.0f);
    w->count = want > FS_MAX_CELLS ? FS_MAX_CELLS : want;
    for (uint32_t i = 0u; i < w->count; ++i) {
        const float u0 = (float)mu_pcg32_next_u32(&rng) * (1.0f / 4294967296.0f);
        const float u1 = (float)mu_pcg32_next_u32(&rng) * (1.0f / 4294967296.0f);
        const float u2 = (float)mu_pcg32_next_u32(&rng) * (1.0f / 4294967296.0f);
        const float u3 = (float)mu_pcg32_next_u32(&rng) * (1.0f / 4294967296.0f);
        const float ang = u0 * 6.28318531f;
        const float rad = 3000.0f + u1 * 38000.0f;
        w->cells[i].x         = centre[0] + sinf(ang) * rad;
        w->cells[i].z         = centre[2] - cosf(ang) * rad;
        w->cells[i].radius    = 2200.0f + u2 * 7800.0f;
        w->cells[i].intensity = 0.35f + u3 * 0.65f;
    }
}

void fs_weather_step(Weather *w, float dt, const float wind[3])
{
    w->time += dt;

    /* Advect the cells by the same wind that scrolls the map. If the two
     * disagree, the cells drift away from the weather they are supposed to be
     * making, which reads as clouds moving one way and rain falling another. */
    w->wind_u = wind[0] / FS_WEATHER_TILE;
    w->wind_v = wind[2] / FS_WEATHER_TILE;
    for (uint32_t i = 0u; i < w->count; ++i) {
        w->cells[i].x += wind[0] * dt;
        w->cells[i].z += wind[2] * dt;
        /* Cells develop. A field of frozen discs looks painted on; a slow radius
         * change costs one multiply per cell and reads as weather forming. The
         * phase is per-cell and the rate is slow, so two cells side by side do
         * not pulse together. */
        const float develop = 1.0f + 0.06f * sinf(w->time * 0.07f + (float)i * 2.1f);
        w->cells[i].radius = fmaxf(900.0f, w->cells[i].radius * (1.0f + 0.02f * develop * dt));
    }

    /* Retire a cell that has drifted out of reach. Swap-remove keeps the table
     * dense, and "still in the weather" is membership in [0, count) rather than
     * a flag on the row. The index does not advance, because the row that moved
     * into the hole has not been tested. */
    const float reach = FS_WEATHER_TILE * 0.55f;
    for (uint32_t i = 0u; i < w->count;) {
        const float dx = w->cells[i].x - w->centre[0];
        const float dz = w->cells[i].z - w->centre[2];
        if (dx * dx + dz * dz < reach * reach) { ++i; continue; }
        w->cells[i] = w->cells[--w->count];
    }

    /* The population is a function of coverage, so clearing the sky actually
     * removes cells instead of merely dimming the ones that are there. New cells
     * are spawned upwind and far out, so they arrive rather than appearing
     * overhead. The golden-angle spacing keeps consecutive spawns from stacking
     * on one bearing without a random number per cell. */
    const uint32_t want = 4u + (uint32_t)(w->coverage * 18.0f);
    while (w->count < want && w->count < FS_MAX_CELLS) {
        const float  i_f  = (float)w->count;
        const float  ang  = i_f * 2.39996323f + w->time * 0.05f;
        const float  rad  = FS_WEATHER_TILE * 0.48f;
        WeatherCell *c    = &w->cells[w->count];
        c->x = w->centre[0] + sinf(ang) * rad - wind[0] * 120.0f;
        c->z = w->centre[2] - cosf(ang) * rad - wind[2] * 120.0f;
        c->radius    = 2600.0f + 6400.0f * mu_ease_smoothstep(fmodf(i_f * 0.37f + w->time * 0.02f, 1.0f));
        c->intensity = 0.40f + 0.55f * mu_ease_smoothstep(fmodf(i_f * 0.61f + w->time * 0.03f, 1.0f));
        w->count++;
    }

    /* The bake runs on a throttle. Weather changes over minutes; re-baking a
     * 512 x 512 map every frame would spend the dispatch on a field that has not
     * moved a texel. */
    w->rebake_in -= dt;
    if (w->rebake_in <= 0.0f || w->dirty) {
        w->rebake_in = 0.35f;
        w->dirty     = false;
        w->rebakes++;
    }
}

/* ---- the airframe --------------------------------------------------------- */

/* One quad as two non-indexed triangles, CCW seen from outside. Unindexed
 * because the vertex shader fetches by SV_VertexID (vk.h §17); an index buffer
 * would be a second upload and a second bind to save nothing at this vertex
 * count.
 *
 * The winding is derived from the supplied normal rather than trusted from the
 * order the corners happen to be listed in. Thirty quads authored by hand is
 * thirty chances to walk a perimeter the wrong way round, and a backwards
 * triangle is invisible until the aircraft is lit from the side that should be
 * dark — which in flight is most of the time. This runs once at startup, so the
 * cross product costs nothing. */
static uint32_t emit_quad(float *v, const float p[4][3], const float n[3])
{
    /* A fan from corner 0 tiles any convex quad, and the two triangles agree on
     * orientation, so one cross product decides both. */
    static const uint8_t forward[6] = {0u, 1u, 2u, 0u, 2u, 3u};
    static const uint8_t reversed[6] = {0u, 2u, 1u, 0u, 3u, 2u};
    const float e1[3] = {p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2]};
    const float e2[3] = {p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2]};
    float       c[3];
    mu_vec3_cross(c, e1, e2);
    const uint8_t *order = mu_vec3_dot(c, n) < 0.0f ? reversed : forward;

    for (int k = 0; k < 6; ++k) {
        const float *s = p[order[k]];
        float       *d = v + (size_t)k * 8u;
        d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 1.0f;
        d[4] = n[0]; d[5] = n[1]; d[6] = n[2]; d[7] = 0.0f;
    }
    return 6u;
}

/* The same quad on the other side of the aircraft: the spanwise coordinate and
 * the normal's spanwise component negated. Body +Y is right, so negating Y is
 * left for right. A reflection reverses winding, and emit_quad derives the
 * winding from the normal it is handed, so the mirrored normal is what keeps the
 * far wing facing outwards — there is no second winding rule to remember.
 *
 * Both wings and both halves of the tailplane are symmetric, and building the
 * far side by hand would be a second place for the numbers to go wrong. */
static uint32_t emit_quad_mirrored(float *v, const float p[4][3], const float n[3])
{
    float       q[4][3];
    const float mn[3] = {n[0], -n[1], n[2]};
    for (int k = 0; k < 4; ++k) { q[k][0] = p[k][0]; q[k][1] = -p[k][1]; q[k][2] = p[k][2]; }
    return emit_quad(v, q, mn);
}

uint32_t fs_airframe_build(float *out, uint32_t max_vertices)
{
    /* A low-wing single in body axes: +X forward, +Y right, +Z down, the frame
     * flight.c integrates in and the frame the attitude quaternion rotates into
     * the world. One convention end to end is the point: a mesh authored in a
     * different handedness from the physics renders an aircraft that banks the
     * wrong way, and nothing in a still frame makes that obvious.
     *
     * Every part is a slab, because at the distance traffic is seen at a slab
     * with the right silhouette and the right normal reads as an aircraft and a
     * smoothed mesh reads as a smudge. What makes it read is the normals: each
     * face gets its own, so the sun catches the wing's upper surface and misses
     * the fuselage's side. */
    float   *v = out;
    uint32_t n = 0u;
    const float up[3] = {0.0f, 0.0f, -1.0f};       /* body -Z is up            */
    const float dn[3] = {0.0f, 0.0f, 1.0f};
    const float lt[3] = {0.0f, -1.0f, 0.0f};       /* port                     */
    const float rt[3] = {0.0f, 1.0f, 0.0f};        /* starboard                */

    /* Fuselage: three tapered boxes — nose, cabin, tailcone. Each row is
     * {x at the front, x at the back, half width in Y, half height in Z}, and
     * the last one closes toward a point rather than toward another box's size,
     * which is what makes it a tail. */
    const float spine[3][4] = {
        {3.10f,  1.20f, 0.40f, 0.42f},
        {1.20f, -1.60f, 0.62f, 0.58f},
        {-1.60f, -4.30f, 0.34f, 0.30f},
    };
    for (int s = 0; s < 3; ++s) {
        const float x0 = spine[s][0], x1 = spine[s][1];
        const float w  = spine[s][2], h  = spine[s][3];
        const float w2 = s < 2 ? spine[s + 1][2] : 0.18f;
        const float h2 = s < 2 ? spine[s + 1][3] : 0.16f;
        const float p_top[4][3] = {{x0, -w, -h}, {x0, w, -h}, {x1, w2, -h2}, {x1, -w2, -h2}};
        const float p_bot[4][3] = {{x0, -w, h}, {x1, -w2, h2}, {x1, w2, h2}, {x0, w, h}};
        const float p_lft[4][3] = {{x0, -w, -h}, {x1, -w2, -h2}, {x1, -w2, h2}, {x0, -w, h}};
        const float p_rgt[4][3] = {{x0, w, -h}, {x0, w, h}, {x1, w2, h2}, {x1, w2, -h2}};
        n += emit_quad(v + (size_t)n * 8u, p_top, up);
        n += emit_quad(v + (size_t)n * 8u, p_bot, dn);
        n += emit_quad(v + (size_t)n * 8u, p_lft, lt);
        n += emit_quad(v + (size_t)n * 8u, p_rgt, rt);
    }

    /* Canopy, forward of the wing root: low at the windscreen, high at the back,
     * so its glazed surface faces up and forward. Its own normals, so it takes a
     * highlight the fuselage does not. */
    {
        const float gl[3]  = {-0.30f, 0.0f, -0.95f};
        const float p_top[4][3] = {{1.00f, -0.30f, -0.52f}, {1.00f, 0.30f, -0.52f},
                                   {-0.70f, 0.22f, -0.86f}, {-0.70f, -0.22f, -0.86f}};
        const float p_lft[4][3] = {{1.00f, -0.30f, -0.52f}, {-0.70f, -0.22f, -0.86f},
                                   {-0.70f, -0.26f, -0.34f}, {1.00f, -0.32f, -0.20f}};
        const float p_rgt[4][3] = {{1.00f, 0.30f, -0.52f}, {1.00f, 0.32f, -0.20f},
                                   {-0.70f, 0.26f, -0.34f}, {-0.70f, 0.22f, -0.86f}};
        n += emit_quad(v + (size_t)n * 8u, p_top, gl);
        n += emit_quad(v + (size_t)n * 8u, p_lft, (const float[3]){0.0f, -0.94f, -0.35f});
        n += emit_quad(v + (size_t)n * 8u, p_rgt, (const float[3]){0.0f, 0.94f, -0.35f});
    }

    /* Main wing: swept, with dihedral, and a leading edge that gets a third
     * normal. Port half authored, starboard half mirrored. */
    {
        const float root = 1.15f, tip_c = 0.72f;   /* chords, m                 */
        const float span = 6.25f, sweep = 0.45f, dihedral = 0.16f, thick = 0.09f;
        const float xr = 0.35f;                    /* root leading edge         */
        const float xt = xr - sweep;               /* tip leading edge, swept   */
        const float y0 = -0.30f;                   /* the root rib, outboard of */
                                                   /* the fuselage              */
        const float p_top[4][3] = {{xr + root, y0, -thick}, {xr, y0, -thick},
                                   {xt, -span, -thick - dihedral}, {xt + tip_c, -span, -thick - dihedral}};
        const float p_bot[4][3] = {{xr, y0, thick}, {xr + root, y0, thick},
                                   {xt + tip_c, -span, thick + dihedral}, {xt, -span, thick + dihedral}};
        const float p_le[4][3]  = {{xr, y0, -thick}, {xr, y0, thick},
                                   {xt, -span, thick + dihedral}, {xt, -span, -thick - dihedral}};
        const float le[3] = {-0.86f, -0.51f, 0.0f};
        n += emit_quad(v + (size_t)n * 8u, p_top, up);
        n += emit_quad(v + (size_t)n * 8u, p_bot, dn);
        n += emit_quad(v + (size_t)n * 8u, p_le, le);
        n += emit_quad_mirrored(v + (size_t)n * 8u, p_top, up);
        n += emit_quad_mirrored(v + (size_t)n * 8u, p_bot, dn);
        n += emit_quad_mirrored(v + (size_t)n * 8u, p_le, le);
    }

    /* Tailplane: the same slab, smaller, further back, and level — no dihedral,
     * because it is not carrying the aircraft. */
    {
        const float p_top[4][3] = {{-3.55f, -0.20f, -0.10f}, {-3.55f, 0.20f, -0.10f},
                                   {-4.05f, 2.10f, -0.12f}, {-4.30f, 2.10f, -0.12f}};
        const float p_bot[4][3] = {{-3.55f, -0.20f, 0.10f}, {-4.30f, 2.10f, 0.12f},
                                   {-4.05f, 2.10f, 0.12f}, {-3.55f, 0.20f, 0.10f}};
        n += emit_quad(v + (size_t)n * 8u, p_top, up);
        n += emit_quad(v + (size_t)n * 8u, p_bot, dn);
        n += emit_quad_mirrored(v + (size_t)n * 8u, p_top, up);
        n += emit_quad_mirrored(v + (size_t)n * 8u, p_bot, dn);
    }

    /* Fin and rudder: a swept vertical slab above the tailcone. Vertical means
     * constant Y and varying Z here, which is the axis swap that catches out a
     * mesh authored in a Y-up frame. */
    {
        const float p_lft[4][3] = {{-3.30f, -0.05f, -0.20f}, {-4.20f, -0.05f, -0.20f},
                                   {-4.55f, -0.05f, -1.55f}, {-3.95f, -0.05f, -1.55f}};
        const float p_rgt[4][3] = {{-3.30f, 0.05f, -0.20f}, {-3.95f, 0.05f, -1.55f},
                                   {-4.55f, 0.05f, -1.55f}, {-4.20f, 0.05f, -0.20f}};
        const float p_le[4][3]  = {{-3.30f, -0.05f, -0.20f}, {-3.30f, 0.05f, -0.20f},
                                   {-3.95f, 0.05f, -1.55f}, {-3.95f, -0.05f, -1.55f}};
        n += emit_quad(v + (size_t)n * 8u, p_lft, lt);
        n += emit_quad(v + (size_t)n * 8u, p_rgt, rt);
        n += emit_quad(v + (size_t)n * 8u, p_le, (const float[3]){-0.92f, 0.0f, -0.38f});
    }

    /* Navigation lights: red port, green starboard, two white beacons. They are
     * geometry and not a shader special case, so the emission carried in the
     * record's alpha is the only thing distinguishing them and the renderer has
     * one path for the whole aircraft. */
    {
        const float p_red[4][3] = {{-0.20f, -6.25f, -0.24f}, {-0.20f, -6.18f, -0.16f},
                                   {-0.52f, -6.18f, -0.16f}, {-0.52f, -6.25f, -0.24f}};
        const float p_grn[4][3] = {{-0.20f, 6.25f, -0.24f}, {-0.52f, 6.25f, -0.24f},
                                   {-0.52f, 6.18f, -0.16f}, {-0.20f, 6.18f, -0.16f}};
        const float p_beacon[4][3] = {{-1.55f, -0.09f, -0.88f}, {-1.55f, 0.09f, -0.88f},
                                      {-1.75f, 0.09f, -0.88f}, {-1.75f, -0.09f, -0.88f}};
        const float p_tail[4][3] = {{-4.58f, -0.08f, -1.57f}, {-4.58f, 0.08f, -1.57f},
                                    {-4.74f, 0.08f, -1.57f}, {-4.74f, -0.08f, -1.57f}};
        n += emit_quad(v + (size_t)n * 8u, p_red, up);
        n += emit_quad(v + (size_t)n * 8u, p_grn, up);
        n += emit_quad(v + (size_t)n * 8u, p_beacon, up);
        n += emit_quad(v + (size_t)n * 8u, p_tail, up);
    }

    return n > max_vertices ? max_vertices : n;
}
