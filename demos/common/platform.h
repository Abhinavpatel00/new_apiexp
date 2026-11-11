/* platform.h — window + WSI surface + the GLFW backend for mu_input.h.
 *
 * This is the only file in the demos that knows GLFW exists. Everything else
 * (gameplay, recording, tools) talks to MuInputActions and MuInputState, so a
 * second backend (SDL3, Win32, a replay file) is a new .c file and nothing else.
 *
 *     Platform *app = platform_create(1280, 720, "Galaxy");
 *     platform_bind_control_name(platform_map(app), ACT_QUIT, "Escape");
 *     while (!platform_should_close(app)) {
 *         platform_poll(app);                       // events -> state -> actions
 *         const MuInputActions *in = platform_actions(app);
 *         if (mu_input_action_pressed(in, ACT_QUIT)) break;
 *         ...
 *     }
 *     platform_destroy(app);
 *
 * Binding names are *positions* on a US layout ("W") resolved to platform
 * scancodes, and labels are read back through the user's own layout, so an
 * AZERTY player gets the same physical cross and sees "Z" on screen.
 *
 * SIMPLE API (no map needed):
 *     if (platform_key_pressed(app, MU_KEY_ESCAPE)) break;
 *     if (platform_mouse_down(app, MU_MOUSE_RIGHT)) drag();
 * The backend maps every MuKey to its scancode at startup, so the quick
 * API works with zero setup. Equivalent to
 * mu_input_key_down(platform_state(app), MU_KEY_W).
 */

#ifndef MU_PLATFORM_H
#define MU_PLATFORM_H

#include "mu_input.h"
#include "vk.h" /* VkInstance / VkSurfaceKHR for WSI attach */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct Platform Platform;

/* Vulkan-ready window (no client graphics API) with an input snapshot attached.
 * NULL on failure; init/teardown are process-global, so one window per process. */
Platform *platform_create(uint32_t width, uint32_t height, const char *title);
void platform_destroy(Platform *platform);

/* Caller-owned variant (AGENTS.md §9.7: no hidden allocation). The caller
 * provides the storage; platform_init fills it, platform_shutdown tears it
 * down. platform_create/destroy are thin calloc/free wrappers over these. */
bool platform_init(Platform *platform, uint32_t width, uint32_t height, const char *title);
void platform_shutdown(Platform *platform);

bool platform_should_close(const Platform *platform);
void platform_request_close(Platform *platform);

/* Pump events, refresh the input snapshot, resolve actions. Once per frame.
 * Split form (for fixed-step sims that sample N ticks per render frame):
 *   platform_poll_events(p); platform_frame_begin(p); platform_resolve(p);
 * platform_poll() is exactly those three, in that order. */
void platform_poll(Platform *platform);
void platform_poll_events(Platform *platform);
void platform_frame_begin(Platform *platform);
void platform_resolve(Platform *platform);

const MuInputState   *platform_state(const Platform *platform);
MuInputState           *platform_state_mutable(Platform *platform); /* for clear_pressed_frame in fixed-step sims */
const MuInputActions *platform_actions(const Platform *platform);
MuInputMap           *platform_map(Platform *platform);
uint64_t              platform_time_ns(const Platform *platform);

/* Simple quick-check API (no MuInputMap needed). */
bool platform_key_down(const Platform *platform, MuKey key);
bool platform_key_pressed(const Platform *platform, MuKey key);
bool platform_key_released(const Platform *platform, MuKey key);
bool platform_mouse_down(const Platform *platform, MuMouseButton button);
bool platform_mouse_pressed(const Platform *platform, MuMouseButton button);
bool platform_mouse_released(const Platform *platform, MuMouseButton button);

/* WSI surface for this window, to hand to set_surface(). */
bool platform_create_surface(const Platform *platform, VkInstance instance,
                            VkSurfaceKHR *out_surface);

/* Control slots by name. "W", "Escape", "Space", "LeftShift", "Up", "F5",
 * "MouseLeft", "MouseRight", "MouseMiddle", ... Returns PLATFORM_SLOT_NONE for
 * an unknown name. Slots are platform scancodes, i.e. physical positions. */
#define PLATFORM_SLOT_NONE 0xFFFFFFFFu
uint32_t platform_slot_from_name(const char *name);

/* Physical slot for a logical MuKey (scancode), or PLATFORM_SLOT_NONE. */
uint32_t platform_slot_from_key(MuKey key);

/* Layout-aware label for a slot ("Z" on AZERTY for the key bound as "W").
 * Writes into scratch when the name needs formatting. Never NULL. */
const char *platform_slot_label(uint32_t slot, char *scratch, size_t capacity);

bool platform_bind_control_name(MuInputMap *map, uint32_t action, const char *control_name);
/* Bind by logical key (uses the state's MuKey -> slot map). */
bool platform_bind_key(MuInputMap *map, uint32_t action, const MuInputState *state, MuKey key);

#endif /* MU_PLATFORM_H */
