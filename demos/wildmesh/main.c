/* wildmesh — simplifying a mesh that is not one clean surface.
 *
 *   Draw -> Present          (the method runs in C: method.c)
 *
 * Hsueh-Ti Derek Liu, Xiaoting Zhang, Cem Yuksel: "Simplifying Textured Triangle Meshes in
 * the Wild" (ACM Trans. Graph. 2025). Three copies of one mesh, each turning in place: the
 * input, what edge collapses by the quadric error of Garland and Heckbert leave of it, and
 * what the paper's method leaves, at the same number of triangles. The number steps down
 * from a half to a fiftieth. Blue lines on the input are the paper's virtual edges. The
 * colours of a result are the input's, looked up through the paper's map (section 4.3.1).
 * --baseline 2 makes the middle copy the paper's mesh as well, but coloured from the
 * nearest point of the input: that is the comparison to look at for the cloth.
 *
 *   scene 0   a small town made the way such models are: a ground sheet, houses of six
 *             separate sheets, lamps and balls that stand on things without being joined to
 *             them, trees of three sheets that meet in one line
 *   scene 1   two blocks as a triangle soup: no two triangles share a vertex
 *   scene 2   a thin cloth, yellow above and blue below
 *   scene 3   a closed surface (a knobbly torus), where both methods must agree
 *
 * --check: the quadrics against their definitions, the two methods on the closed surface,
 * what is left of the ground sheet, how many parts the virtual edges leave, the distance
 * between input and result, the two ways to add up the area term on the soup, and for the
 * cloth how often a point of the result gets its colour from the wrong side.
 */
#include "canvas.h"
#include "method.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PI     3.14159265358979323846
#define LEVELS 5

typedef struct Shape {
    int       vertices, faces, vertex_room, face_room;
    double   *x;
    int      *face;
    uint32_t *colour;   /* a face */
} Shape;

typedef struct Result {
    int       faces, pieces, virtual_edges, parts, joined;
    double   *corner;   /* 9 a face */
    int      *origin;   /* the face's number in the input */
    float    *piece;    /* 9 a piece: the faces cut up, each piece with the input's colour where it comes from */
    uint32_t *rgba;
    double    seconds;
} Result;

typedef struct App {
    Demo      demo;   /* first: see demo.h */
    Canvas    canvas;
    DemoOrbit orbit;
    Shape     shape;
    Result    result[2][LEVELS];
    Wm       *joined;   /* the input with the paper's virtual edges, for drawing them */
    uint32_t  scene, keep, virtual_edges, area, baseline, built_scene, built_virtual, built_area, built_baseline;
    float     reach, built_reach, clock, middle[3], scale;
    bool      still, wire, no_edges;
} App;

static const double k_keep[LEVELS] = {0.5, 0.25, 0.1, 0.05, 0.02};

static double uniform(uint64_t *state) {
    *state = *state * 6364136223846793005ull + 1442695040888963407ull;
    return (double)(*state >> 11) / 9007199254740992.0;
}

static int shape_vertex(Shape *s, double x, double y, double z) {
    if (s->vertices == s->vertex_room) { s->vertex_room = s->vertex_room ? 2 * s->vertex_room : 1024; s->x = realloc(s->x, (size_t)s->vertex_room * 3 * sizeof(double)); }
    s->x[3 * s->vertices] = x; s->x[3 * s->vertices + 1] = y; s->x[3 * s->vertices + 2] = z;
    return s->vertices++;
}

static void shape_face(Shape *s, int a, int b, int c, uint32_t colour) {
    if (s->faces == s->face_room) {
        s->face_room = s->face_room ? 2 * s->face_room : 1024;
        s->face = realloc(s->face, (size_t)s->face_room * 3 * sizeof(int)); s->colour = realloc(s->colour, (size_t)s->face_room * sizeof(uint32_t));
    }
    s->face[3 * s->faces] = a; s->face[3 * s->faces + 1] = b; s->face[3 * s->faces + 2] = c; s->colour[s->faces++] = colour;
}

/* A flat sheet o + u s + v t of nu by nv squares, chequered in two colours. */
static void shape_sheet(Shape *s, double ox, double oy, double oz, double ux, double uy, double uz, double vx, double vy, double vz, int nu, int nv, uint32_t first, uint32_t second) {
    const int base = s->vertices;
    int       i, j;
    for (j = 0; j <= nv; ++j)
        for (i = 0; i <= nu; ++i) shape_vertex(s, ox + ux * i / nu + vx * j / nv, oy + uy * i / nu + vy * j / nv, oz + uz * i / nu + vz * j / nv);
    for (j = 0; j < nv; ++j)
        for (i = 0; i < nu; ++i) {
            const int      a = base + j * (nu + 1) + i;
            const uint32_t colour = (i + j) % 2 ? second : first;
            shape_face(s, a, a + 1, a + nu + 2, colour); shape_face(s, a, a + nu + 2, a + nu + 1, colour);
        }
}

/* A box of six sheets that share no vertex. */
static void shape_box(Shape *s, double x0, double y0, double z0, double x1, double y1, double z1, int n, uint32_t first, uint32_t second) {
    shape_sheet(s, x0, y0, z0, x1 - x0, 0, 0, 0, 0, z1 - z0, n, n, first, second); shape_sheet(s, x0, y1, z0, 0, 0, z1 - z0, x1 - x0, 0, 0, n, n, first, second);
    shape_sheet(s, x0, y0, z0, 0, y1 - y0, 0, x1 - x0, 0, 0, n, n, first, second); shape_sheet(s, x0, y0, z1, x1 - x0, 0, 0, 0, y1 - y0, 0, n, n, first, second);
    shape_sheet(s, x0, y0, z0, 0, 0, z1 - z0, 0, y1 - y0, 0, n, n, first, second); shape_sheet(s, x1, y0, z0, 0, y1 - y0, 0, 0, 0, z1 - z0, n, n, first, second);
}

