/* world.h — the ground, the weather, and the airframe.
 *
 * The ground is the reason mu_shader_noise.h exists. fs_terrain_height() is the
 * same function as fsTerrainHeight() in shaders/fs_common.slang, evaluated here
 * so the simulator can answer the questions a shader cannot: what is under this
 * aircraft, has it just flown into a hill, where may traffic be spawned without
 * starting inside a mountain. --check renders the terrain and compares the
 * picture against this function, because a disagreement between the two is the
 * one bug in this file that is invisible in a screenshot and fatal in play.
 *
 * The weather is split the way its cost is split. The map is a static field,
 * baked on the GPU and scrolled by the wind at march time, so weather moves
 * every frame for free. The cells on top of it are the dynamic part: they are
 * advected, spawned and retired here on the CPU, and the map is re-baked at a
 * few hertz rather than every frame, because nothing a player can see changes
 * faster than that.
 */
#ifndef FS_WORLD_H
#define FS_WORLD_H

#include "flight.h"

#include <stdbool.h>
#include <stdint.h>

#define FS_MAX_CELLS   24u
#define FS_SEA_LEVEL   0.0f
#define FS_WEATHER_TILE 49152.0f   /* m; must match shaders/fs_common.slang    */

/* The terrain height in metres above sea level, at world (x, z). */
float fs_terrain_height(float x, float z);

/* The terrain normal by central difference, the same way the vertex shader
 * does it, at the same step. Returns the unit normal with +Y up. */
void fs_terrain_normal(float x, float z, float step, float out[3]);

/* The height the aircraft can collide with: the terrain, or the sea where the
 * terrain is below it. Flying into the ocean is a crash, not a landing, and
 * without this the water would be a hole in the world. */
float fs_ground_height(float x, float z);

/* Fill one ground height per row, ready for fs_sim_step(). This is a separate
 * transform from the physics because it is the only thing in the frame that
 * needs the terrain, and the physics is the only thing that needs the answer. */
void fs_world_ground(const Sim *s, GroundHeights *out);

/* ---- weather -------------------------------------------------------------- */

/* One cell, 16 bytes, and exactly the record the bake shader loads. The weather
 * map's own texels are not stored here: they are a function of the seed and of
 * this table, which is the difference between regenerating weather and
 * persisting it. */
typedef struct WeatherCell {
    float x, z;          /* world position                                     */
    float radius;        /* m                                                  */
    float intensity;     /* 0 .. 1                                             */
} WeatherCell;
_Static_assert(sizeof(WeatherCell) == 16, "bake_weather.slang loads this as a float4");

typedef struct Weather {
    WeatherCell cells[FS_MAX_CELLS];
    uint32_t    count;
    float       coverage;      /* 0 clear .. 1 overcast, the global slider     */
    float       type_bias;     /* -1 stratus .. +1 towering                    */
    float       precip;        /* 0 .. 1                                       */
    float       twist;         /* how much the detail is swirled per texel     */
    float       wind_u, wind_v;/* map units per second, the scroll rate        */
    float       time;
    const float *centre;       /* the aircraft the weather is built around     */
    /* The map is re-baked on a throttle, not every frame: a 512 x 512 bake with
     * a loop over the cells is a millisecond the frame does not have to spend
     * sixty times a second on a field that changes over minutes. */
    float       rebake_in;
    bool        dirty;
    uint32_t    rebakes;
} Weather;

void fs_weather_reset(Weather *w, const float *centre, float coverage, uint64_t seed);
void fs_weather_step(Weather *w, float dt, const float wind[3]);

/* ---- the airframe --------------------------------------------------------- */

/* One mesh, instanced for the player and every traffic aircraft. Built at
 * startup into `out` as 32-byte vertices (position xyzw, normal xyzw) in body
 * axes: +X forward, +Y right, +Z down, the frame the attitude quaternion
 * rotates into the world. Returns the vertex count. */
uint32_t fs_airframe_build(float *out, uint32_t max_vertices);

#endif /* FS_WORLD_H */
