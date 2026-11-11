/* simple_input_example.c — the quick-check API, no MuInputMap needed.
 *
 * Build (core only, no window):
 *   gcc -std=c99 -O2 -Wall -Wextra -I mu_lib/mu simple_input_example.c -o simple -lm
 *
 * In a real app with the GLFW backend, the platform fills the MuKey->slot
 * map for you and you can use platform_key_down(app, MU_KEY_W) instead
 * (see the #if 0 block at the bottom).
 */
#include "mu_input.h"

#include <stdio.h>

int main(void) {
    MuInputState in;
    uint64_t t = 1000000000ull;

    mu_input_init(&in);
    /* Tests / slot==enum backends only. A real backend (platform.c) maps
     * each MuKey to its scancode once at startup — you never call this. */
    mu_input_key_map_identity(&in);

    /* Simulate: W pressed, then held, then released. */
    mu_input_push_control(&in, MU_KEY_W, true, t);
    mu_input_frame_begin(&in, t);
    printf("frame 1: key_down(W)=%d key_pressed(W)=%d\n",
           mu_input_key_down(&in, MU_KEY_W),
           mu_input_key_pressed(&in, MU_KEY_W)); /* 1 1 (edge) */

    mu_input_frame_begin(&in, t + 16666667ull);
    printf("frame 2: key_down(W)=%d key_pressed(W)=%d\n",
           mu_input_key_down(&in, MU_KEY_W),
           mu_input_key_pressed(&in, MU_KEY_W)); /* 1 0 (held, no re-fire) */

    mu_input_push_control(&in, MU_KEY_W, false, t);
    mu_input_frame_begin(&in, t + 2 * 16666667ull);
    printf("frame 3: key_down(W)=%d key_released(W)=%d\n",
           mu_input_key_down(&in, MU_KEY_W),
           mu_input_key_released(&in, MU_KEY_W)); /* 0 1 */

    /* Mouse by enum — no slot numbers in game code. */
    mu_input_push_control(&in, MU_INPUT_MOUSE_SLOT_OF(1u), true, t); /* right */
    mu_input_frame_begin(&in, t + 3 * 16666667ull);
    printf("frame 4: mouse_down(RIGHT)=%d mouse_pressed(RIGHT)=%d\n",
           mu_input_mouse_down(&in, MU_MOUSE_RIGHT),
           mu_input_mouse_pressed(&in, MU_MOUSE_RIGHT)); /* 1 1 */

    /* Tap inside one frame: down=false but pressed=true (exactly once). */
    mu_input_push_control(&in, MU_KEY_C, true, t);
    mu_input_push_control(&in, MU_KEY_C, false, t);
    mu_input_frame_begin(&in, t + 4 * 16666667ull);
    printf("frame 5: key_down(C)=%d key_pressed(C)=%d (tap)\n",
           mu_input_key_down(&in, MU_KEY_C),
           mu_input_key_pressed(&in, MU_KEY_C)); /* 0 1 */

    return 0;
}

#if 0
/* ---- with the GLFW backend (new_apiexp/demos/common/platform.h) ---- */
/* #include "platform.h" */
/* Platform *app = platform_create(1280, 720, "Demo"); */
/* while (!platform_should_close(app)) { */
/*     platform_poll(app); */
/*     if (platform_key_pressed(app, MU_KEY_ESCAPE)) break; */
/*     if (platform_key_down(app, MU_KEY_W)) move_forward(); */
/*     if (platform_mouse_down(app, MU_MOUSE_RIGHT)) drag(); */
/*     if (platform_mouse_pressed(app, MU_MOUSE_LEFT)) click(); */
/* } */
/* platform_destroy(app); */
#endif