/* Rings round the y axis from angle `from` to `to` above the bottom (0 .. pi: a closed ball), radius r(angle). */
static void shape_ball(Shape *s, double cx, double cy, double cz, double radius, int round, int rings, double from, double to, uint32_t colour) {
    const int base = s->vertices, closed_low = from <= 0.0, closed_high = to >= PI;
    int       i, j;
    for (j = 0; j <= rings; ++j) {
        const double angle = from + (to - from) * j / rings;
        if ((j == 0 && closed_low) || (j == rings && closed_high)) { shape_vertex(s, cx, cy - radius * cos(angle), cz); continue; }
        for (i = 0; i < round; ++i) shape_vertex(s, cx + radius * sin(angle) * cos(2.0 * PI * i / round), cy - radius * cos(angle), cz + radius * sin(angle) * sin(2.0 * PI * i / round));
    }
    for (j = 0; j < rings; ++j)
        for (i = 0; i < round; ++i) {
            const int low = closed_low ? (j == 0 ? base : base + 1 + (j - 1) * round) : base + j * round, high = closed_low ? base + 1 + j * round : base + (j + 1) * round, next = (i + 1) % round;
            if (j == 0 && closed_low) { shape_face(s, low, high + next, high + i, colour); continue; }
            if (j == rings - 1 && closed_high) { shape_face(s, low + i, low + next, high, colour); continue; }
            shape_face(s, low + i, low + next, high + next, colour); shape_face(s, low + i, high + next, high + i, colour);
        }
}

/* An upright tube, open at both ends. */
static void shape_tube(Shape *s, double cx, double cy, double cz, double radius, double height, int round, int rings, uint32_t colour) {
    const int base = s->vertices;
    int       i, j;
    for (j = 0; j <= rings; ++j)
        for (i = 0; i < round; ++i) shape_vertex(s, cx + radius * cos(2.0 * PI * i / round), cy + height * j / rings, cz + radius * sin(2.0 * PI * i / round));
    for (j = 0; j < rings; ++j)
        for (i = 0; i < round; ++i) {
            const int a = base + j * round + i, b = base + j * round + (i + 1) % round;
            shape_face(s, a, b, b + round, colour); shape_face(s, a, b + round, a + round, colour);
        }
}

static void shape_build(Shape *s, uint32_t scene) {
    int i, j, k;
    s->vertices = s->faces = 0;
    if (scene == 0u) {
        static const double k_house[5][5] = {{-1.5, -1.3, 0.9, 0.8, 0.9}, {-0.2, -1.45, 0.7, 0.7, 1.5}, {0.95, -1.2, 0.8, 0.9, 0.6}, {-1.45, 0.35, 0.6, 0.9, 1.1}, {0.8, 0.3, 0.95, 0.6, 0.8}};
        static const uint32_t k_wall[5][2] = {{CANVAS_RGB(200, 120, 90), CANVAS_RGB(215, 140, 105)}, {CANVAS_RGB(110, 140, 200), CANVAS_RGB(130, 160, 215)}, {CANVAS_RGB(205, 185, 110), CANVAS_RGB(220, 200, 130)},
                                              {CANVAS_RGB(160, 110, 170), CANVAS_RGB(180, 130, 190)}, {CANVAS_RGB(120, 180, 160), CANVAS_RGB(140, 200, 180)}};
        shape_sheet(s, -2, 0, -2, 4, 0, 0, 0, 0, 4, 24, 24, CANVAS_RGB(128, 142, 120), CANVAS_RGB(104, 120, 100));
        for (k = 0; k < 5; ++k) shape_box(s, k_house[k][0], 0, k_house[k][1], k_house[k][0] + k_house[k][2], k_house[k][4], k_house[k][1] + k_house[k][3], 6, k_wall[k][0], k_wall[k][1]);
        shape_ball(s, -1.02, 0.9, -0.92, 0.3, 16, 5, 0.5 * PI, PI, CANVAS_RGB(230, 225, 210));        /* a dome on the first house */
        shape_ball(s, 0.13, 1.72, -1.12, 0.22, 16, 10, 0.0, PI, CANVAS_RGB(235, 200, 80));          /* a ball on the second */
        for (k = 0; k < 8; ++k) {
            /* lamps in the middle of ground squares: no ground vertex is near */
            const double x = -2.0 + (2.5 + 3.0 * k) / 6.0, z = k % 2 ? 1.75 : -0.0833333;
            shape_tube(s, x, 0.0, z, 0.015, 0.45, 8, 3, CANVAS_RGB(70, 70, 80));
            shape_ball(s, x, 0.5, z, 0.05, 10, 6, 0.0, PI, CANVAS_RGB(250, 240, 180));
        }
        for (k = 0; k < 4; ++k) {
            /* trees: a trunk, and three sheets that meet in one line */
            const double x = -1.75 + 1.1 * k + 0.0833333, z = 1.25 + 0.0833333;
            int          centre[5], rim[3][5];
            shape_tube(s, x, 0.0, z, 0.03, 0.3, 6, 2, CANVAS_RGB(120, 85, 60));
            for (j = 0; j < 5; ++j) {
                const double y = 0.3 + 0.16 * j, out = j == 0 ? 0.26 : 0.26 * (1.0 - j / 4.6);
                centre[j] = shape_vertex(s, x, y, z);
                for (i = 0; i < 3; ++i) rim[i][j] = shape_vertex(s, x + out * cos(2.0 * PI * i / 3.0 + k), y - 0.05, z + out * sin(2.0 * PI * i / 3.0 + k));
            }
            for (j = 0; j < 4; ++j)
                for (i = 0; i < 3; ++i) {
                    shape_face(s, centre[j], rim[i][j], rim[i][j + 1], CANVAS_RGB(70, 150 + 12 * i, 80));
                    shape_face(s, centre[j], rim[i][j + 1], centre[j + 1], CANVAS_RGB(70, 150 + 12 * i, 80));
                }
        }
    }
    if (scene == 1u) {
        /* two blocks, then every triangle gets its own three vertices */
        Shape whole = {0};
        shape_box(&whole, -1.0, 0.0, -0.7, 1.0, 0.5, 0.7, 10, CANVAS_RGB(200, 150, 100), CANVAS_RGB(215, 170, 120));
        shape_box(&whole, -0.5, 0.5, -0.4, 0.3, 1.2, 0.4, 8, CANVAS_RGB(110, 150, 200), CANVAS_RGB(130, 170, 215));
        for (i = 0; i < whole.faces; ++i) {
            int corner[3];
            for (k = 0; k < 3; ++k) corner[k] = shape_vertex(s, whole.x[3 * whole.face[3 * i + k]], whole.x[3 * whole.face[3 * i + k] + 1], whole.x[3 * whole.face[3 * i + k] + 2]);
            shape_face(s, corner[0], corner[1], corner[2], whole.colour[i]);
        }
        free(whole.x); free(whole.face); free(whole.colour);
    }
    if (scene == 2u) {
        /* a cloth 0.012 thick, closed at its rim: faces 0 .. 3199 are the yellow upper side, the rest the blue lower side */
        enum { N = 40 };
        static int upper[N + 1][N + 1], lower[N + 1][N + 1];
        for (k = 0; k < 2; ++k)
            for (j = 0; j <= N; ++j)
                for (i = 0; i <= N; ++i) {
                    const double x = -1.0 + 2.0 * i / N, z = -1.0 + 2.0 * j / N, y = 0.5 + 0.12 * sin(3.0 * x) * cos(2.5 * z) + 0.05 * sin(7.0 * x + 2.0 * z);
                    const bool   rim = i == 0 || j == 0 || i == N || j == N;
                    if (k == 0) upper[j][i] = shape_vertex(s, x, y + (rim ? 0.0 : 0.006), z);
                    else lower[j][i] = rim ? upper[j][i] : shape_vertex(s, x, y - 0.006, z);
                }
        for (k = 0; k < 2; ++k)
            for (j = 0; j < N; ++j)
                for (i = 0; i < N; ++i) {
                    int (*v)[N + 1] = k ? lower : upper;
                    const uint32_t colour = k ? ((i / 4 + j / 4) % 2 ? CANVAS_RGB(60, 90, 200) : CANVAS_RGB(80, 115, 225)) : ((i / 4 + j / 4) % 2 ? CANVAS_RGB(235, 200, 70) : CANVAS_RGB(245, 220, 100));
                    /* the upper side is wound to face up, the lower side to face down */
                    if (k == 0) { shape_face(s, v[j][i], v[j + 1][i + 1], v[j][i + 1], colour); shape_face(s, v[j][i], v[j + 1][i], v[j + 1][i + 1], colour); }
                    else { shape_face(s, v[j][i], v[j][i + 1], v[j + 1][i + 1], colour); shape_face(s, v[j][i], v[j + 1][i + 1], v[j + 1][i], colour); }
                }
    }
    if (scene == 3u) {
        /* a torus with knobs, closed and without borders */
        enum { NU = 64, NV = 28 };
        for (j = 0; j < NV; ++j)
            for (i = 0; i < NU; ++i) {
                const double u = 2.0 * PI * i / NU, v = 2.0 * PI * j / NV, tube = 0.35 + 0.08 * sin(5.0 * u) * sin(3.0 * v) + 0.03 * cos(9.0 * u + v);
                shape_vertex(s, (1.0 + tube * cos(v)) * cos(u), 0.6 + tube * sin(v), (1.0 + tube * cos(v)) * sin(u));
            }
        for (j = 0; j < NV; ++j)
            for (i = 0; i < NU; ++i) {
                const int      a = j * NU + i, b = j * NU + (i + 1) % NU, c = ((j + 1) % NV) * NU + (i + 1) % NU, e = ((j + 1) % NV) * NU + i;
                const uint32_t colour = (i / 4 + j / 4) % 2 ? CANVAS_RGB(210, 120, 110) : CANVAS_RGB(235, 215, 200);
                shape_face(s, a, b, c, colour); shape_face(s, a, c, e, colour);
            }
    }
}

