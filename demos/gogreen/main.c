/* gogreen — sculpting brushes from the Green's functions of anisotropic elasticity.
 *
 *   Draw -> Present          (the method runs in C: method.c)
 *
 * Chen and Desbrun: "Go Green: General Regularized Green's Functions for Elasticity"
 * (SIGGRAPH 2022). A grid in the plane z = 0 of an infinite elastic body is displaced by a
 * load at its centre: grab (a force, eq. 6), scale, twist and pinch (affine loads, eq. 11),
 * or five handles with prescribed displacements (the paper's constrained deformation). The
 * material is isotropic, cubic, or one with stiff fibres at an angle.
 *
 * --check tests the harmonics, the radial functions of eq. 13 against their integrals, the
 * isotropic case against Kelvin's solution, the elasticity equation itself, the truncated sum
 * against a direct integral for two anisotropic materials, and the handles.
 */
#include "canvas.h"
#include "method.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PI    3.14159265358979323846
#define CELLS 40

typedef struct App {
    Demo     demo;   /* first: see demo.h */
    Canvas   canvas;
    Gg       green;
    uint32_t material, brush, lmax, made_material;
    float    angle, pull, epsilon, poisson, low, next, clock, made_angle, made_poisson;
    float    at[CELLS + 1][CELLS + 1][3];
} App;

/* 0 isotropic, 1 cubic (C11 : C12 : C44 = 2.23 : 1.61 : 1), 2 fibres ten times stiffer than
 * the rest, along x turned by `angle` about z. These are this demo's choices, not the paper's. */
static void material(double C[3][3][3][3], uint32_t which, double angle, double poisson) {
    static const double k_cubic[6][6] = {{2.23, 1.61, 1.61, 0, 0, 0}, {1.61, 2.23, 1.61, 0, 0, 0}, {1.61, 1.61, 2.23, 0, 0, 0}, {0, 0, 0, 1, 0, 0}, {0, 0, 0, 0, 1, 0}, {0, 0, 0, 0, 0, 1}};
    static const double k_fibre[6][6] = {{20, 1, 1, 0, 0, 0}, {1, 2.5, 1, 0, 0, 0}, {1, 1, 2.5, 0, 0, 0}, {0, 0, 0, 0.75, 0, 0}, {0, 0, 0, 0, 1, 0}, {0, 0, 0, 0, 0, 1}};
    gg_isotropic(C, 1.0, poisson);
    if (which == 1u) gg_voigt(C, k_cubic);
    if (which == 2u) gg_voigt(C, k_fibre);
    gg_rotate_z(C, angle);
}

static void app_ui(App *a, struct nk_context *ctx) {
    static const char *const k_materials[3] = {"Isotropic", "Cubic", "Fibres"};
    static const char *const k_brushes[5] = {"Grab", "Scale", "Twist", "Pinch", "Five handles"};
    float lmax = (float)a->lmax;
    if (nk_begin(ctx, "Go Green", nk_rect(16, 16, 320, 330), NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_TITLE)) {
        demo_ui_choice(ctx, "Material", k_materials, 3u, &a->material);
        demo_ui_choice(ctx, "Brush", k_brushes, 5u, &a->brush);
        demo_ui_float(ctx, "Material turned, degrees", &a->angle, 0.0f, 180.0f);
        demo_ui_float(ctx, "Pull direction, degrees", &a->pull, 0.0f, 360.0f);
        demo_ui_float(ctx, "Width of the load", &a->epsilon, 0.05f, 0.8f);
        demo_ui_float(ctx, "Poisson's ratio (isotropic)", &a->poisson, 0.0f, 0.49f);
        demo_ui_float(ctx, "Highest degree", &lmax, 2.0f, 20.0f);
        demo_ui_float(ctx, "Weight of the lowest radial function", &a->low, 0.0f, 3.0f);
        demo_ui_float(ctx, "Weight of the next", &a->next, 0.0f, 10.0f);
    }
    nk_end(ctx);
    a->lmax = 2u * (uint32_t)(0.5f * lmax + 0.5f);
}

