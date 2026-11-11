/* canvas.h — coloured triangles from the CPU, for demos whose method runs in C.
 *
 * Those demos have nothing for a shader to work out: the paper's algorithm is
 * in method.c and what it produces is a few thousand triangles, lines and
 * dots a frame. A canvas is that list and the two passes that show it:
 *
 *     Draw -> Present
 *
 *     canvas_open(d, &a->canvas, 60000u);            (in app_init)
 *     canvas_view_orbit(d, &a->canvas, &a->orbit);   (each frame; or canvas_view_flat)
 *     canvas_lit(&a->canvas, p0, p1, p2, CANVAS_RGB(210, 120, 60));
 *     canvas_line(&a->canvas, p, q, 0.01f, CANVAS_RGB(20, 20, 20));
 *     canvas_frame(d, &a->canvas);
 *
 * Colours are the bytes the screen shows. Depth is tested, so order does not
 * matter in three dimensions; in a flat view a larger z is in front.
 */
#ifndef MU_CANVAS_H
#define MU_CANVAS_H
#include "demo.h"

#define CANVAS_RGB(r, g, b) ((uint32_t)(r) | (uint32_t)(g) << 8 | (uint32_t)(b) << 16 | 0xff000000u)

typedef struct CanvasVertex {
    float    pos[3];
    uint32_t rgba;
} CanvasVertex;

typedef struct Canvas {
    CanvasVertex *vertices;
    uint32_t      count, capacity;
    float         vp[16];                    /* column-major                    */
    float         eye[3], right[3], up[3];   /* of the camera, for lines, dots and lighting */
    bool          flat;                      /* a flat view has no eye          */
    BufSlice      slice;
    BufUse        use;
    AttachDesc    depth;
} Canvas;

/* The vertex buffer, the two passes and demo->program. `background` is a CANVAS_RGB. */
void canvas_open(Demo *d, Canvas *c, uint32_t max_vertices, uint32_t background);

/* The camera of this frame: an orbit the mouse turns, or a flat view of the
 * plane z = 0 centred on (cx, cy) that shows `half_height` above and below. */
void canvas_view_orbit(Demo *d, Canvas *c, DemoOrbit *orbit);
void canvas_view_flat(Demo *d, Canvas *c, float cx, float cy, float half_height);

/* A triangle as given; one shaded by its normal (either side); a line `width`
 * wide and a dot of `radius`, both facing the camera. A list that is full
 * drops what is added. */
void canvas_triangle(Canvas *c, const float p0[3], const float p1[3], const float p2[3], uint32_t rgba);
void canvas_lit(Canvas *c, const float p0[3], const float p1[3], const float p2[3], uint32_t rgba);
void canvas_line(Canvas *c, const float p[3], const float q[3], float width, uint32_t rgba);
void canvas_dot(Canvas *c, const float p[3], float radius, uint32_t rgba);

/* Upload the list, record the frame, empty the list. */
void canvas_frame(Demo *d, Canvas *c);
#endif /* MU_CANVAS_H */