/* The diagonal of the box round a shape. */
static double shape_size(const Shape *s) {
    double low[3] = {1e300, 1e300, 1e300}, high[3] = {-1e300, -1e300, -1e300};
    int    v, k;
    for (v = 0; v < s->vertices; ++v)
        for (k = 0; k < 3; ++k) { low[k] = fmin(low[k], s->x[3 * v + k]); high[k] = fmax(high[k], s->x[3 * v + k]); }
    return sqrt((high[0] - low[0]) * (high[0] - low[0]) + (high[1] - low[1]) * (high[1] - low[1]) + (high[2] - low[2]) * (high[2] - low[2]));
}

/* Collapses edges until `target` faces are left; the result's faces, and with pieces > 0 its colours
 * (about `pieces` of them in all) taken from the input through the collapses. */
static void simplify(const Shape *s, int virtual_edges, int area, double reach, int target, int pieces, bool nearest, Result *r) {
    const clock_t start = clock();
    Wm           *w = wm_open(s->vertices, s->x, s->faces, s->face, virtual_edges, area, reach);
    int           f, k, n = 0, cut, i, j, m = 0, *from;
    double       *point, *where;

    while (w->faces_left > target && wm_collapse(w)) {}
    r->seconds = (double)(clock() - start) / CLOCKS_PER_SEC;
    r->faces = w->faces_left; r->virtual_edges = w->virtual_edges; r->parts = w->parts; r->joined = w->joined;
    r->corner = realloc(r->corner, (size_t)r->faces * 9 * sizeof(double) + 8); r->origin = realloc(r->origin, (size_t)r->faces * sizeof(int) + 8);
    for (f = 0; f < w->faces; ++f) {
        if (!w->alive[f]) continue;
        for (k = 0; k < 3; ++k) memcpy(r->corner + 9 * n + 3 * k, w->x + 3 * w->face[3 * f + k], 3 * sizeof(double));
        r->origin[n++] = f;
    }
    r->pieces = 0;
    if (pieces > 0 && r->faces > 0) {
        /* every face cut into cut * cut small triangles; the middle of each is taken back to the input */
        cut = (int)fmin(fmax(sqrt((double)pieces / r->faces), 1.0), 8.0);
        r->pieces = r->faces * cut * cut;
        r->piece = realloc(r->piece, (size_t)r->pieces * 9 * sizeof(float)); r->rgba = realloc(r->rgba, (size_t)r->pieces * sizeof(uint32_t));
        point = malloc((size_t)r->pieces * 3 * sizeof(double)); where = malloc((size_t)r->pieces * 3 * sizeof(double)); from = malloc((size_t)r->pieces * sizeof(int));
        for (f = 0; f < r->faces; ++f)
            for (i = 0; i < cut; ++i)
                for (j = 0; j < 2 * (cut - i) - 1; ++j, ++m) {
                    /* row i holds cut - i upright triangles and one fewer upside down */
                    const int    col = j / 2, down = j % 2;
                    const double bary[3][2] = {{(double)(col + down) / cut, (double)i / cut}, {(double)(col + 1) / cut, (double)(i + down) / cut}, {(double)col / cut, (double)(i + 1) / cut}};
                    const double *c = r->corner + 9 * f;
                    for (k = 0; k < 3; ++k) point[3 * m + k] = 0.0;
                    for (k = 0; k < 9; ++k) {
                        const double value = c[k % 3] + (c[3 + k % 3] - c[k % 3]) * bary[k / 3][0] + (c[6 + k % 3] - c[k % 3]) * bary[k / 3][1];
                        r->piece[9 * m + k] = (float)value; point[3 * m + k % 3] += value / 3.0;
                    }
                    from[m] = r->origin[f];
                }
        wm_transfer(w, r->pieces, point, from, where);
        for (m = 0; m < (nearest ? r->pieces : 0); ++m) {
            /* instead: the nearest point of the whole input, which is what the paper's figure 17 compares with */
            double least = 1e300;
            for (f = 0; f < s->faces; ++f) {
                const double *a = s->x + 3 * s->face[3 * f], *b = s->x + 3 * s->face[3 * f + 1], *c = s->x + 3 * s->face[3 * f + 2], *q = point + 3 * m;
                /* no point of the triangle is nearer than its first corner less its two sides from there */
                const double  near = sqrt((q[0] - a[0]) * (q[0] - a[0]) + (q[1] - a[1]) * (q[1] - a[1]) + (q[2] - a[2]) * (q[2] - a[2])) -
                                     fmax(sqrt((b[0] - a[0]) * (b[0] - a[0]) + (b[1] - a[1]) * (b[1] - a[1]) + (b[2] - a[2]) * (b[2] - a[2])), sqrt((c[0] - a[0]) * (c[0] - a[0]) + (c[1] - a[1]) * (c[1] - a[1]) + (c[2] - a[2]) * (c[2] - a[2])));
                const double  distance = near > 0.0 && near * near >= least ? 1e300 : wm_closest(q, a, b, c, where);
                if (distance < least) { least = distance; from[m] = f; }
            }
        }
        for (m = 0; m < r->pieces; ++m) r->rgba[m] = s->colour[from[m]];
        free(point); free(where); free(from);
    }
    wm_close(w);
}