static void app_frame(App *a) {
    static const double k_affine[3][3][3] = {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}, {{0, -1, 0}, {1, 0, 0}, {0, 0, 0}}, {{1, 0, 0}, {0, -1, 0}, {0, 0, 0}}};
    static const double k_handle[5][3] = {{0, 0, 0}, {0.6, 0.6, 0}, {-0.6, 0.6, 0}, {-0.6, -0.6, 0}, {0.6, -0.6, 0}};
    Demo        *d = &a->demo;
    Canvas      *c = &a->canvas;
    Gg          *s = &a->green;
    const int    handles = a->brush == 0u ? 1 : a->brush == 4u ? 5 : 0;
    double       amount, moved[15] = {0}, load[15], largest = 0.0, G[3][3], D[3][3][3];
    int          i, j, k, p, q;

    if (a->made_material != a->material || a->made_angle != a->angle || a->made_poisson != a->poisson) {
        material(s->C, a->material, a->angle * PI / 180.0, a->poisson);
        gg_material(s, 8, 0.25, 48);
        a->made_material = a->material; a->made_angle = a->angle; a->made_poisson = a->poisson;
    }
    s->lmax = (int)a->lmax; s->epsilon = a->epsilon;
    /* the lowest two radial functions that the brush uses: R_0, R_2 or, for affine loads, R_1, R_3 */
    for (i = 0; i < GG_DEGREES; ++i) s->weight[i] = i < 2 ? a->low : i < 4 ? a->next : 1.0;
    a->clock += 1.0f / 60.0f;
    amount = sin(1.5 * a->clock);

    /* grab and handles: the centre is moved by 0.3, the other handles stay */
    moved[0] = 0.3 * amount * cos(a->pull * PI / 180.0); moved[1] = 0.3 * amount * sin(a->pull * PI / 180.0);
    if (handles) gg_fit(s, handles, &k_handle[0][0], moved, load);

    for (j = 0; j <= CELLS; ++j)
        for (i = 0; i <= CELLS; ++i) {
            const double x[3] = {-1.0 + 2.0 * i / CELLS, -1.0 + 2.0 * j / CELLS, 0.0};
            double       u[3] = {0.0, 0.0, 0.0};
            for (k = 0; k < handles; ++k) {
                const double from[3] = {x[0] - k_handle[k][0], x[1] - k_handle[k][1], 0.0};
                gg_green(s, from, G);
                for (p = 0; p < 3; ++p)
                    for (q = 0; q < 3; ++q) u[p] += G[p][q] * load[3 * k + q];
            }
            if (!handles) {
                gg_gradient(s, x, D);
                for (k = 0; k < 3; ++k)
                    for (p = 0; p < 3; ++p)
                        for (q = 0; q < 3; ++q) u[p] += D[k][p][q] * k_affine[a->brush - 1u][q][k];
            }
            largest = fmax(largest, hypot(u[0], u[1]));
            a->at[j][i][0] = (float)u[0]; a->at[j][i][1] = (float)u[1]; a->at[j][i][2] = 0.0f;
        }
    /* affine brushes have no natural size: the largest displacement is set to 0.12 */
    for (j = 0; j <= CELLS; ++j)
        for (i = 0; i <= CELLS; ++i) {
            const float scale = handles ? 1.0f : (float)(0.12 * amount / fmax(largest, 1e-30));
            a->at[j][i][0] = -1.0f + 2.0f * (float)i / CELLS + scale * a->at[j][i][0];
            a->at[j][i][1] = -1.0f + 2.0f * (float)j / CELLS + scale * a->at[j][i][1];
        }

    canvas_view_flat(d, c, 0.0f, 0.0f, 1.2f);
    if (a->material) {
        /* the direction that the material's x axis has after the turn */
        const float along[3] = {1.12f * cosf(a->angle * (float)PI / 180.0f), 1.12f * sinf(a->angle * (float)PI / 180.0f), 0.0f}, back[3] = {-along[0], -along[1], 0.0f};
        canvas_line(c, back, along, 0.012f, CANVAS_RGB(200, 120, 70));
    }
    for (j = 0; j <= CELLS; ++j)
        for (i = 0; i < CELLS; ++i) {
            canvas_line(c, a->at[j][i], a->at[j][i + 1], 0.009f, CANVAS_RGB(110, 160, 220));
            canvas_line(c, a->at[i][j], a->at[i + 1][j], 0.009f, CANVAS_RGB(110, 160, 220));
        }
    for (k = 0; k < handles; ++k) {
        const float at[3] = {(float)(k_handle[k][0] + moved[3 * k]), (float)(k_handle[k][1] + moved[3 * k + 1]), 0.1f};
        canvas_dot(c, at, 0.025f, k ? CANVAS_RGB(230, 230, 230) : CANVAS_RGB(240, 200, 90));
    }
    canvas_frame(d, c);
}

