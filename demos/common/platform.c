/* platform.c — GLFW window, WSI surface, and the mu_input backend.
 *
 * Rules kept here so no demo has to: latches from callbacks (never polling for
 * edges), scancode control slots, layout-aware labels, focus-loss reset, wheel
 * accumulation, and gamepad polling with synthesized button edges.
 */

#include "platform.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct Platform {
    GLFWwindow   *handle;
    MuInputState  state;
    MuInputMap    map;
    MuInputActions actions;
    uint8_t       pad_prev[MU_INPUT_MAX_PADS][MU_INPUT_PAD_BUTTONS];
    uint8_t       pad_of_jid[GLFW_JOYSTICK_LAST + 1];
    uint64_t      now_ns; /* single clock reading for this poll (P0-3) */
};

/* ---- names ---------------------------------------------------------------
 * Names are positions on a US layout ("W"); slots are platform scancodes, so
 * the binding follows the *position* on any layout. Labels are read back
 * through glfwGetKeyName(), which reports the user's own layout. */

typedef struct KeyName {
    const char *name;
    int         key;
} KeyName;

static const KeyName k_named_keys[] = {
    {"Escape", GLFW_KEY_ESCAPE},       {"Space", GLFW_KEY_SPACE},
    {"Tab", GLFW_KEY_TAB},             {"Enter", GLFW_KEY_ENTER},
    {"Backspace", GLFW_KEY_BACKSPACE}, {"Delete", GLFW_KEY_DELETE},
    {"Insert", GLFW_KEY_INSERT},       {"Home", GLFW_KEY_HOME},
    {"End", GLFW_KEY_END},             {"PageUp", GLFW_KEY_PAGE_UP},
    {"PageDown", GLFW_KEY_PAGE_DOWN},  {"Left", GLFW_KEY_LEFT},
    {"Right", GLFW_KEY_RIGHT},         {"Up", GLFW_KEY_UP},
    {"Down", GLFW_KEY_DOWN},           {"LeftShift", GLFW_KEY_LEFT_SHIFT},
    {"RightShift", GLFW_KEY_RIGHT_SHIFT}, {"LeftCtrl", GLFW_KEY_LEFT_CONTROL},
    {"RightCtrl", GLFW_KEY_RIGHT_CONTROL}, {"LeftAlt", GLFW_KEY_LEFT_ALT},
    {"RightAlt", GLFW_KEY_RIGHT_ALT},  {"Minus", GLFW_KEY_MINUS},
    {"Equal", GLFW_KEY_EQUAL},         {"LeftBracket", GLFW_KEY_LEFT_BRACKET},
    {"RightBracket", GLFW_KEY_RIGHT_BRACKET}, {"Comma", GLFW_KEY_COMMA},
    {"Period", GLFW_KEY_PERIOD},       {"Slash", GLFW_KEY_SLASH},
    {"Semicolon", GLFW_KEY_SEMICOLON}, {"Apostrophe", GLFW_KEY_APOSTROPHE},
    {"Backslash", GLFW_KEY_BACKSLASH}, {"Grave", GLFW_KEY_GRAVE_ACCENT},
    {"F1", GLFW_KEY_F1},               {"F2", GLFW_KEY_F2},
    {"F3", GLFW_KEY_F3},               {"F4", GLFW_KEY_F4},
    {"F5", GLFW_KEY_F5},               {"F6", GLFW_KEY_F6},
    {"F7", GLFW_KEY_F7},               {"F8", GLFW_KEY_F8},
    {"F9", GLFW_KEY_F9},               {"F10", GLFW_KEY_F10},
    {"F11", GLFW_KEY_F11},             {"F12", GLFW_KEY_F12},
};

static const char *const k_mouse_names[] = {
    "MouseLeft", "MouseRight", "MouseMiddle", "Mouse4", "Mouse5", "Mouse6", "Mouse7", "Mouse8",
};

/* MuKey (logical, platform-free) -> GLFW key code. Indexed by MuKey.
 * Order must match the MuKey enum in mu_input.h. */