static void app_build(App *a) {
    double low[3] = {1e300, 1e300, 1e300}, high[3] = {-1e300, -1e300, -1e300};
    int    method, level, v, k;
    shape_build(&a->shape, a->scene);
    for (v = 0; v < a->shape.vertices; ++v)
        for (k = 0; k < 3; ++k) { low[k] = fmin(low[k], a->shape.x[3 * v + k]); high[k] = fmax(high[k], a->shape.x[3 * v + k]); }
    a->middle[0] = (float)(0.5 * (low[0] + high[0])); a->middle[1] = (float)low[1]; a->middle[2] = (float)(0.5 * (low[2] + high[2]));
    a->scale = (float)(4.0 / fmax(high[0] - low[0], high[2] - low[2]));
    for (method = 0; method < 2; ++method)
        for (level = 0; level < LEVELS; ++level)
            simplify(&a->shape, method || a->baseline == 2u ? (int)a->virtual_edges : (int)a->baseline, method || a->baseline == 2u ? (int)a->area : (int)a->baseline, a->reach * shape_size(&a->shape),
                     (int)(k_keep[level] * a->shape.faces), 9000, !method && a->baseline == 2u, &a->result[method][level]);
    if (a->joined) wm_close(a->joined);
    a->joined = wm_open(a->shape.vertices, a->shape.x, a->shape.faces, a->shape.face, 2, 2, a->reach * shape_size(&a->shape));
    a->built_scene = a->scene; a->built_virtual = a->virtual_edges; a->built_area = a->area; a->built_reach = a->reach; a->built_baseline = a->baseline;
}

static void app_ui(App *a, struct nk_context *ctx) {
    static const char *const k_scenes[] = {"town", "soup of two blocks", "thin cloth", "closed surface"};
    static const char *const k_keeps[] = {"steps down", "50 %", "25 %", "10 %", "5 %", "2 %"};
    static const char *const k_virtual[] = {"none", "near vertices", "near triangles (paper)"};
    static const char *const k_area[] = {"none", "summed", "anew each time (paper)"};
    static const char *const k_baseline[] = {"plane term only", "near vertices + summed area term", "as the right, colours from nearest point"};
    const Result            *r = &a->result[1][a->keep ? a->keep - 1u : (uint32_t)(a->clock / 2.5f) % LEVELS];
    if (nk_begin(ctx, "Mesh simplification", nk_rect(16, 16, 330, 330), NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_TITLE)) {
        demo_ui_text(ctx, "%d faces -> %d; %d parts, %d virtual edges", a->shape.faces, r->faces, r->parts, r->virtual_edges);
        demo_ui_choice(ctx, "Scene", k_scenes, 4u, &a->scene);
        demo_ui_choice(ctx, "Faces kept", k_keeps, 6u, &a->keep);
        demo_ui_choice(ctx, "Middle", k_baseline, 3u, &a->baseline);
        demo_ui_choice(ctx, "Right: virtual edges", k_virtual, 3u, &a->virtual_edges);
        demo_ui_choice(ctx, "Right: area term", k_area, 3u, &a->area);
        demo_ui_float(ctx, "Reach r / size", &a->reach, 0.001f, 0.03f);
        demo_ui_bool(ctx, "Edges of the triangles", &a->wire);
        demo_ui_bool(ctx, "Still", &a->still);
    }
    nk_end(ctx);
}