/* spherical Bessel function, for the check */
static double bessel(int l, double z) {
    double a = 1.0, b, sum;
    int    k;
    if (z < l + 1.0) {
        for (k = 1; k <= l; ++k) a *= z / (2.0 * k + 1.0);
        for (sum = a, k = 1; k < 60; ++k) { a *= -0.5 * z * z / (k * (2.0 * l + 2.0 * k + 1.0)); sum += a; }
        return sum;
    }
    a = sin(z) / z; b = sin(z) / (z * z) - cos(z) / z;
    if (l == 0) return a;
    for (k = 1; k < l; ++k) { const double next = (2.0 * k + 1.0) / z * b - a; a = b; b = next; }
    return b;
}

/* G far from the load by the integral over the unit circle across x: 1 / (8 pi^2 r) of the
 * integral of (C_ijkl xi_j xi_l)^-1. Written without method.c. */
static void far_field(const double C[3][3][3][3], const double x[3], double G[3][3]) {
    const double r = sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]), n[3] = {x[0] / r, x[1] / r, x[2] / r};
    const double pick[3] = {fabs(n[0]) < 0.6 ? 1.0 : 0.0, fabs(n[0]) < 0.6 ? 0.0 : 1.0, 0.0};
    double       e1[3] = {n[1] * pick[2] - n[2] * pick[1], n[2] * pick[0] - n[0] * pick[2], n[0] * pick[1] - n[1] * pick[0]}, e2[3], length;
    int          t, i, j, k, l;
    length = sqrt(e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2]);
    for (i = 0; i < 3; ++i) e1[i] /= length;
    e2[0] = n[1] * e1[2] - n[2] * e1[1]; e2[1] = n[2] * e1[0] - n[0] * e1[2]; e2[2] = n[0] * e1[1] - n[1] * e1[0];
    memset(G, 0, 9 * sizeof(double));
    for (t = 0; t < 720; ++t) {
        const double phi = 2.0 * PI * t / 720.0, xi[3] = {cos(phi) * e1[0] + sin(phi) * e2[0], cos(phi) * e1[1] + sin(phi) * e2[1], cos(phi) * e1[2] + sin(phi) * e2[2]};
        double       A[3][3] = {{0}}, det, inv[3][3];
        for (i = 0; i < 3; ++i)
            for (j = 0; j < 3; ++j)
                for (k = 0; k < 3; ++k)
                    for (l = 0; l < 3; ++l) A[i][k] += C[i][j][k][l] * xi[j] * xi[l];
        for (i = 0; i < 3; ++i)
            for (j = 0; j < 3; ++j) inv[j][i] = A[(i + 1) % 3][(j + 1) % 3] * A[(i + 2) % 3][(j + 2) % 3] - A[(i + 1) % 3][(j + 2) % 3] * A[(i + 2) % 3][(j + 1) % 3];
        det = A[0][0] * inv[0][0] + A[0][1] * inv[1][0] + A[0][2] * inv[2][0];
        for (i = 0; i < 3; ++i)
            for (j = 0; j < 3; ++j) G[i][j] += inv[i][j] / det * (2.0 * PI / 720.0) / (8.0 * PI * PI * r);
    }
}

/* C_ijkl d2G_km / dx_l dx_j + delta_im g(x), largest entry over the load at its centre; the
 * second derivatives are differences of eq. 12. */
static double residual(const Gg *s, const double x[3]) {
    const double e = 1e-4 * s->epsilon, r2 = x[0] * x[0] + x[1] * x[1] + x[2] * x[2];
    const double peak = 1.0 / (s->epsilon * s->epsilon * s->epsilon * pow(PI, 1.5)), g = peak * exp(-r2 / (s->epsilon * s->epsilon));
    double       second[3][3][3][3], up[3][3][3], down[3][3][3], worst = 0.0;   /* second[l][j][k][m] */
    int          i, j, k, l, m;
    for (l = 0; l < 3; ++l) {
        double a[3] = {x[0], x[1], x[2]}, b[3] = {x[0], x[1], x[2]};
        a[l] += e; b[l] -= e;
        gg_gradient(s, a, up); gg_gradient(s, b, down);
        for (j = 0; j < 3; ++j)
            for (k = 0; k < 3; ++k)
                for (m = 0; m < 3; ++m) second[l][j][k][m] = (up[j][k][m] - down[j][k][m]) / (2.0 * e);
    }
    for (i = 0; i < 3; ++i)
        for (m = 0; m < 3; ++m) {
            double sum = (i == m) * g;
            for (j = 0; j < 3; ++j)
                for (k = 0; k < 3; ++k)
                    for (l = 0; l < 3; ++l) sum += s->C[i][j][k][l] * second[l][j][k][m];
            worst = fmax(worst, fabs(sum) / peak);
        }
    return worst;
}

