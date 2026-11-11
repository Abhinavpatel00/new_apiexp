/* Gate test for mu_input.h: edges, tap capture, wheel, focus loss, maps,
 * analog deadzone, repeat — all without a window, a GPU or an OS. */
#include "mu_input.h"

#include <stdio.h>
#include <string.h>

#define FRAME_NS 16666667ull /* 60 Hz */
#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "input check failed at %s:%d: %s\n", __FILE__, __LINE__, #x); \
    return 1; \
} } while (0)

#define SLOT_A 4u
#define SLOT_B 5u
#define SLOT_C 6u

enum { ACT_ORBIT_LEFT, ACT_ORBIT_RIGHT, ACT_THROTTLE, ACT_UNBOUND, ACT_COUNT };

static int bind_galaxy_actions(MuInputMap *map) {
    mu_input_map_init(map, ACT_COUNT);
    CHECK(mu_input_bind_control(map, ACT_ORBIT_LEFT, SLOT_A));
    CHECK(mu_input_bind_control(map, ACT_ORBIT_RIGHT, SLOT_B));
    CHECK(mu_input_bind_control(map, ACT_ORBIT_RIGHT, SLOT_C));   /* two keys, one action */
    CHECK(mu_input_bind_pad_axis(map, ACT_THROTTLE, 0u, MU_INPUT_PAD_AXIS_LY, -1));
    return 0;
}