static const int k_mu_key_to_glfw[MU_KEY_COUNT] = {
    GLFW_KEY_UNKNOWN,      /* MU_KEY_UNKNOWN */
    GLFW_KEY_ESCAPE,       /* MU_KEY_ESCAPE */
    GLFW_KEY_SPACE,        /* MU_KEY_SPACE */
    GLFW_KEY_TAB,          /* MU_KEY_TAB */
    GLFW_KEY_ENTER,        /* MU_KEY_ENTER */
    GLFW_KEY_BACKSPACE,    /* MU_KEY_BACKSPACE */
    GLFW_KEY_DELETE,       /* MU_KEY_DELETE */
    GLFW_KEY_INSERT,       /* MU_KEY_INSERT */
    GLFW_KEY_HOME,         /* MU_KEY_HOME */
    GLFW_KEY_END,          /* MU_KEY_END */
    GLFW_KEY_PAGE_UP,      /* MU_KEY_PAGE_UP */
    GLFW_KEY_PAGE_DOWN,    /* MU_KEY_PAGE_DOWN */
    GLFW_KEY_LEFT,         /* MU_KEY_LEFT */
    GLFW_KEY_RIGHT,        /* MU_KEY_RIGHT */
    GLFW_KEY_UP,           /* MU_KEY_UP */
    GLFW_KEY_DOWN,         /* MU_KEY_DOWN */
    GLFW_KEY_LEFT_SHIFT,   /* MU_KEY_LEFT_SHIFT */
    GLFW_KEY_RIGHT_SHIFT,  /* MU_KEY_RIGHT_SHIFT */
    GLFW_KEY_LEFT_CONTROL, /* MU_KEY_LEFT_CTRL */
    GLFW_KEY_RIGHT_CONTROL,/* MU_KEY_RIGHT_CTRL */
    GLFW_KEY_LEFT_ALT,     /* MU_KEY_LEFT_ALT */
    GLFW_KEY_RIGHT_ALT,    /* MU_KEY_RIGHT_ALT */
    GLFW_KEY_MINUS,        /* MU_KEY_MINUS */
    GLFW_KEY_EQUAL,        /* MU_KEY_EQUAL */
    GLFW_KEY_LEFT_BRACKET, /* MU_KEY_LEFT_BRACKET */
    GLFW_KEY_RIGHT_BRACKET,/* MU_KEY_RIGHT_BRACKET */
    GLFW_KEY_COMMA,        /* MU_KEY_COMMA */
    GLFW_KEY_PERIOD,       /* MU_KEY_PERIOD */
    GLFW_KEY_SLASH,        /* MU_KEY_SLASH */
    GLFW_KEY_SEMICOLON,    /* MU_KEY_SEMICOLON */
    GLFW_KEY_APOSTROPHE,   /* MU_KEY_APOSTROPHE */
    GLFW_KEY_BACKSLASH,    /* MU_KEY_BACKSLASH */
    GLFW_KEY_GRAVE_ACCENT, /* MU_KEY_GRAVE */
    GLFW_KEY_F1, GLFW_KEY_F2, GLFW_KEY_F3, GLFW_KEY_F4, GLFW_KEY_F5, GLFW_KEY_F6,
    GLFW_KEY_F7, GLFW_KEY_F8, GLFW_KEY_F9, GLFW_KEY_F10, GLFW_KEY_F11, GLFW_KEY_F12,
    GLFW_KEY_A, GLFW_KEY_B, GLFW_KEY_C, GLFW_KEY_D, GLFW_KEY_E, GLFW_KEY_F,
    GLFW_KEY_G, GLFW_KEY_H, GLFW_KEY_I, GLFW_KEY_J, GLFW_KEY_K, GLFW_KEY_L,
    GLFW_KEY_M, GLFW_KEY_N, GLFW_KEY_O, GLFW_KEY_P, GLFW_KEY_Q, GLFW_KEY_R,
    GLFW_KEY_S, GLFW_KEY_T, GLFW_KEY_U, GLFW_KEY_V, GLFW_KEY_W, GLFW_KEY_X,
    GLFW_KEY_Y, GLFW_KEY_Z,
    GLFW_KEY_0, GLFW_KEY_1, GLFW_KEY_2, GLFW_KEY_3, GLFW_KEY_4,
    GLFW_KEY_5, GLFW_KEY_6, GLFW_KEY_7, GLFW_KEY_8, GLFW_KEY_9,
};

uint32_t platform_slot_from_key(MuKey key) {
    int scancode;
    if ((uint32_t)key >= (uint32_t)MU_KEY_COUNT) return PLATFORM_SLOT_NONE;
    if (k_mu_key_to_glfw[key] == GLFW_KEY_UNKNOWN) return PLATFORM_SLOT_NONE;
    scancode = glfwGetKeyScancode(k_mu_key_to_glfw[key]);
    return scancode < 0 ? PLATFORM_SLOT_NONE : (uint32_t)scancode;
}