static void app_check(App *a) {
    static const char *const k_names[3] = {"isotropic", "cubic", "fibres at 30 degrees"};
    static const double      k_radius[7] = {0.3, 1.0, 3.0, 8.8, 9.1, 12.0, 30.0};
    static Gg                other;
    Demo                    *d = &a->demo;
    Gg                      *s = &a->green;
    double                   y[GG_TERMS], z[GG_TERMS], worst, second, G[3][3], D[3][3][3];
    int                      i, j, l, m, t, n, which;
    char                     label[160];

    /* 1: the harmonics */
    {
        double addition = 0.0;
        srand(5u);
        for (t = 0; t < 200; ++t) {
            double p[3], q[3], dot, before = 1.0, now, length;
            for (i = 0; i < 3; ++i) { p[i] = rand() / (double)RAND_MAX - 0.5; q[i] = rand() / (double)RAND_MAX - 0.5; }
            length = sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]); for (i = 0; i < 3; ++i) p[i] /= length;
            length = sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]); for (i = 0; i < 3; ++i) q[i] /= length;
            dot = p[0] * q[0] + p[1] * q[1] + p[2] * q[2]; now = dot;
            gg_harmonics(GG_DEGREES - 1, p, y); gg_harmonics(GG_DEGREES - 1, q, z);
            for (l = 1; l < GG_DEGREES; ++l) {
                double sum = 0.0;
                for (m = -l; m <= l; ++m) sum += y[l * l + l + m] * z[l * l + l + m];
                addition = fmax(addition, fabs(sum - (2.0 * l + 1.0) / (4.0 * PI) * now));
                { const double next = ((2.0 * l + 1.0) * dot * now - l * before) / (l + 1.0); before = now; now = next; }
            }
        }
        demo_check(d, "real harmonics of degrees 1 .. 21, 200 pairs of directions: sum_m y_lm(p) y_lm(q) against (2l + 1) / (4 pi) P_l(p . q)", addition, "< 1e-11", addition < 1e-11);
    }

    /* 2: eq. 13 against the integrals it stands for */
    {
        double load = 0.0, affine = 0.0, erf_form = 0.0;
        for (l = 0; l <= 9; ++l)
            for (t = 0; t < 7; ++t) {
                const double eps = 0.25, r = eps * k_radius[t], step = 14.0 / eps / 200000.0;
                double       sum = 0.0, sum2 = 0.0;
                for (i = 0; i <= 200000; ++i) {
                    const double xi = i * step, weight = (i == 0 || i == 200000) ? 1.0 : i % 2 ? 4.0 : 2.0, g = exp(-eps * eps * xi * xi / 4.0) * bessel(l, r * xi);
                    sum += weight * g * step / 3.0; sum2 += weight * g * xi * step / 3.0;
                }
                load   = fmax(load, fabs(gg_radial(l, r, eps) - sum) / (fabs(sum) + 1e-3));
                affine = fmax(affine, fabs(gg_radial_affine(l, r, eps) - sum2) / (fabs(sum2) + 1e-3));
                if (l == 0) erf_form = fmax(erf_form, fabs(gg_radial(0, r, eps) - PI * erf(r / eps) / (2.0 * r)) * r);
            }
        demo_check(d, "eq. 13, R_l for l 0 .. 9 at r / epsilon 0.3 .. 30 against Simpson's rule on its integral, relative", load, "< 1e-9", load < 1e-9);
        demo_check(d, "  the same for the radial function of affine loads", affine, "< 1e-9", affine < 1e-9);
        demo_check(d, "  R_0 against pi erf(r / epsilon) / (2 r), x r", erf_form, "< 1e-12", erf_form < 1e-12);
    }

    /* 3 to 5, for each material */
    for (which = 0; which < 3; ++which) {
        static const double k_near[4][3] = {{0.21, -0.13, 0.3}, {0.05, 0.02, -0.03}, {-0.3, 0.4, 0.1}, {0.0, 0.25, 0.0}};
        double              high = 0.0, high_q = 0.0, sym = 0.0, gradient = 0.0, scale = 0.0, pde = 0.0, pde8 = 0.0, coarse = 0.0, size = 0.0;
        material(s->C, (uint32_t)which, 30.0 * PI / 180.0, 0.3);
        gg_material(s, 20, 0.25, 48);
        memcpy(other.C, s->C, sizeof other.C);
        gg_material(&other, 20, 0.25, 96);
        for (t = 0; t < GG_TERMS; ++t)
            for (i = 0; i < 3; ++i)
                for (j = 0; j < 3; ++j) {
                    const int degree = (int)sqrt((double)t);
                    coarse = fmax(coarse, fabs(s->P[t][i][j] - other.P[t][i][j])); size = fmax(size, fabs(s->P[t][i][j]));
                    if (degree >= 4) high = fmax(high, fabs(s->P[t][i][j]));
                    if (degree >= 5) high_q = fmax(high_q, fmax(fabs(s->Q[t][0][i][j]), fmax(fabs(s->Q[t][1][i][j]), fabs(s->Q[t][2][i][j]))));
                }
        snprintf(label, sizeof label, "%s: largest entry of the material matrices of degree >= 4", k_names[which]);
        demo_check(d, label, high, which ? "> 1e-3" : "< 1e-12 (only 0 and 2)", which ? high > 1e-3 : high < 1e-12);
        demo_check(d, "  of degree >= 5, affine loads", high_q, which ? "> 1e-3" : "< 1e-12 (only 1 and 3)", which ? high_q > 1e-3 : high_q < 1e-12);
        demo_check(d, "  matrices from 48 x 96 quadrature points against 96 x 192, relative", coarse / size, "< 1e-6", coarse / size < 1e-6);

        /* far from the load: the truncated sum against the direct integral, 60 directions */
        for (n = 0; n < 4; ++n) {
            static const int k_lmax[4] = {4, 8, 12, 20};
            double           off = 0.0, all = 0.0;
            s->lmax = k_lmax[n];
            srand(11u);
            for (t = 0; t < 60; ++t) {
                double x[3], exact[3][3], length;
                for (i = 0; i < 3; ++i) x[i] = rand() / (double)RAND_MAX - 0.5;
                length = sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
                for (i = 0; i < 3; ++i) x[i] *= 250.0 / length;   /* 1000 epsilon away */
                gg_green(s, x, G); far_field(s->C, x, exact);
                for (i = 0; i < 3; ++i)
                    for (j = 0; j < 3; ++j) { off += fabs(G[i][j] - exact[i][j]); all += fabs(exact[i][j]); sym = fmax(sym, fabs(G[i][j] - G[j][i]) * 250.0); }
            }
            snprintf(label, sizeof label, "  1000 epsilon away, degrees up to %d: sum |G - direct integral| / sum |direct integral|", k_lmax[n]);
            demo_check(d, label, off / all, which == 0 ? "< 1e-5" : n == 3 ? "< 1e-2" : "info", which == 0 ? off / all < 1e-5 : n == 3 ? off / all < 1e-2 : true);
            /* near the load: the elasticity equation */
            for (t = 0; t < 4 && (n == 1 || n == 3); ++t) {
                if (n == 1) pde8 = fmax(pde8, residual(s, k_near[t]));
                if (n == 3) pde = fmax(pde, residual(s, k_near[t]));
            }
        }
        demo_check(d, "  G - G transposed, x r", sym, "< 1e-12", sym < 1e-12);
        demo_check(d, "  eq. 2 near the load, degrees up to 8: largest residual / load at its centre", pde8, which ? "info" : "< 1e-5", which ? true : pde8 < 1e-5);
        demo_check(d, "  the same with degrees up to 20", pde, which ? "< 0.05" : "< 1e-5", which ? pde < 0.05 : pde < 1e-5);
        /* eq. 12 against differences of eq. 10 */
        for (t = 0; t < 4; ++t)
            for (l = 0; l < 3; ++l) {
                double p[3] = {k_near[t][0], k_near[t][1], k_near[t][2]}, q[3] = {k_near[t][0], k_near[t][1], k_near[t][2]}, up[3][3], down[3][3];
                p[l] += 1e-5; q[l] -= 1e-5;
                gg_green(s, p, up); gg_green(s, q, down); gg_gradient(s, k_near[t], D);
                for (i = 0; i < 3; ++i)
                    for (j = 0; j < 3; ++j) { gradient = fmax(gradient, fabs((up[i][j] - down[i][j]) / 2e-5 - D[l][i][j])); scale = fmax(scale, fabs(D[l][i][j])); }
            }
        demo_check(d, "  eq. 12 against differences of eq. 10, degrees up to 20 and 21, relative", gradient / scale, which ? "< 0.05" : "< 1e-7", which ? gradient / scale < 0.05 : gradient / scale < 1e-7);
    }

    /* Kelvin's solution, isotropic */
    {
        const double x[3] = {120.0, -70.0, 200.0}, r = sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
        material(s->C, 0u, 0.0, 0.3);
        gg_material(s, 8, 0.25, 48);
        gg_green(s, x, G);
        for (worst = second = 0.0, i = 0; i < 3; ++i)
            for (j = 0; j < 3; ++j) {
                const double kelvin = ((3.0 - 4.0 * 0.3) * (i == j) + x[i] * x[j] / (r * r)) / (16.0 * PI * (1.0 - 0.3) * r);
                worst = fmax(worst, fabs(G[i][j] - kelvin)); second = fmax(second, fabs(kelvin));
            }
        demo_check(d, "isotropic, 980 epsilon away: G against Kelvin's solution, relative", worst / second, "< 1e-5", worst / second < 1e-5);
    }

    /* handles */
    {
        static const double k_point[5][3] = {{0, 0, 0}, {0.6, 0.6, 0}, {-0.6, 0.6, 0}, {-0.6, -0.6, 0.2}, {0.6, -0.6, 0}};
        static const double k_moved[15] = {0.3, 0.1, 0.05, 0, 0, 0, 0, 0, 0, -0.1, 0, 0.1, 0, 0, 0};
        double              load[15], miss = 0.0;
        material(s->C, 2u, 30.0 * PI / 180.0, 0.3);
        gg_material(s, 8, 0.25, 48);
        gg_fit(s, 5, &k_point[0][0], k_moved, load);
        for (n = 0; n < 5; ++n) {
            double u[3] = {0.0, 0.0, 0.0};
            for (t = 0; t < 5; ++t) {
                const double from[3] = {k_point[n][0] - k_point[t][0], k_point[n][1] - k_point[t][1], k_point[n][2] - k_point[t][2]};
                gg_green(s, from, G);
                for (i = 0; i < 3; ++i)
                    for (j = 0; j < 3; ++j) u[i] += G[i][j] * load[3 * t + j];
            }
            for (i = 0; i < 3; ++i) miss = fmax(miss, fabs(u[i] - k_moved[3 * n + i]));
        }
        demo_check(d, "five handles in the fibre material: largest miss of a prescribed displacement", miss, "< 1e-12", miss < 1e-12);
    }
    a->made_material = ~0u;
}