/* A point of the mesh as copy 0, 1 or 2 shows it: each copy turns about its own middle. */
static void app_place(const App *a, double x, double y, double z, int copy, float p[3]) {
    const float angle = 0.6f + 0.3f * a->clock, px = ((float)x - a->middle[0]) * a->scale, pz = ((float)z - a->middle[2]) * a->scale;
    p[0] = cosf(angle) * px + sinf(angle) * pz + 6.2f * (float)(copy - 1); p[1] = ((float)y - a->middle[1]) * a->scale; p[2] = -sinf(angle) * px + cosf(angle) * pz;
}

static void app_frame(App *a) {
    Demo          *d = &a->demo;
    Canvas        *c = &a->canvas;
    const uint32_t level = a->keep ? a->keep - 1u : (uint32_t)(a->clock / 2.5f) % LEVELS;
    const Shape   *s = &a->shape;
    float          p[3][3];
    int            f, k, copy;

    if (a->scene != a->built_scene || a->virtual_edges != a->built_virtual || a->area != a->built_area || a->reach != a->built_reach || a->baseline != a->built_baseline) app_build(a);
    a->clock += a->still ? 0.0f : 1.0f / 60.0f;
    canvas_view_orbit(d, c, &a->orbit);
    for (f = 0; f < s->faces; ++f) {
        for (k = 0; k < 3; ++k) app_place(a, s->x[3 * s->face[3 * f + k]], s->x[3 * s->face[3 * f + k] + 1], s->x[3 * s->face[3 * f + k] + 2], 0, p[k]);
        canvas_lit(c, p[0], p[1], p[2], s->colour[f]);
    }
    for (k = 0; k < (a->no_edges ? 0 : a->joined->virtual_edges) && k < 6000; ++k) {
        /* the virtual edges come after those of the faces */
        const int *end = a->joined->end + 2 * (a->joined->edges - 1 - k);
        app_place(a, s->x[3 * end[0]], s->x[3 * end[0] + 1], s->x[3 * end[0] + 2], 0, p[0]); app_place(a, s->x[3 * end[1]], s->x[3 * end[1] + 1], s->x[3 * end[1] + 2], 0, p[1]);
        canvas_line(c, p[0], p[1], 0.02f, CANVAS_RGB(60, 130, 255));
    }
    for (copy = 1; copy <= 2; ++copy) {
        const Result *r = &a->result[copy - 1][level];
        for (f = 0; f < r->pieces; ++f) {
            for (k = 0; k < 3; ++k) app_place(a, r->piece[9 * f + 3 * k], r->piece[9 * f + 3 * k + 1], r->piece[9 * f + 3 * k + 2], copy, p[k]);
            canvas_lit(c, p[0], p[1], p[2], r->rgba[f]);
        }
        for (f = 0; f < (a->wire ? r->faces : 0); ++f)
            for (k = 0; k < 3; ++k) {
                const double *from = r->corner + 9 * f + 3 * k, *to = r->corner + 9 * f + 3 * ((k + 1) % 3);
                app_place(a, from[0], from[1], from[2], copy, p[0]); app_place(a, to[0], to[1], to[2], copy, p[1]);
                canvas_line(c, p[0], p[1], 0.012f, CANVAS_RGB(20, 22, 28));
            }
    }
    canvas_frame(d, c);
}

/* From `count` points spread evenly over triangles `from` (9 doubles each) to the nearest point of triangles
 * `to`: the largest distance and the mean squared distance. */