/* Fill the state's MuKey -> slot map so mu_input_key_down() works. */
static void fill_key_slots(MuInputState *state) {
    uint32_t i;
    for (i = 0; i < (uint32_t)MU_KEY_COUNT; ++i) {
        const uint32_t slot = platform_slot_from_key((MuKey)i);
        if (slot != PLATFORM_SLOT_NONE) mu_input_key_set_slot(state, (MuKey)i, slot);
    }
}

static bool name_equals(const char *a, const char *b) {
    if (!a || !b) return false;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

static int key_from_name(const char *name) {
    unsigned i;

    if (!name || !name[0]) return GLFW_KEY_UNKNOWN;
    if (!name[1] && isalpha((unsigned char)name[0]))
        return GLFW_KEY_A + (toupper((unsigned char)name[0]) - 'A');
    if (!name[1] && isdigit((unsigned char)name[0]))
        return GLFW_KEY_0 + (name[0] - '0');
    for (i = 0; i < sizeof k_named_keys / sizeof k_named_keys[0]; ++i)
        if (name_equals(name, k_named_keys[i].name)) return k_named_keys[i].key;
    return GLFW_KEY_UNKNOWN;
}

uint32_t platform_slot_from_name(const char *name) {
    unsigned i;
    int key, scancode;

    if (!name) return PLATFORM_SLOT_NONE;
    for (i = 0; i < sizeof k_mouse_names / sizeof k_mouse_names[0]; ++i)
        if (name_equals(name, k_mouse_names[i])) return MU_INPUT_MOUSE_SLOT_OF(i);

    key = key_from_name(name);
    if (key == GLFW_KEY_UNKNOWN) return PLATFORM_SLOT_NONE;
    scancode = glfwGetKeyScancode(key);
    return scancode < 0 ? PLATFORM_SLOT_NONE : (uint32_t)scancode;
}

const char *platform_slot_label(uint32_t slot, char *scratch, size_t capacity) {
    const char *layout;
    unsigned i;

    if (slot >= MU_INPUT_MOUSE_SLOT) {
        const uint32_t button = slot - MU_INPUT_MOUSE_SLOT;
        if (button < sizeof k_mouse_names / sizeof k_mouse_names[0]) return k_mouse_names[button];
        snprintf(scratch, capacity, "Mouse%u", (unsigned)button + 1u);
        return scratch;
    }

    /* The user's layout wins for anything printable. */
    layout = glfwGetKeyName(GLFW_KEY_UNKNOWN, (int)slot);
    if (layout && layout[0] && !(layout[0] == ' ' && layout[1] == '\0')) {
        if (!layout[1]) {
            scratch[0] = (char)toupper((unsigned char)layout[0]);
            scratch[1] = '\0';
        } else {
            snprintf(scratch, capacity, "%s", layout);
        }
        return scratch;
    }

    /* Non-printable (Esc, arrows, F-keys): name the token that sits here. */
    for (i = 0; i < sizeof k_named_keys / sizeof k_named_keys[0]; ++i) {
        if (glfwGetKeyScancode(k_named_keys[i].key) == (int)slot) return k_named_keys[i].name;
    }
    snprintf(scratch, capacity, "scancode %u", (unsigned)slot);
    return scratch;
}

bool platform_bind_control_name(MuInputMap *map, uint32_t action, const char *control_name) {
    const uint32_t slot = platform_slot_from_name(control_name);
    if (slot == PLATFORM_SLOT_NONE) {
        fprintf(stderr, "[platform] unknown control name '%s'\n", control_name);
        return false;
    }
    return mu_input_bind_control(map, action, slot);
}

bool platform_bind_key(MuInputMap *map, uint32_t action, const MuInputState *state, MuKey key) {
    return mu_input_bind_key(map, action, state, key);
}

/* ---- lifecycle ---------------------------------------------------------- */

static void on_glfw_error(int code, const char *message) {
    fprintf(stderr, "[platform] GLFW %d: %s\n", code, message);
}

static void on_key(GLFWwindow *handle, int key, int scancode, int action, int mods) {
    Platform *p = (Platform *)glfwGetWindowUserPointer(handle);
    uint32_t slot;

    (void)key;
    (void)mods;
    if (!p) return;
    if (action != GLFW_PRESS && action != GLFW_RELEASE) return; /* repeat is not an edge */
    slot = scancode >= 0 ? (uint32_t)scancode : (uint32_t)key;
    mu_input_push_control(&p->state, slot, action == GLFW_PRESS, p->now_ns);
}

static void on_mouse_button(GLFWwindow *handle, int button, int action, int mods) {
    Platform *p = (Platform *)glfwGetWindowUserPointer(handle);

    (void)mods;
    if (!p || button < 0) return;
    mu_input_push_control(&p->state, MU_INPUT_MOUSE_SLOT_OF(button), action == GLFW_PRESS,
                          p->now_ns);
}

static void on_scroll(GLFWwindow *handle, double x_offset, double y_offset) {
    Platform *p = (Platform *)glfwGetWindowUserPointer(handle);

    if (!p) return;
    mu_input_wheel_add(&p->state, (float)x_offset, (float)y_offset);
}

static void on_focus(GLFWwindow *handle, int focused) {
    Platform *p = (Platform *)glfwGetWindowUserPointer(handle);
    double x = 0.0, y = 0.0;

    if (!p) return;
    if (!focused) {
        mu_input_focus_lost(&p->state);
        memset(p->pad_prev, 0, sizeof p->pad_prev);
        glfwGetCursorPos(handle, &x, &y);      /* no delta on the way back */
        mu_input_mouse_warp(&p->state, (float)x, (float)y);
    }
}

bool platform_init(Platform *p, uint32_t width, uint32_t height, const char *title) {
    double x = 0.0, y = 0.0;
    int jid;

    if (!p) return false;
    memset(p, 0, sizeof *p);

    glfwSetErrorCallback(on_glfw_error);
    if (!glfwInit()) return false;

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    p->handle = glfwCreateWindow((int)width, (int)height, title, NULL, NULL);
    if (!p->handle) {
        glfwTerminate();
        return false;
    }

    glfwSetWindowUserPointer(p->handle, p);
    glfwSetKeyCallback(p->handle, on_key);
    glfwSetMouseButtonCallback(p->handle, on_mouse_button);
    glfwSetScrollCallback(p->handle, on_scroll);
    glfwSetWindowFocusCallback(p->handle, on_focus);

    mu_input_init(&p->state);
    fill_key_slots(&p->state); /* simple API: mu_input_key_down(MU_KEY_W) works */
    mu_input_map_init(&p->map, MU_INPUT_MAX_ACTIONS);
    mu_input_actions_reset(&p->actions);
    for (jid = 0; jid <= GLFW_JOYSTICK_LAST; ++jid) p->pad_of_jid[jid] = 0xFFu;

    glfwGetCursorPos(p->handle, &x, &y);
    mu_input_mouse_warp(&p->state, (float)x, (float)y);
    p->now_ns = (uint64_t)(glfwGetTime() * 1e9);
    return true;
}

void platform_shutdown(Platform *platform) {
    if (!platform) return;
    if (platform->handle) glfwDestroyWindow(platform->handle);
    platform->handle = NULL;
    glfwTerminate();
}

Platform *platform_create(uint32_t width, uint32_t height, const char *title) {
    Platform *p = (Platform *)calloc(1, sizeof *p);
    if (!p) return NULL;
    if (!platform_init(p, width, height, title)) {
        free(p);
        return NULL;
    }
    return p;
}

void platform_destroy(Platform *platform) {
    if (!platform) return;
    platform_shutdown(platform);
    free(platform);
}

bool platform_should_close(const Platform *platform) {
    return !platform || glfwWindowShouldClose(platform->handle) == GLFW_TRUE;
}

void platform_request_close(Platform *platform) {
    if (platform && platform->handle) glfwSetWindowShouldClose(platform->handle, GLFW_TRUE);
}

const MuInputState *platform_state(const Platform *platform) {
    return platform ? &platform->state : NULL;
}

MuInputState *platform_state_mutable(Platform *platform) {
    return platform ? &platform->state : NULL;
}

const MuInputActions *platform_actions(const Platform *platform) {
    return platform ? &platform->actions : NULL;
}

MuInputMap *platform_map(Platform *platform) {
    return platform ? &platform->map : NULL;
}

uint64_t platform_time_ns(const Platform *platform) {
    (void)platform;
    return (uint64_t)(glfwGetTime() * 1e9);
}

bool platform_key_down(const Platform *platform, MuKey key) {
    return platform && mu_input_key_down(&platform->state, key);
}
bool platform_key_pressed(const Platform *platform, MuKey key) {
    return platform && mu_input_key_pressed(&platform->state, key);
}
bool platform_key_released(const Platform *platform, MuKey key) {
    return platform && mu_input_key_released(&platform->state, key);
}
bool platform_mouse_down(const Platform *platform, MuMouseButton button) {
    return platform && mu_input_mouse_down(&platform->state, button);
}
bool platform_mouse_pressed(const Platform *platform, MuMouseButton button) {
    return platform && mu_input_mouse_pressed(&platform->state, button);
}
bool platform_mouse_released(const Platform *platform, MuMouseButton button) {
    return platform && mu_input_mouse_released(&platform->state, button);
}

/* Gamepads are polled (GLFW has no button events for them); edges are
 * synthesized so they still travel through the same event ring. */
static int find_free_pad_slot(const Platform *p) {
    int jid;
    uint32_t slot;
    for (slot = 0; slot < MU_INPUT_MAX_PADS; ++slot) {
        bool used = false;
        for (jid = GLFW_JOYSTICK_1; jid <= GLFW_JOYSTICK_LAST; ++jid)
            if (p->pad_of_jid[jid] == (uint8_t)slot) { used = true; break; }
        if (!used) return (int)slot;
    }
    return -1;
}

static void poll_gamepads(Platform *p) {
    int jid;

    for (jid = GLFW_JOYSTICK_1; jid <= GLFW_JOYSTICK_LAST; ++jid) {
        GLFWgamepadstate state;
        uint32_t axis, button;
        const bool is_pad = glfwJoystickIsGamepad(jid) == GLFW_TRUE &&
                            glfwGetGamepadState(jid, &state) == GLFW_TRUE;

        if (!is_pad) {
            /* Disconnect: free the slot, zero prev, emit disconnect event. */
            if (p->pad_of_jid[jid] != 0xFFu) {
                const uint32_t slot = p->pad_of_jid[jid];
                mu_input_pad_connect(&p->state, slot, false, p->now_ns);
                memset(p->pad_prev[slot], 0, sizeof p->pad_prev[slot]);
                p->pad_of_jid[jid] = 0xFFu;
            }
            continue;
        }
        if (p->pad_of_jid[jid] == 0xFFu) {
            const int slot = find_free_pad_slot(p);
            if (slot < 0) continue; /* all 4 slots taken */
            p->pad_of_jid[jid] = (uint8_t)slot;
            memset(p->pad_prev[slot], 0, sizeof p->pad_prev[slot]);
            mu_input_pad_connect(&p->state, (uint32_t)slot, true, p->now_ns);
        }

        const uint32_t this_pad = p->pad_of_jid[jid];
        for (axis = 0; axis < MU_INPUT_PAD_AXES; ++axis)
            mu_input_pad_axis(&p->state, this_pad, axis, state.axes[axis]);

        for (button = 0; button <= GLFW_GAMEPAD_BUTTON_LAST && button < MU_INPUT_PAD_BUTTONS;
             ++button) {
            const uint8_t down = (uint8_t)(state.buttons[button] == GLFW_PRESS);
            if (down != p->pad_prev[this_pad][button]) {
                p->pad_prev[this_pad][button] = down;
                mu_input_push_pad_button(&p->state, this_pad, button, down != 0u, p->now_ns);
            }
        }
    }
}

void platform_poll_events(Platform *platform) {
    double x = 0.0, y = 0.0;

    if (!platform || !platform->handle) return;

    /* Single clock reading for the whole poll: callbacks (which fire inside
     * glfwPollEvents) and pad edges all share it, so events are attributed
     * to exactly one frame. */
    platform->now_ns = (uint64_t)(glfwGetTime() * 1e9);
    glfwPollEvents();

    glfwGetCursorPos(platform->handle, &x, &y);
    mu_input_mouse_pos(&platform->state, (float)x, (float)y);
    poll_gamepads(platform);
}

void platform_frame_begin(Platform *platform) {
    if (!platform) return;
    mu_input_frame_begin(&platform->state, platform->now_ns);
}

void platform_resolve(Platform *platform) {
    if (!platform) return;
    mu_input_resolve(&platform->state, &platform->map, &platform->actions,
                     platform->state.dt);
}

void platform_poll(Platform *platform) {
    if (!platform) return;
    platform_poll_events(platform);
    platform_frame_begin(platform);
    platform_resolve(platform);
}

bool platform_create_surface(const Platform *platform, VkInstance instance,
                             VkSurfaceKHR *out_surface) {
    VkResult result;

    *out_surface = VK_NULL_HANDLE;
    if (!platform || !platform->handle) return false;
    result = glfwCreateWindowSurface(instance, platform->handle, NULL, out_surface);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "[platform] glfwCreateWindowSurface failed: %d\n", (int)result);
        *out_surface = VK_NULL_HANDLE;
        return false;
    }
    return true;
}