int main(int argc, char **argv) {
    static App app;
    static const DemoOption options[] = {
        {"--material", .u32 = &app.material, .lo = 0, .hi = 2, .help = "0 isotropic, 1 cubic, 2 stiff fibres"},
        {"--brush", .u32 = &app.brush, .lo = 0, .hi = 4, .help = "0 grab, 1 scale, 2 twist, 3 pinch, 4 five handles"},
        {"--angle", .f32 = &app.angle, .lo = 0, .hi = 180, .help = "the material turned about z, degrees"},
        {"--pull", .f32 = &app.pull, .lo = 0, .hi = 360, .help = "direction in which the centre is pulled, degrees"},
        {"--epsilon", .f32 = &app.epsilon, .lo = 0.05f, .hi = 0.8f, .help = "width of the Gaussian load"},
        {"--poisson", .f32 = &app.poisson, .lo = 0, .hi = 0.49f, .help = "Poisson's ratio of the isotropic material"},
        {"--lmax", .u32 = &app.lmax, .lo = 0, .hi = 20, .help = "highest degree of the harmonics for loads (affine loads: one more)"},
        {"--low", .f32 = &app.low, .lo = 0, .hi = 3, .help = "factor on the lowest radial function (R_0, or R_1 of affine loads)"},
        {"--next", .f32 = &app.next, .lo = 0, .hi = 10, .help = "factor on the next radial function (R_2, or R_3)"},
        {0}};
    Demo *d = &app.demo;
    app.material = 2u; app.angle = 30.0f; app.epsilon = 0.25f; app.poisson = 0.3f; app.lmax = 8u; app.low = 1.0f; app.next = 1.0f; app.made_material = ~0u;
    demo_defaults(d, "gogreen", "Go Green");
    demo_open(d, options, argc, argv);
    canvas_open(d, &app.canvas, 60000u, CANVAS_RGB(24, 27, 36));
    if (d->headless && !d->frames) d->frames = 63u;
    while (demo_running(d)) {
        if (d->ui) app_ui(&app, d->ui);
        app_frame(&app);
    }
    if (d->check) app_check(&app);
    return demo_close(d);
}