int main(void) {
    MuInputState in;
    MuInputMap map;
    MuInputActions actions;
    uint64_t t;

    /* ---- 1. edges across frames -------------------------------------- */
    mu_input_init(&in);
    mu_input_actions_reset(&actions);
    t = 1000000000ull;
    mu_input_push_control(&in, SLOT_A, true, t);
    mu_input_frame_begin(&in, t);
    CHECK(mu_input_control_down(&in, SLOT_A));
    CHECK(mu_input_control_pressed(&in, SLOT_A));
    CHECK(!mu_input_control_released(&in, SLOT_A));

    mu_input_frame_begin(&in, t + FRAME_NS);
    CHECK(mu_input_control_down(&in, SLOT_A));           /* still held */
    CHECK(!mu_input_control_pressed(&in, SLOT_A));       /* but not pressed again */

    mu_input_push_control(&in, SLOT_A, false, t + FRAME_NS);
    mu_input_frame_begin(&in, t + 2 * FRAME_NS);
    CHECK(!mu_input_control_down(&in, SLOT_A));
    CHECK(mu_input_control_released(&in, SLOT_A));
    CHECK(!mu_input_control_pressed(&in, SLOT_A));

    /* ---- 2. a tap that starts and ends inside one frame -------------- */
    mu_input_frame_begin(&in, t + 3 * FRAME_NS);
    mu_input_push_control(&in, SLOT_B, true, t + 3 * FRAME_NS);
    mu_input_push_control(&in, SLOT_B, false, t + 3 * FRAME_NS);
    mu_input_frame_begin(&in, t + 4 * FRAME_NS);
    CHECK(!mu_input_control_down(&in, SLOT_B));          /* already up */
    CHECK(mu_input_control_pressed(&in, SLOT_B));        /* but the tap is seen */
    mu_input_frame_begin(&in, t + 5 * FRAME_NS);
    CHECK(!mu_input_control_pressed(&in, SLOT_B));       /* and counts exactly once */

    /* ---- 3. wheel: several callbacks, one consume -------------------- */
    mu_input_wheel_add(&in, 0.0f, 1.0f);
    mu_input_wheel_add(&in, 0.0f, 2.0f);
    mu_input_frame_begin(&in, t + 6 * FRAME_NS);
    CHECK(in.wheel_y == 3.0f);
    mu_input_frame_begin(&in, t + 7 * FRAME_NS);
    CHECK(in.wheel_y == 0.0f);

    /* ---- 4. pointer delta, and no snap after a warp ------------------ */
    mu_input_mouse_pos(&in, 10.0f, 20.0f);
    mu_input_frame_begin(&in, t + 8 * FRAME_NS);
    mu_input_mouse_pos(&in, 13.0f, 18.0f);
    mu_input_frame_begin(&in, t + 9 * FRAME_NS);
    CHECK(in.mouse_dx == 3.0f && in.mouse_dy == -2.0f);
    mu_input_mouse_warp(&in, 500.0f, 500.0f);            /* e.g. focus regained */
    mu_input_frame_begin(&in, t + 10 * FRAME_NS);
    CHECK(in.mouse_dx == 0.0f && in.mouse_dy == 0.0f);

    /* ---- 5. focus loss drops everything held ------------------------- */
    mu_input_push_control(&in, SLOT_A, true, t);
    mu_input_frame_begin(&in, t + 11 * FRAME_NS);
    CHECK(mu_input_control_down(&in, SLOT_A));
    mu_input_focus_lost(&in);
    CHECK(!mu_input_control_down(&in, SLOT_A));
    mu_input_frame_begin(&in, t + 12 * FRAME_NS);
    CHECK(!mu_input_control_pressed(&in, SLOT_A));       /* no phantom press */

    /* ---- 6. frame delta is clamped ---------------------------------- */
    mu_input_frame_begin(&in, t + 13 * FRAME_NS);
    mu_input_frame_begin(&in, t + 13 * FRAME_NS + 5000000000ull); /* 5 s stall */
    CHECK(in.dt == MU_INPUT_MAX_DT);

    /* ---- 7. the event ring is bounded and reports overflow ---------- */
    mu_input_init(&in);
    {
        uint32_t i, ok = 0;
        for (i = 0; i < MU_INPUT_EVENT_CAP + 32u; ++i)
            if (mu_input_push(&in, MU_INPUT_EV_CONTROL_DOWN, 100u + i, -1, 0.0f, 0ull)) ++ok;
        CHECK(ok == MU_INPUT_EVENT_CAP);
        CHECK(in.event_dropped == 32u);
        mu_input_frame_begin(&in, FRAME_NS);
        CHECK(in.event_count == 0u);
    }

    /* ---- 8. map: OR of sources, unbound actions, analog with deadzone */
    mu_input_init(&in);
    CHECK(bind_galaxy_actions(&map) == 0);
    mu_input_actions_reset(&actions);

    mu_input_push_control(&in, SLOT_A, true, 0ull);
    mu_input_frame_begin(&in, FRAME_NS);
    mu_input_resolve(&in, &map, &actions, in.dt);
    CHECK(mu_input_action_down(&actions, ACT_ORBIT_LEFT));
    CHECK(mu_input_action_pressed(&actions, ACT_ORBIT_LEFT));
    CHECK(mu_input_action_value(&actions, ACT_ORBIT_LEFT) == 1.0f);
    CHECK(!mu_input_action_down(&actions, ACT_ORBIT_RIGHT));
    CHECK(!mu_input_action_down(&actions, ACT_UNBOUND));      /* nothing bound */
    CHECK(mu_input_action_value(&actions, ACT_UNBOUND) == 0.0f);

    mu_input_frame_begin(&in, 2 * FRAME_NS);
    mu_input_resolve(&in, &map, &actions, in.dt);
    CHECK(!mu_input_action_pressed(&actions, ACT_ORBIT_LEFT)); /* edge is one frame */

    /* second source for the same action keeps it down */
    mu_input_push_control(&in, SLOT_A, false, 0ull);
    mu_input_push_control(&in, SLOT_C, true, 0ull);
    mu_input_frame_begin(&in, 3 * FRAME_NS);
    mu_input_resolve(&in, &map, &actions, in.dt);
    CHECK(mu_input_action_down(&actions, ACT_ORBIT_RIGHT));

    /* analog: stick pushed up reads negative on LY, dir=-1 flips it */
    mu_input_pad_axis(&in, 0u, MU_INPUT_PAD_AXIS_LY, -1.0f);
    mu_input_frame_begin(&in, 4 * FRAME_NS);
    mu_input_resolve(&in, &map, &actions, in.dt);
    CHECK(mu_input_action_value(&actions, ACT_THROTTLE) > 0.99f);

    /* inside the deadzone, an axis is zero */
    mu_input_pad_axis(&in, 0u, MU_INPUT_PAD_AXIS_LY, -0.1f);
    mu_input_frame_begin(&in, 5 * FRAME_NS);
    mu_input_resolve(&in, &map, &actions, in.dt);
    CHECK(mu_input_action_value(&actions, ACT_THROTTLE) == 0.0f);
    CHECK(!mu_input_action_down(&actions, ACT_THROTTLE));

    /* partially deflected: rescaled so the deadzone does not cost range */
    mu_input_pad_axis(&in, 0u, MU_INPUT_PAD_AXIS_LY, -0.575f);
    mu_input_frame_begin(&in, 6 * FRAME_NS);
    mu_input_resolve(&in, &map, &actions, in.dt);
    CHECK(mu_input_action_value(&actions, ACT_THROTTLE) > 0.49f);
    CHECK(mu_input_action_value(&actions, ACT_THROTTLE) < 0.51f);

    /* ---- 8b. a tap inside one frame must reach the action layer ------ */
    {
        MuInputState tap_in;
        MuInputActions tap_actions;

        mu_input_init(&tap_in);
        mu_input_map_init(&map, ACT_COUNT);
        CHECK(mu_input_bind_control(&map, ACT_ORBIT_LEFT, SLOT_A));
        mu_input_actions_reset(&tap_actions);

        mu_input_push_control(&tap_in, SLOT_A, true, 0ull);
        mu_input_push_control(&tap_in, SLOT_A, false, 0ull);
        mu_input_frame_begin(&tap_in, FRAME_NS);
        mu_input_resolve(&tap_in, &map, &tap_actions, tap_in.dt);
        CHECK(!mu_input_action_down(&tap_actions, ACT_ORBIT_LEFT));      /* already up */
        CHECK(mu_input_action_pressed(&tap_actions, ACT_ORBIT_LEFT));    /* but seen */
        CHECK(mu_input_action_released(&tap_actions, ACT_ORBIT_LEFT));

        mu_input_frame_begin(&tap_in, 2 * FRAME_NS);
        mu_input_resolve(&tap_in, &map, &tap_actions, tap_in.dt);
        CHECK(!mu_input_action_pressed(&tap_actions, ACT_ORBIT_LEFT));   /* exactly once */
        CHECK(!mu_input_action_released(&tap_actions, ACT_ORBIT_LEFT));
    }

    /* ---- 9. opposed axis, held time, repeat ------------------------- */
    CHECK(mu_input_deadzone_f32(0.5f, 0.5f) == 0.0f);
    CHECK(mu_input_deadzone_f32(1.0f, 0.25f) == 1.0f);
    CHECK(mu_input_deadzone_f32(-1.0f, 0.25f) == -1.0f);
    {
        float x = 1.0f, y = 0.0f;
        mu_input_deadzone2_f32(&x, &y, 0.5f);
        CHECK(y == 0.0f && x == 1.0f);
        x = 0.2f; y = 0.2f;
        mu_input_deadzone2_f32(&x, &y, 0.5f);
        CHECK(x == 0.0f && y == 0.0f);                     /* inside radial dead */
    }

    mu_input_init(&in);
    mu_input_map_init(&map, ACT_COUNT);
    mu_input_bind_control(&map, ACT_ORBIT_LEFT, SLOT_A);
    mu_input_bind_control(&map, ACT_ORBIT_RIGHT, SLOT_B);
    mu_input_actions_reset(&actions);
    mu_input_push_control(&in, SLOT_A, true, 0ull);
    mu_input_frame_begin(&in, FRAME_NS);
    mu_input_resolve(&in, &map, &actions, in.dt);
    CHECK(mu_input_axis(&actions, ACT_ORBIT_LEFT, ACT_ORBIT_RIGHT) == -1.0f);
    CHECK(mu_input_axis(&actions, ACT_ORBIT_RIGHT, ACT_ORBIT_LEFT) == 1.0f);

    {
        int i, repeats = 0;
        const float dt = 0.01f;   /* 100 Hz frames */
        for (i = 0; i < 120; ++i) {
            mu_input_resolve(&in, &map, &actions, dt);
            if (mu_input_action_repeat(&actions, ACT_ORBIT_LEFT, 0.30f, 20.0f, dt)) ++repeats;
        }
        CHECK(actions.held_time[ACT_ORBIT_LEFT] > 1.1f);
        /* first repeat at 0.30 s, then every 0.05 s -> ~15 in 1.2 s */
        CHECK(repeats >= 12 && repeats <= 24);
    }

    /* ---- 10. simple API: key_down / key_pressed / mouse_down ---------- */
    {
        MuInputState s;
        mu_input_init(&s);
        mu_input_key_map_identity(&s); /* MU_KEY_A -> slot MU_KEY_A, etc. */
        /* unmapped key reads false, never crashes */
        mu_input_key_map_clear(&s);
        CHECK(!mu_input_key_down(&s, MU_KEY_W));
        CHECK(!mu_input_key_pressed(&s, MU_KEY_W));
        mu_input_key_map_identity(&s);

        mu_input_push_control(&s, MU_KEY_W, true, 0ull);
        mu_input_frame_begin(&s, FRAME_NS);
        CHECK(mu_input_key_down(&s, MU_KEY_W));
        CHECK(mu_input_key_pressed(&s, MU_KEY_W));
        CHECK(!mu_input_key_released(&s, MU_KEY_W));
        CHECK(!mu_input_key_down(&s, MU_KEY_A));

        mu_input_frame_begin(&s, 2 * FRAME_NS);
        CHECK(mu_input_key_down(&s, MU_KEY_W));       /* held */
        CHECK(!mu_input_key_pressed(&s, MU_KEY_W));   /* edge is one frame */

        /* tap inside one frame: pressed once, down false */
        mu_input_push_control(&s, MU_KEY_W, false, 0ull);
        mu_input_frame_begin(&s, 3 * FRAME_NS);
        CHECK(mu_input_key_released(&s, MU_KEY_W));
        mu_input_push_control(&s, MU_KEY_C, true, 0ull);
        mu_input_push_control(&s, MU_KEY_C, false, 0ull);
        mu_input_frame_begin(&s, 4 * FRAME_NS);
        CHECK(!mu_input_key_down(&s, MU_KEY_C));
        CHECK(mu_input_key_pressed(&s, MU_KEY_C));

        /* mouse by enum: MU_MOUSE_RIGHT == button 1 */
        mu_input_push_control(&s, MU_INPUT_MOUSE_SLOT_OF(1u), true, 0ull);
        mu_input_frame_begin(&s, 5 * FRAME_NS);
        CHECK(mu_input_mouse_down(&s, MU_MOUSE_RIGHT));
        CHECK(mu_input_mouse_pressed(&s, MU_MOUSE_RIGHT));
        CHECK(!mu_input_mouse_down(&s, MU_MOUSE_LEFT));
        mu_input_push_control(&s, MU_INPUT_MOUSE_SLOT_OF(1u), false, 0ull);
        mu_input_frame_begin(&s, 6 * FRAME_NS);
        CHECK(mu_input_mouse_released(&s, MU_MOUSE_RIGHT));

        /* explicit slot mapping (backend style): slot != enum value */
        mu_input_init(&s);
        mu_input_key_set_slot(&s, MU_KEY_W, SLOT_A);
        mu_input_push_control(&s, SLOT_A, true, 0ull);
        mu_input_frame_begin(&s, FRAME_NS);
        CHECK(mu_input_key_down(&s, MU_KEY_W));
        CHECK(mu_input_key_slot(&s, MU_KEY_W) == SLOT_A);
    }

    /* ---- 11. wheel: single path + take-once consume ------------------- */
    {
        MuInputState s;
        float wx = 99.0f, wy = 99.0f;
        mu_input_init(&s);
        mu_input_wheel_add(&s, 1.0f, 2.0f);
        mu_input_wheel_add(&s, 0.0f, 3.0f);
        /* events are queued (single path), not pre-accumulated into wheel_* */
        CHECK(s.event_count == 2u);
        mu_input_frame_begin(&s, FRAME_NS);
        CHECK(s.wheel_x == 1.0f && s.wheel_y == 5.0f);
        mu_input_wheel_consume(&s, &wx, &wy);
        CHECK(wx == 1.0f && wy == 5.0f);
        CHECK(s.wheel_x == 0.0f && s.wheel_y == 0.0f); /* taken */
        mu_input_frame_begin(&s, 2 * FRAME_NS);
        CHECK(s.wheel_y == 0.0f); /* no phantom scroll next frame */
    }

    /* ---- 12. pad connect / disconnect --------------------------------- */
    {
        MuInputState s;
        mu_input_init(&s);
        CHECK(mu_input_pad_connected(&s, 0u)); /* v1 default: connected */
        mu_input_pad_axis(&s, 0u, MU_INPUT_PAD_AXIS_LX, 0.9f);
        mu_input_push_pad_button(&s, 0u, 1u, true, 0ull);
        mu_input_frame_begin(&s, FRAME_NS);
        CHECK(mu_input_pad_button_down(&s, 0u, 1u));
        mu_input_pad_connect(&s, 0u, false, 0ull);
        mu_input_frame_begin(&s, 2 * FRAME_NS);
        CHECK(!mu_input_pad_connected(&s, 0u));
        CHECK(!mu_input_pad_button_down(&s, 0u, 1u)); /* zeroed, no stuck */
        CHECK(mu_input_pad_axis_value(&s, 0u, MU_INPUT_PAD_AXIS_LX) == 0.0f);
        mu_input_pad_connect(&s, 0u, true, 0ull);
        mu_input_frame_begin(&s, 3 * FRAME_NS);
        CHECK(mu_input_pad_connected(&s, 0u));
        CHECK(!mu_input_pad_button_pressed(&s, 0u, 1u)); /* no phantom press */
    }

    /* ---- 13. tick sampler: buffered / coyote / chords ----------------- */
    {
        MuInputState s;
        MuInputMap m;
        MuInputFrame f;
        MuInputChords chords;
        enum { ACT_JUMP, ACT_CROUCH, ACT_COUNT2 };
        mu_input_init(&s);
        mu_input_key_map_identity(&s);
        mu_input_map_init(&m, ACT_COUNT2);
        CHECK(mu_input_bind_key(&m, ACT_JUMP, &s, MU_KEY_SPACE));
        CHECK(mu_input_bind_key(&m, ACT_CROUCH, &s, MU_KEY_C));
        mu_input_frame_reset(&f);
        mu_input_chords_init(&chords);
        CHECK(mu_input_chord_add(&chords, ACT_JUMP, ACT_CROUCH, 5u));

        /* tick 10: press jump */
        mu_input_push_control(&s, MU_KEY_SPACE, true, 0ull);
        mu_input_frame_begin(&s, FRAME_NS);
        mu_input_sample_tick(&s, &m, &chords, &f, 10u);
        CHECK(mu_input_frame_pressed(&f, ACT_JUMP));
        CHECK(mu_input_frame_down(&f, ACT_JUMP));
        CHECK(f.held_ticks[ACT_JUMP] == 1u);
        CHECK(mu_input_buffered(&f, ACT_JUMP, 7u));
        CHECK(mu_input_coyote(&f, ACT_JUMP, 7u));

        /* tick 13: press crouch -> chord (jump pressed 3 ticks ago, window 5) */
        mu_input_push_control(&s, MU_KEY_C, true, 0ull);
        mu_input_frame_begin(&s, 2 * FRAME_NS);
        mu_input_sample_tick(&s, &m, &chords, &f, 13u);
        CHECK(mu_input_chord_pressed(&f, 0u));
        CHECK(!mu_input_frame_pressed(&f, ACT_JUMP)); /* edge was tick 10 */

        /* tick 18: buffered window (7) expired for jump (18-10=8 > 7) */
        mu_input_frame_begin(&s, 3 * FRAME_NS);
        mu_input_sample_tick(&s, &m, &chords, &f, 18u);
        CHECK(!mu_input_buffered(&f, ACT_JUMP, 7u));
        CHECK(!mu_input_chord_pressed(&f, 0u)); /* chord is one tick only */

        /* tick repeat: delay 3, period 2 -> fires at held 3,5,7,... */
        {
            MuInputFrame rf;
            uint32_t tick;
            int fires = 0;
            mu_input_frame_reset(&rf);
            for (tick = 0; tick < 10u; ++tick) {
                mu_input_sample_tick(&s, &m, NULL, &rf, tick);
                if (mu_input_frame_repeat(&rf, ACT_JUMP, 3u, 2u)) ++fires;
            }
            CHECK(fires == 4); /* held 3,5,7,9 */
        }
    }

    /* ---- 14. record / replay: event span round-trip ------------------- */
    {
        MuInputState a, b;
        MuInputEventSpan span;
        MuInputEvent copy[8];
        uint32_t n, i;
        mu_input_init(&a);
        mu_input_push_control(&a, SLOT_A, true, 111ull);
        mu_input_push_control(&a, SLOT_B, true, 222ull);
        mu_input_wheel_add(&a, 0.0f, 2.0f);
        span = mu_input_events(&a);
        n = span.count;
        CHECK(n == 3u);
        for (i = 0; i < n; ++i) copy[i] = span.ptr[i]; /* copy before drain */
        mu_input_frame_begin(&a, FRAME_NS);
        CHECK(a.event_count == 0u); /* drained */

        mu_input_init(&b);
        {
            MuInputEventSpan replay_span;
            replay_span.ptr = copy;
            replay_span.count = n;
            CHECK(mu_input_replay_events(&b, replay_span) == n);
        }
        mu_input_frame_begin(&b, FRAME_NS);
        CHECK(mu_input_control_down(&b, SLOT_A));
        CHECK(mu_input_control_down(&b, SLOT_B));
        CHECK(b.wheel_y == a.wheel_y);
        CHECK(mu_input_control_pressed(&b, SLOT_A) == mu_input_control_pressed(&a, SLOT_A));
    }

    /* ---- 15. per-source deadzone + slope curve ------------------------ */
    {
        MuInputState s;
        MuInputMap m;
        MuInputActions act;
        enum { ACT_STICK, ACT_COUNT3 };
        mu_input_init(&s);
        mu_input_map_init(&m, ACT_COUNT3);
        /* deadzone 0.5 (explicit) vs global 0.15: 0.3 deflection reads 0 here */
        CHECK(mu_input_bind_pad_axis_ex(&m, ACT_STICK, 0u, MU_INPUT_PAD_AXIS_LX, 1, 0.5f, MU_INPUT_CURVE_LINEAR));
        mu_input_actions_reset(&act);
        mu_input_pad_axis(&s, 0u, MU_INPUT_PAD_AXIS_LX, 0.3f);
        mu_input_frame_begin(&s, FRAME_NS);
        mu_input_resolve(&s, &m, &act, s.dt);
        CHECK(mu_input_action_value(&act, ACT_STICK) == 0.0f);
        mu_input_pad_axis(&s, 0u, MU_INPUT_PAD_AXIS_LX, 1.0f);
        mu_input_frame_begin(&s, 2 * FRAME_NS);
        mu_input_resolve(&s, &m, &act, s.dt);
        CHECK(mu_input_action_value(&act, ACT_STICK) == 1.0f);
        /* slope curve helper: 0.575 with dead 0.25 -> rescaled 0.5 -> shaped */
        CHECK(mu_input_deadzone_slope_f32(0.575f, 0.25f, 0.15f) > 0.3f);
        CHECK(mu_input_sign_f32(-2.0f) == -1.0f);
        CHECK(mu_input_sign_f32(0.0f) == 0.0f);
        CHECK(mu_input_sign_f32(3.0f) == 1.0f);
    }

    /* ---- 16. mouse_move + actions carry mouse/wheel ------------------- */
    {
        MuInputState s;
        MuInputMap m;
        MuInputActions act;
        mu_input_init(&s);
        mu_input_map_init(&m, ACT_COUNT);
        mu_input_actions_reset(&act);
        mu_input_mouse_pos(&s, 100.0f, 100.0f);
        mu_input_frame_begin(&s, FRAME_NS);
        mu_input_mouse_move(&s, 5.0f, -3.0f);
        mu_input_wheel_add(&s, 0.0f, 1.0f);
        mu_input_frame_begin(&s, 2 * FRAME_NS);
        mu_input_resolve(&s, &m, &act, s.dt);
        CHECK(s.mouse_dx == 5.0f && s.mouse_dy == -3.0f);
        CHECK(act.mouse_dx == 5.0f && act.mouse_dy == -3.0f); /* actions-only */
        CHECK(act.wheel_y == 1.0f);
    }

    /* ---- 17. clear_pressed_frame: press fires on exactly one tick ----- */
    {
        MuInputState s;
        MuInputMap m;
        MuInputFrame f;
        enum { ACT_J, ACT_COUNT4 };
        mu_input_init(&s);
        mu_input_key_map_identity(&s);
        mu_input_map_init(&m, ACT_COUNT4);
        CHECK(mu_input_bind_key(&m, ACT_J, &s, MU_KEY_SPACE));
        mu_input_frame_reset(&f);
        mu_input_push_control(&s, MU_KEY_SPACE, true, 0ull);
        mu_input_frame_begin(&s, FRAME_NS);
        mu_input_sample_tick(&s, &m, NULL, &f, 0u);
        CHECK(mu_input_frame_pressed(&f, ACT_J)); /* first tick: fires */
        mu_input_clear_pressed_frame(&s);
        mu_input_sample_tick(&s, &m, NULL, &f, 1u);
        CHECK(!mu_input_frame_pressed(&f, ACT_J)); /* second tick: consumed */
        CHECK(mu_input_frame_down(&f, ACT_J));     /* still held */
        CHECK(f.held_ticks[ACT_J] == 2u);
    }

    puts("mu_input edges, tap capture, wheel, focus, maps, deadzone, repeat: all passed");
    puts("mu_input simple key/mouse API, wheel consume, pad lifecycle, tick/buffered/chord, replay, per-source deadzone: all passed");
    return 0;
}