static void distance_between(const double *from, int from_count, const double *to, int to_count, int count, uint64_t *state, double *largest, double *mean_square) {
    double *sum = malloc((size_t)(from_count + 1) * sizeof(double)), q[3];
    int     f, i, k;
    sum[0] = 0.0;
    for (f = 0; f < from_count; ++f) {
        const double *t = from + 9 * f, u[3] = {t[3] - t[0], t[4] - t[1], t[5] - t[2]}, v[3] = {t[6] - t[0], t[7] - t[1], t[8] - t[2]};
        const double n[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
        sum[f + 1] = sum[f] + 0.5 * sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    }
    *largest = 0.0; *mean_square = 0.0;
    for (i = 0; i < count; ++i) {
        const double pick = uniform(state) * sum[from_count], root = sqrt(uniform(state)), other = uniform(state);
        double       p[3], least = 1e300;
        int          low = 0, high = from_count;
        while (high - low > 1) {
            const int middle = (low + high) / 2;
            if (sum[middle] <= pick) low = middle; else high = middle;
        }
        for (k = 0; k < 3; ++k) p[k] = from[9 * low + k] * (1.0 - root) + from[9 * low + 3 + k] * root * (1.0 - other) + from[9 * low + 6 + k] * root * other;
        for (f = 0; f < to_count; ++f) least = fmin(least, wm_closest(p, to + 9 * f, to + 9 * f + 3, to + 9 * f + 6, q));
        *largest = fmax(*largest, sqrt(least)); *mean_square += least / count;
    }
    free(sum);
}

static void app_check(App *a) {
    Demo    *d = &a->demo;
    Shape    s = {0};
    Result   r[3] = {{0}};
    uint64_t state = 2025u;
    double  *input = NULL;
    char     label[220];
    int      f, i, k, v, e, level, method;

    /* 1: the quadrics against what they stand for, on the town */
    shape_build(&s, 0u);
    {
        Wm    *w = wm_open(s.vertices, s.x, s.faces, s.face, 0, 1, 0.0);
        double worst_plane = 0.0, worst_area = 0.0;
        int    borders = 0;
        for (i = 0; i < 400; ++i) {
            const double *q, x[3] = {4.0 * uniform(&state) - 2.0, 2.0 * uniform(&state), 4.0 * uniform(&state) - 2.0};
            double        direct = 0.0, area = 0.0, value;
            v = (int)(uniform(&state) * s.vertices);
            for (k = 0; k < w->vertex_faces[v].count; ++k) {
                /* eq. 9: a third of each face's area times the squared distance from its plane */
                const int    *t = s.face + 3 * w->vertex_faces[v].item[k];
                const double *p0 = s.x + 3 * t[0], *p1 = s.x + 3 * t[1], *p2 = s.x + 3 * t[2];
                const double  n[3] = {(p1[1] - p0[1]) * (p2[2] - p0[2]) - (p1[2] - p0[2]) * (p2[1] - p0[1]), (p1[2] - p0[2]) * (p2[0] - p0[0]) - (p1[0] - p0[0]) * (p2[2] - p0[2]),
                                      (p1[0] - p0[0]) * (p2[1] - p0[1]) - (p1[1] - p0[1]) * (p2[0] - p0[0])};
                const double  twice = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]), away = (n[0] * (x[0] - p0[0]) + n[1] * (x[1] - p0[1]) + n[2] * (x[2] - p0[2])) / twice;
                direct += twice / 6.0 * away * away;
            }
            for (k = 0; k < w->vertex_edges[v].count; ++k) {
                /* eq. 15: for each border edge at the vertex, twice the squared area of the triangle it makes with x */
                const int     g = w->vertex_edges[v].item[k], other = w->end[2 * g] == v ? w->end[2 * g + 1] : w->end[2 * g];
                const double *p0 = s.x + 3 * v, *p1 = s.x + 3 * other;
                const double  n[3] = {(p0[1] - x[1]) * (p1[2] - x[2]) - (p0[2] - x[2]) * (p1[1] - x[1]), (p0[2] - x[2]) * (p1[0] - x[0]) - (p0[0] - x[0]) * (p1[2] - x[2]),
                                      (p0[0] - x[0]) * (p1[1] - x[1]) - (p0[1] - x[1]) * (p1[0] - x[0])};
                int           shared = 0, m;
                for (m = 0; m < w->vertex_faces[v].count; ++m) {
                    const int *t = s.face + 3 * w->vertex_faces[v].item[m];
                    shared += t[0] == other || t[1] == other || t[2] == other;
                }
                if (shared != 1) continue;
                area += 0.5 * (n[0] * n[0] + n[1] * n[1] + n[2] * n[2]); borders += 1;
            }
            q = w->quadric + 10 * v;
            value = x[0] * (q[0] * x[0] + 2.0 * q[1] * x[1] + 2.0 * q[2] * x[2]) + x[1] * (q[3] * x[1] + 2.0 * q[4] * x[2]) + q[5] * x[2] * x[2] + 2.0 * (q[6] * x[0] + q[7] * x[1] + q[8] * x[2]) + q[9];
            worst_plane = fmax(worst_plane, fabs(value - direct) / (direct + 1e-12));
            q = w->area_quadric + 10 * v;
            value = x[0] * (q[0] * x[0] + 2.0 * q[1] * x[1] + 2.0 * q[2] * x[2]) + x[1] * (q[3] * x[1] + 2.0 * q[4] * x[2]) + q[5] * x[2] * x[2] + 2.0 * (q[6] * x[0] + q[7] * x[1] + q[8] * x[2]) + q[9];
            worst_area = fmax(worst_area, fabs(value - area) / (area + 1e-12));
        }
        demo_check(d, "town, 400 vertices and points: plane quadric against the sum of area / 3 * distance^2 (eq. 9), largest relative difference", worst_plane, "< 1e-9", worst_plane < 1e-9);
        demo_check(d, "  border edges met", borders, "> 100", borders > 100);
        demo_check(d, "  area quadric against the sum of 2 * (area of the triangle with a border edge)^2 (eq. 15)", worst_area, "< 1e-9", worst_area < 1e-9);
        wm_close(w);
    }

    /* 2: a closed surface: the paper's method is Garland and Heckbert's */
    shape_build(&s, 3u);
    simplify(&s, 0, 0, 0.0, s.faces / 10, 0, false, &r[0]);
    simplify(&s, 2, 2, 0.005 * shape_size(&s), s.faces / 10, 0, false, &r[1]);
    {
        double differ = 0.0;
        for (i = 0; i < 9 * (r[0].faces < r[1].faces ? r[0].faces : r[1].faces); ++i) differ = fmax(differ, fabs(r[0].corner[i] - r[1].corner[i]));
        snprintf(label, sizeof label, "closed surface, %d faces to a tenth: parts", s.faces);
        demo_check(d, label, r[1].parts, "= 1", r[1].parts == 1);
        demo_check(d, "  virtual edges", r[1].virtual_edges, "= 0", r[1].virtual_edges == 0);
        demo_check(d, "  faces left by Garland and Heckbert's cost", r[0].faces, "info", true);
        demo_check(d, "  faces left by the paper's", r[1].faces, "the same", r[1].faces == r[0].faces);
        demo_check(d, "  largest difference between the two results' coordinates", differ, "= 0", differ == 0.0);
    }

    /* 3: the town: parts, the ground sheet, distances */
    shape_build(&s, 0u);
    input = realloc(input, (size_t)s.faces * 9 * sizeof(double));
    for (f = 0; f < s.faces; ++f)
        for (k = 0; k < 3; ++k) memcpy(input + 9 * f + 3 * k, s.x + 3 * s.face[3 * f + k], 3 * sizeof(double));
    simplify(&s, 1, 2, 0.005 * shape_size(&s), s.faces, 0, false, &r[0]);
    simplify(&s, 2, 2, 0.005 * shape_size(&s), s.faces, 0, false, &r[1]);
    snprintf(label, sizeof label, "town, %d faces: connected parts", s.faces);
    demo_check(d, label, r[1].parts, "> 50", r[1].parts > 50);
    demo_check(d, "  parts once vertices nearer than 2 r are joined (r = 0.005 of the size)", r[0].joined, "fewer", r[0].joined < r[0].parts);
    demo_check(d, "  virtual edges for that (info)", r[0].virtual_edges, "any", true);
    demo_check(d, "  parts once triangles nearer than 2 r are joined (the paper's)", r[1].joined, "= 1", r[1].joined == 1);
    demo_check(d, "  virtual edges for that (info)", r[1].virtual_edges, "any", true);
    for (level = 0; level < 2; ++level) {
        const int target = level ? s.faces / 50 : s.faces / 10;
        double    ground[2] = {0}, far[2][2], mean[2][2], lamps[2] = {0}, unused[3];
        for (method = 0; method < 2; ++method) {
            simplify(&s, method ? 2 : 0, method ? 2 : 0, 0.005 * shape_size(&s), target, 0, false, &r[method]);
            for (f = 0; f < r[method].faces; ++f) {
                /* the eight lamps are 148 faces each, after the ground (1152), the houses (432 each), the dome (144) and the ball (288) */
                lamps[method] += r[method].origin[f] >= 1152 + 5 * 432 + 144 + 288 && r[method].origin[f] < 1152 + 5 * 432 + 144 + 288 + 8 * 148;
            }
            for (i = 0; i < 1000; ++i) {
                /* a point of the ground sheet (y = 0, 4 by 4): is the result within 0.01 of the size of it */
                const double point[3] = {4.0 * uniform(&state) - 2.0, 0.0, 4.0 * uniform(&state) - 2.0};
                double       least = 1e300;
                for (f = 0; f < r[method].faces; ++f) least = fmin(least, wm_closest(point, r[method].corner + 9 * f, r[method].corner + 9 * f + 3, r[method].corner + 9 * f + 6, unused));
                ground[method] += 0.1 * (sqrt(least) < 0.01 * shape_size(&s));
            }
            distance_between(input, s.faces, r[method].corner, r[method].faces, 2500, &state, &far[method][0], &mean[method][0]);
            distance_between(r[method].corner, r[method].faces, input, s.faces, 2500, &state, &far[method][1], &mean[method][1]);
        }
        snprintf(label, sizeof label, "town to %d faces: per cent of the ground sheet that the result still covers (within 0.01 of the size), Garland and Heckbert's cost", target);
        demo_check(d, label, ground[0], "info", true);
        demo_check(d, "  the paper's", ground[1], "> 95", ground[1] > 95.0);
        demo_check(d, "  faces left that were lamps, Garland and Heckbert's cost (info)", lamps[0], "any", true);
        demo_check(d, "  the paper's (info)", lamps[1], "any", true);
        demo_check(d, "  largest distance input to result / size, Garland and Heckbert's cost", far[0][0] / shape_size(&s), "info", true);
        demo_check(d, "  the paper's", far[1][0] / shape_size(&s), "smaller", far[1][0] < far[0][0]);
        demo_check(d, "  largest distance result to input / size, Garland and Heckbert's cost (info)", far[0][1] / shape_size(&s), "any", true);
        demo_check(d, "  the paper's (info)", far[1][1] / shape_size(&s), "any", true);
        demo_check(d, "  mean squared distance, both ways added, / size^2, Garland and Heckbert's cost", (mean[0][0] + mean[0][1]) / (shape_size(&s) * shape_size(&s)), "info", true);
        demo_check(d, "  the paper's", (mean[1][0] + mean[1][1]) / (shape_size(&s) * shape_size(&s)), "smaller", mean[1][0] + mean[1][1] < mean[0][0] + mean[0][1]);
        demo_check(d, "  seconds for the paper's, with the search for near triangles (info)", r[1].seconds, "any", true);
    }

    /* 4: the soup: how the area term is added up */
    shape_build(&s, 1u);
    input = realloc(input, (size_t)s.faces * 9 * sizeof(double));
    for (f = 0; f < s.faces; ++f)
        for (k = 0; k < 3; ++k) memcpy(input + 9 * f + 3 * k, s.x + 3 * s.face[3 * f + k], 3 * sizeof(double));
    for (level = 0; level < 2; ++level) {
        const int target = level ? s.faces / 50 : s.faces / 10;
        double    far[3][2], mean[3][2];
        for (method = 0; method < 3; ++method) {
            simplify(&s, method ? 2 : 0, method, 0.005 * shape_size(&s), target, 0, false, &r[method]);
            distance_between(input, s.faces, r[method].corner, r[method].faces, 2500, &state, &far[method][0], &mean[method][0]);
            distance_between(r[method].corner, r[method].faces, input, s.faces, 2500, &state, &far[method][1], &mean[method][1]);
        }
        snprintf(label, sizeof label, "soup of %d triangles (%d parts, %d virtual edges) to %d: faces left without virtual edges", s.faces, r[2].parts, r[2].virtual_edges, target);
        demo_check(d, label, r[0].faces, "info: a triangle alone can still be collapsed away", true);
        demo_check(d, "  parts once the virtual edges join them", r[2].joined, "= 1", r[2].joined == 1);
        demo_check(d, "  largest distance, either way, / size: no virtual edges, plane term only", fmax(far[0][0], far[0][1]) / shape_size(&s), "info", true);
        demo_check(d, "  virtual edges, area term summed", fmax(far[1][0], far[1][1]) / shape_size(&s), "info", true);
        demo_check(d, "  virtual edges, area term anew each time (the paper's)", fmax(far[2][0], far[2][1]) / shape_size(&s), "smaller than both", fmax(far[2][0], far[2][1]) < fmin(fmax(far[0][0], far[0][1]), fmax(far[1][0], far[1][1])));
        demo_check(d, "  mean squared distance, both ways added, / size^2: no virtual edges, plane term only", (mean[0][0] + mean[0][1]) / (shape_size(&s) * shape_size(&s)), "info", true);
        demo_check(d, "  virtual edges, area term summed", (mean[1][0] + mean[1][1]) / (shape_size(&s) * shape_size(&s)), "info", true);
        demo_check(d, "  virtual edges, area term anew each time (the paper's)", (mean[2][0] + mean[2][1]) / (shape_size(&s) * shape_size(&s)), "smaller than both", mean[2][0] + mean[2][1] < fmin(mean[0][0] + mean[0][1], mean[1][0] + mean[1][1]));
    }

    /* 5: the cloth: where the colour of a point of the result comes from */
    shape_build(&s, 2u);
    for (level = 0; level < 2; ++level) {
        const int target = level ? s.faces / 50 : s.faces / 10, count = 4000;
        Wm       *w = wm_open(s.vertices, s.x, s.faces, s.face, 2, 2, 0.005 * shape_size(&s));
        double   *p = malloc((size_t)count * 3 * sizeof(double)), *q = malloc((size_t)count * 3 * sizeof(double)), unused[3], stray = 0.0, differ = 0.0;
        int      *from = malloc((size_t)count * sizeof(int)), *up = malloc((size_t)count * sizeof(int)), *own = malloc((size_t)count * sizeof(int)), wrong = 0, wrong_near = 0, turned = 0, collapses;
        while (w->faces_left > target && wm_collapse(w)) {}
        collapses = w->collapses;
        for (i = 0; i < count; ++i) {
            /* a point on a face of the result; the face looks up or down */
            const double root = sqrt(uniform(&state)), other = uniform(&state);
            const double *c0, *c1, *c2;
            do f = (int)(uniform(&state) * s.faces); while (!w->alive[f]);
            c0 = w->x + 3 * w->face[3 * f]; c1 = w->x + 3 * w->face[3 * f + 1]; c2 = w->x + 3 * w->face[3 * f + 2];
            for (k = 0; k < 3; ++k) p[3 * i + k] = c0[k] * (1.0 - root) + c1[k] * root * (1.0 - other) + c2[k] * root * other;
            from[i] = own[i] = f; up[i] = (c1[2] - c0[2]) * (c2[0] - c0[0]) - (c1[0] - c0[0]) * (c2[2] - c0[2]) > 0.0;
        }
        wm_transfer(w, count, p, from, q);
        for (v = 0; v < 3 * s.vertices; ++v) differ = fmax(differ, fabs(w->x[v] - s.x[v]));
        for (e = 0; e < 3 * s.faces; ++e) differ = fmax(differ, abs(w->face[e] - s.face[e]));
        for (i = 0; i < count; ++i) {
            const int *t = s.face + 3 * from[i];
            double     least = 1e300;
            int        near = 0;
            stray = fmax(stray, sqrt(wm_closest(q + 3 * i, s.x + 3 * t[0], s.x + 3 * t[1], s.x + 3 * t[2], unused)));
            wrong += (from[i] < 3200) != up[i]; turned += (own[i] < 3200) != up[i];
            for (f = 0; f < s.faces; ++f) {
                const double away = wm_closest(p + 3 * i, s.x + 3 * s.face[3 * f], s.x + 3 * s.face[3 * f + 1], s.x + 3 * s.face[3 * f + 2], unused);
                if (away < least) { least = away; near = f; }
            }
            wrong_near += (near < 3200) != up[i];
        }
        snprintf(label, sizeof label, "cloth, %d faces to %d, %d points of the result taken back through %d collapses: mesh afterwards against the input, largest difference", s.faces, target, count, collapses);
        demo_check(d, label, differ, "= 0", differ == 0.0);
        demo_check(d, "  faces afterwards", w->faces_left, "all", w->faces_left == s.faces);
        demo_check(d, "  largest distance of a point's place on the input from the input face it names", stray, "< 1e-12", stray < 1e-12);
        demo_check(d, "  per cent of the points on a face that looks the other way than it did in the input (info)", 100.0 * turned / count, "any", true);
        demo_check(d, "  per cent coloured from the side their face does not look to: nearest point of the whole input", 100.0 * wrong_near / count, "info", true);
        demo_check(d, "  back through the collapses (the paper's)", 100.0 * wrong / count, "fewer", wrong < wrong_near);
        free(p); free(q); free(from); free(up); free(own); wm_close(w);
    }
    for (k = 0; k < 3; ++k) { free(r[k].corner); free(r[k].origin); }
    free(input); free(s.x); free(s.face); free(s.colour);
}

