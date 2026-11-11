/* check.inc — lines' acceptance test; included by main.c.
 *
 * Line width in pixels, and which side of the silhouette the line is on, measured on a
 * ball; what each method draws on a box; false lines on a floor seen at a low angle.
 */

typedef struct Shot {
    uint8_t *object;   /* 1 where the objects drawn cover the pixel */
    uint8_t *line;     /* 1 where the final picture is black */
    size_t   covered, lines, lines_inside, lines_outside;
} Shot;

static void shot_free(Shot *s) { free(s->object); free(s->line); }

static void shot(App *a, uint32_t first, uint32_t last, uint32_t mode, float width, Shot *s) {
    Demo        *d      = &a->demo;
    const size_t pixels = (size_t)d->width * d->height;
    float       *normal, *color;
    size_t       i;
    memset(s, 0, sizeof *s);
    s->object = (uint8_t *)malloc(pixels); s->line = (uint8_t *)malloc(pixels);
    a->first = first; a->last = last; a->mode = mode; a->width = width;
    app_frame(a);
    normal = demo_read_floats(d, a->normal, pixels * 4u);
    color  = demo_read_floats(d, a->hdr, pixels * 4u);
    for (i = 0u; i < pixels; ++i) {
        s->object[i] = normal[i * 4u + 3u] > 0.5f;
        s->line[i]   = color[i * 4u] + color[i * 4u + 1u] + color[i * 4u + 2u] < 0.05f;
        s->covered += s->object[i]; s->lines += s->line[i];
        s->lines_inside += s->line[i] && s->object[i]; s->lines_outside += s->line[i] && !s->object[i];
    }
    free(normal); free(color);
}

/* Is there a pixel with map == want within `reach` pixels (a square)? Outside the picture counts as `edge`. */
static bool near_pixel(const App *a, const uint8_t *map, uint32_t x, uint32_t y, int reach, uint8_t want, bool edge) {
    const int w = (int)a->demo.width, h = (int)a->demo.height;
    int       dx, dy;
    for (dy = -reach; dy <= reach; ++dy)
        for (dx = -reach; dx <= reach; ++dx) {
            const int qx = (int)x + dx, qy = (int)y + dy;
            if (qx < 0 || qy < 0 || qx >= w || qy >= h) { if (edge) return true; continue; }
            if (map[(size_t)qx + (size_t)qy * (size_t)w] == want) return true;
        }
    return false;
}

static void app_check(App *a) {
    static const float k_widths[3] = {2.0f, 3.0f, 5.0f}, k_distance[2] = {6.0f, 14.0f};
    Demo           *d = &a->demo;
    const App       saved = *a;
    const double    pi = 3.14159265358979;
    char     label[160];
    uint32_t i, j, x, y, method;
    Shot     s;

    /* The ball alone. Its silhouette is a circle (nearly: it is close to the middle of the picture), so
       radius = sqrt(area / pi), and the line's width is the difference of two radii. */
    for (j = 0u; j < 2u; ++j)
        for (i = 0u; i < 3u; ++i) {
            double radius, hull_width, edge_width;
            a->orbit = saved.orbit; a->orbit.target[1] = 1.0f; a->orbit.distance = k_distance[j];
            shot(a, 1u, 2u, MODE_HULL, k_widths[i], &s);
            radius     = sqrt((double)s.covered / pi);
            hull_width = sqrt((double)(s.covered + s.lines_outside) / pi) - radius;
            if (i == 0u) {
                snprintf(label, sizeof label, "ball at %.0f m: radius on screen, pixels (reported)", (double)k_distance[j]);
                demo_check(d, label, radius, "> 10", radius > 10.0);
            }
            snprintf(label, sizeof label, "ball at %.0f m, hull, %.0f px asked: width measured", (double)k_distance[j], (double)k_widths[i]);
            demo_check(d, label, hull_width, "within 0.35 px", fabs(hull_width - (double)k_widths[i]) < 0.35);
            snprintf(label, sizeof label, "ball at %.0f m, hull, %.0f px asked: line pixels on the ball", (double)k_distance[j], (double)k_widths[i]);
            demo_check(d, label, (double)s.lines_inside, "== 0", s.lines_inside == 0u);
            shot_free(&s);
            shot(a, 1u, 2u, MODE_EDGES, k_widths[i], &s);
            edge_width = radius - sqrt((double)(s.covered - s.lines_inside) / pi);
            snprintf(label, sizeof label, "ball at %.0f m, edges, %.0f px asked: width measured", (double)k_distance[j], (double)k_widths[i]);
            /* The search goes along the two axes only, so a line of w pixels there is w / sqrt(2) on the
               diagonals; round a circle the mean is w * 2 sqrt(2) / pi = 0.90 w. */
            demo_check(d, label, edge_width, "0.90 x asked, within 0.15 px", fabs(edge_width - 0.9003 * (double)k_widths[i]) < 0.15);
            snprintf(label, sizeof label, "ball at %.0f m, edges, %.0f px asked: line pixels outside the ball", (double)k_distance[j], (double)k_widths[i]);
            demo_check(d, label, (double)s.lines_outside, "== 0", s.lines_outside == 0u);
            shot_free(&s);
        }

    /* A box alone: the silhouette, and the two edges between its three visible faces. */
    a->orbit = saved.orbit; a->orbit.target[0] = -2.6f; a->orbit.target[1] = 0.7f; a->orbit.target[2] = 0.4f; a->orbit.distance = 6.0f;
    for (method = 0u; method < 2u; ++method) {
        size_t rim = 0u, rim_lined = 0u, interior = 0u;
        shot(a, 2u, 3u, method ? MODE_EDGES : MODE_HULL, 3.0f, &s);
        for (y = 0u; y < d->height; ++y)
            for (x = 0u; x < d->width; ++x) {
                const size_t at = (size_t)x + (size_t)y * d->width;
                if (!s.object[at]) continue;
                if (near_pixel(a, s.object, x, y, 1, 0u, true)) {            /* on the silhouette */
                    ++rim;
                    rim_lined += near_pixel(a, s.line, x, y, 2, 1u, false);
                } else if (s.line[at] && !near_pixel(a, s.object, x, y, 6, 0u, true)) ++interior;   /* well inside */
            }
        snprintf(label, sizeof label, "box, %s: share of the silhouette with a line within 2 px", method ? "edges" : "hull");
        demo_check(d, label, (double)rim_lined / (double)rim, method ? "== 1" : "> 0.97", method ? rim_lined == rim : rim_lined * 100u > rim * 97u);
        snprintf(label, sizeof label, "box, %s: line pixels more than 6 px inside the silhouette", method ? "edges" : "hull");
        demo_check(d, label, (double)interior, method ? "> 100" : "== 0", method ? interior > 100u : interior == 0u);
        shot_free(&s);
    }

    /* The floor alone, seen from 1.5 m up: no line belongs anywhere but at its far edge. */
    a->orbit = saved.orbit; a->orbit.pitch = 0.06f;
    for (method = 0u; method < 2u; ++method) {
        size_t wrong = 0u;
        a->naive = method == 1u;
        shot(a, 0u, 1u, MODE_EDGES, 3.0f, &s);
        for (y = 0u; y < d->height; ++y)
            for (x = 0u; x < d->width; ++x)
                if (s.line[(size_t)x + (size_t)y * d->width] && !near_pixel(a, s.object, x, y, 4, 0u, false)) ++wrong;
        snprintf(label, sizeof label, "floor at a low angle, %s: share of the floor with a line that is not its edge",
                 method ? "depths compared directly" : "depth compared with the pixel's plane");
        demo_check(d, label, (double)wrong / (double)s.covered, method ? "> 1 % (why the plane is used)" : "== 0",
                         method ? wrong * 100u > s.covered : wrong == 0u);
        shot_free(&s);
    }
    a->naive = false;

    /* The whole scene. */
    a->orbit = saved.orbit;
    for (method = 0u; method < 2u; ++method) {
        shot(a, 0u, a->object_count, method ? MODE_EDGES : MODE_HULL, 3.0f, &s);
        snprintf(label, sizeof label, "whole scene, %s: share of the picture that is line", method ? "edges" : "hull");
        demo_check(d, label, (double)s.lines / (double)((size_t)d->width * d->height), "0.5 % .. 10 %",
                         s.lines * 200u > (size_t)d->width * d->height && s.lines * 10u < (size_t)d->width * d->height);
        shot_free(&s);
    }
    a->orbit = saved.orbit; a->first = saved.first; a->last = saved.last; a->mode = saved.mode; a->width = saved.width;
}