int main(int argc, char **argv) {
    static App app;
    static const DemoOption options[] = {
        {"--scene", .u32 = &app.scene, .lo = 0, .hi = 3, .help = "0 town, 1 soup of two blocks, 2 thin cloth, 3 closed surface"},
        {"--keep", .u32 = &app.keep, .lo = 0, .hi = 5, .help = "faces kept: 0 steps down, 1 .. 5: 50, 25, 10, 5, 2 per cent"},
        {"--virtual", .u32 = &app.virtual_edges, .lo = 0, .hi = 2, .help = "right copy's virtual edges: 0 none, 1 near vertices, 2 near triangles (the paper's)"},
        {"--area", .u32 = &app.area, .lo = 0, .hi = 2, .help = "right copy's area term: 0 none, 1 summed, 2 anew for every cost (the paper's)"},
        {"--baseline", .u32 = &app.baseline, .lo = 0, .hi = 2,
         .help = "middle copy: 0 plane term only, 1 virtual edges between near vertices and the area term summed, 2 the right copy's mesh with every colour from the nearest point of the input"},
        {"--reach", .f32 = &app.reach, .lo = 0.001f, .hi = 0.03f, .help = "r of the virtual edges, as a part of the mesh's size"},
        {"--wire", .flag = &app.wire, .help = "draw the edges of the simplified triangles"},
        {"--no-edges", .flag = &app.no_edges, .help = "do not draw the virtual edges"},
        {"--still", .flag = &app.still, .help = "no turning, no stepping down"},
        {0}};
    Demo *d = &app.demo;
    app.virtual_edges = 2u; app.area = 2u; app.reach = 0.005f; app.built_scene = 99u;
    demo_defaults(d, "wildmesh", "Simplifying meshes in the wild");
    app.orbit = (DemoOrbit){.target = {0.0f, 0.7f, 0.0f}, .distance = 13.5f, .yaw = 0.0f, .pitch = 0.45f, .fov_deg = 36.0f, .znear = 0.1f, .zfar = 100.0f, .min_distance = 3.0f, .max_distance = 30.0f};
    demo_open(d, options, argc, argv);
    canvas_open(d, &app.canvas, 400000u, CANVAS_RGB(24, 27, 36));
    if (d->headless && !d->frames) d->frames = 400u;
    while (demo_running(d)) {
        if (d->ui) app_ui(&app, d->ui);
        app_frame(&app);
    }
    if (d->check) app_check(&app);
    return demo_close(d);
}
