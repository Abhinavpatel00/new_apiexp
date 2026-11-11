/* method.c — see method.h. Follows Feng, Gkioulekas, Crane, "Points as Tori"
 * (2026); equation numbers are the paper's. */
#include "method.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the network (section 4.3) --------------------------------------------
 *
 * features (6) -> linear -> layer norm -> 8 x [ x += attention(norm(x));
 * x += mlp(norm(x)) ] -> layer norm -> the centre token -> linear (6).
 * Kernels are stored [input][output], so a layer is a sum of rows. */

bool pat_net_load(PatNet *net, const char *path) {
    FILE    *f = fopen(path, "rb");
    char     magic[4];
    uint32_t head[5];
    size_t   count;
    if (!f) return false;
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "PATW", 4) != 0 || fread(head, 4, 5, f) != 5) { fclose(f); return false; }
    net->embed = head[0]; net->layers = head[1]; net->heads = head[2]; net->mlp = head[3]; net->k = head[4];
    count = (size_t)6 * net->embed + 3 * net->embed +
            (size_t)net->layers * (4 * (size_t)net->embed * net->embed + 9 * net->embed + 2 * (size_t)net->embed * net->mlp + net->mlp) +
            2 * net->embed + (size_t)net->embed * 6 + 6;
    net->w = (float *)malloc(count * sizeof(float));
    if (net->k != PAT_NEIGHBOURS || fread(net->w, sizeof(float), count, f) != count || fgetc(f) != EOF) {
        fclose(f); free(net->w); net->w = NULL;
        return false;
    }
    fclose(f);
    return true;
}

/* out[t] = bias + in[t] . kernel, for `tokens` rows. Returns the weights after this layer. */
static const float *linear(const float *w, const float *in, uint32_t n_in, float *out, uint32_t n_out, uint32_t tokens) {
    const float *bias = w + (size_t)n_in * n_out;
    uint32_t t, i, o;
    for (t = 0u; t < tokens; ++t) {
        float       *y = out + (size_t)t * n_out;
        const float *x = in + (size_t)t * n_in;
        for (o = 0u; o < n_out; ++o) y[o] = bias[o];
        for (i = 0u; i < n_in; ++i) {
            const float  a = x[i];
            const float *row = w + (size_t)i * n_out;
            for (o = 0u; o < n_out; ++o) y[o] += a * row[o];
        }
    }
    return bias + n_out;
}

static const float *layer_norm(const float *w, const float *in, float *out, uint32_t n, uint32_t tokens) {
    uint32_t t, i;
    for (t = 0u; t < tokens; ++t) {
        const float *x = in + (size_t)t * n;
        float mean = 0.0f, var = 0.0f, scale;
        for (i = 0u; i < n; ++i) mean += x[i];
        mean /= (float)n;
        for (i = 0u; i < n; ++i) var += (x[i] - mean) * (x[i] - mean);
        scale = 1.0f / sqrtf(var / (float)n + 1e-6f);
        for (i = 0u; i < n; ++i) out[(size_t)t * n + i] = (x[i] - mean) * scale * w[i] + w[n + i];
    }
    return w + 2u * n;
}

void pat_frame(const double n[3], double s[3], double t[3]) {
    /* n x (1,0,0), or n x (0,1,0) when n is nearly along x; then t = n x s. The released code's choice,
     * which the weights were trained with. */
    const double u[3] = {fabs(n[0]) > 0.9 ? 0.0 : 1.0, fabs(n[0]) > 0.9 ? 1.0 : 0.0, 0.0};
    double len;
    s[0] = n[1] * u[2] - n[2] * u[1]; s[1] = n[2] * u[0] - n[0] * u[2]; s[2] = n[0] * u[1] - n[1] * u[0];
    len = sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    s[0] /= len; s[1] /= len; s[2] /= len;
    t[0] = n[1] * s[2] - n[2] * s[1]; t[1] = n[2] * s[0] - n[0] * s[2]; t[2] = n[0] * s[1] - n[1] * s[0];
}

static int compare_float(const void *a, const void *b) {
    const float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

void pat_net_forward(const PatNet *net, const float (*pos)[3], const float (*normal)[3], double coef[6]) {
    const uint32_t e = net->embed, k = net->k, dim = e / net->heads;
    const float   *w = net->w;
    float  *x = (float *)malloc((size_t)k * e * 5u * sizeof(float)), *h = x + (size_t)k * e, *q = h + (size_t)k * e,
           *key = q + (size_t)k * e, *val = key + (size_t)k * e;
    float  *wide = (float *)malloc((size_t)k * net->mlp * sizeof(float));
    float   feature[PAT_NEIGHBOURS][6], dist[PAT_NEIGHBOURS], weight[PAT_NEIGHBOURS], out[6];
    double  n0[3], s[3], t[3], len, median;
    uint32_t i, j, c, block, head;

    /* Features: position and normal of each neighbour in the centre's frame, lengths in units of the
     * median distance to the centre (the centre's own zero counts). */
    len = sqrt((double)normal[0][0] * normal[0][0] + (double)normal[0][1] * normal[0][1] + (double)normal[0][2] * normal[0][2]);
    for (c = 0u; c < 3u; ++c) n0[c] = normal[0][c] / len;
    pat_frame(n0, s, t);
    for (i = 0u; i < k; ++i) {
        const double d[3] = {(double)pos[i][0] - pos[0][0], (double)pos[i][1] - pos[0][1], (double)pos[i][2] - pos[0][2]};
        dist[i] = (float)sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    }
    qsort(dist, k, sizeof(float), compare_float);
    median = 0.5 * ((double)dist[k / 2u - 1u] + dist[k / 2u]);
    for (i = 0u; i < k; ++i) {
        const double d[3] = {((double)pos[i][0] - pos[0][0]) / median, ((double)pos[i][1] - pos[0][1]) / median,
                             ((double)pos[i][2] - pos[0][2]) / median};
        const double m[3] = {normal[i][0], normal[i][1], normal[i][2]};
        len = sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
        feature[i][0] = (float)(d[0] * s[0] + d[1] * s[1] + d[2] * s[2]);
        feature[i][1] = (float)(d[0] * t[0] + d[1] * t[1] + d[2] * t[2]);
        feature[i][2] = (float)(d[0] * n0[0] + d[1] * n0[1] + d[2] * n0[2]);
        feature[i][3] = (float)((m[0] * s[0] + m[1] * s[1] + m[2] * s[2]) / len);
        feature[i][4] = (float)((m[0] * t[0] + m[1] * t[1] + m[2] * t[2]) / len);
        feature[i][5] = (float)((m[0] * n0[0] + m[1] * n0[1] + m[2] * n0[2]) / len);
    }

    w = linear(w, &feature[0][0], 6u, h, e, k);
    w = layer_norm(w, h, x, e, k);
    for (block = 0u; block < net->layers; ++block) {
        w = layer_norm(w, x, h, e, k);
        w = linear(w, h, e, q, e, k);
        w = linear(w, h, e, key, e, k);
        w = linear(w, h, e, val, e, k);
        /* Each head: weights softmax(q . key / sqrt(dim)) over the k points, applied to val; the result goes back into h. */
        for (head = 0u; head < net->heads; ++head)
            for (i = 0u; i < k; ++i) {
                float best = -1e30f, sum = 0.0f;
                for (j = 0u; j < k; ++j) {
                    float dot = 0.0f;
                    for (c = 0u; c < dim; ++c) dot += q[(size_t)i * e + head * dim + c] * key[(size_t)j * e + head * dim + c];
                    weight[j] = dot / sqrtf((float)dim);
                    if (weight[j] > best) best = weight[j];
                }
                for (j = 0u; j < k; ++j) { weight[j] = expf(weight[j] - best); sum += weight[j]; }
                for (c = 0u; c < dim; ++c) {
                    float mix = 0.0f;
                    for (j = 0u; j < k; ++j) mix += weight[j] * val[(size_t)j * e + head * dim + c];
                    h[(size_t)i * e + head * dim + c] = mix / sum;
                }
            }
        w = linear(w, h, e, q, e, k);
        for (i = 0u; i < k * e; ++i) x[i] += q[i];
        w = layer_norm(w, x, h, e, k);
        w = linear(w, h, e, wide, net->mlp, k);
        for (i = 0u; i < k * net->mlp; ++i) {   /* GELU, the tanh form */
            const float v = wide[i];
            wide[i] = 0.5f * v * (1.0f + tanhf(0.7978845608f * (v + 0.044715f * v * v * v)));
        }
        w = linear(w, wide, net->mlp, q, e, k);
        for (i = 0u; i < k * e; ++i) x[i] += q[i];
    }
    w = layer_norm(w, x, h, e, 1u);       /* only the centre token is read */
    linear(w, h, e, out, 6u, 1u);
    /* Back to the cloud's units: a height, two slopes, three curvatures. */
    coef[0] = out[0] * median;
    coef[1] = out[1];
    coef[2] = out[2];
    coef[3] = out[3] / median;
    coef[4] = out[4] / median;
    coef[5] = out[5] / median;
    free(x);
    free(wide);
}

/* ---- the torus at a point (section 4.1) ---------------------------------- */

void pat_fit_torus(const float pos[3], const float normal[3], const double coef[6], PatPoint *out, PatTorus *paper) {
    const double a00 = coef[0], a01 = coef[1], a10 = coef[2], a11 = coef[3], a02 = coef[4], a20 = coef[5];
    const double area = sqrt(1.0 + a01 * a01 + a10 * a10);
    /* First and second fundamental forms of Q*(s, t) = p + s s + t t + Q(s, t) n at the origin. */
    const double i11 = 1.0 + a10 * a10, i12 = a10 * a01, i22 = 1.0 + a01 * a01;
    const double ii11 = 2.0 * a20 / area, ii12 = a11 / area, ii22 = 2.0 * a02 / area;
    const double mean = (a02 * i11 + a20 * i22 - a11 * a10 * a01) / (area * area * area);          /* (19) */
    const double gauss = (4.0 * a02 * a20 - a11 * a11) / (area * area * area * area);
    const double root = sqrt(mean * mean - gauss > 0.0 ? mean * mean - gauss : 0.0);
    const double kappa[2] = {mean + root, mean - root};
    double n[3], s[3], t[3], star[3], v[2][3], len, kmin, kmax, sign, side;
    uint32_t c, which, lo;

    len = sqrt((double)normal[0] * normal[0] + (double)normal[1] * normal[1] + (double)normal[2] * normal[2]);
    for (c = 0u; c < 3u; ++c) n[c] = normal[c] / len;
    pat_frame(n, s, t);
    /* Principal directions: (II - kappa I) w = 0, read off whichever row is larger (20). At an umbilic
     * both rows vanish; any direction does, the torus is a sphere. */
    for (which = 0u; which < 2u; ++which) {
        const double r1[2] = {ii11 - kappa[which] * i11, ii12 - kappa[which] * i12};
        const double r2[2] = {ii12 - kappa[which] * i12, ii22 - kappa[which] * i22};
        double w[2];
        if (r1[0] * r1[0] + r1[1] * r1[1] >= r2[0] * r2[0] + r2[1] * r2[1]) { w[0] = -r1[1]; w[1] = r1[0]; }
        else { w[0] = r2[1]; w[1] = -r2[0]; }
        if (w[0] * w[0] + w[1] * w[1] < 1e-24) { w[0] = which == 0u ? 1.0 : 0.0; w[1] = which == 0u ? 0.0 : 1.0; }
        for (c = 0u; c < 3u; ++c) v[which][c] = w[0] * (s[c] + a10 * n[c]) + w[1] * (t[c] + a01 * n[c]);
        len = sqrt(v[which][0] * v[which][0] + v[which][1] * v[which][1] + v[which][2] * v[which][2]);
        for (c = 0u; c < 3u; ++c) v[which][c] /= len;
    }
    lo   = fabs(kappa[0]) < fabs(kappa[1]) ? 0u : 1u;
    kmin = kappa[lo];
    kmax = kappa[1u - lo];
    /* The paper's sign rule: +1 when the larger curvature bends away from the normal. */
    sign = kmax < 0.0 ? 1.0 : -1.0;
    side = kmax * kmin < 0.0 ? -1.0 : 1.0;
    for (c = 0u; c < 3u; ++c) star[c] = (n[c] - a10 * s[c] - a01 * t[c]) / area;                    /* (21) */

    for (c = 0u; c < 3u; ++c) {
        out->pos[c]    = pos[c];
        out->normal[c] = (float)n[c];
        out->anchor[c] = (float)(pos[c] + a00 * n[c]);
        out->spoke[c]    = (float)(sign * star[c]);      /* (22): the centre is at anchor - sign / |k_min| n* */
    }
    out->axis[0] = (float)(star[1] * v[lo][2] - star[2] * v[lo][1]);
    out->axis[1] = (float)(star[2] * v[lo][0] - star[0] * v[lo][2]);
    out->axis[2] = (float)(star[0] * v[lo][1] - star[1] * v[lo][0]);
    out->kmin = (float)fabs(kmin);
    out->kmax = (float)fabs(kmax);
    out->sign = (float)sign;
    out->side = (float)side;
    out->pad  = 0.0f;
    if (paper) {
        const double tiny = 1e-300;
        paper->minor = 1.0 / (fabs(kmax) + tiny);
        paper->major = 1.0 / (fabs(kmin) + tiny) - side * paper->minor;
        paper->sign  = sign;
        paper->curvature[0] = kmin;
        paper->curvature[1] = kmax;
        for (c = 0u; c < 3u; ++c) {
            paper->centre[c] = pos[c] + a00 * n[c] - sign / (fabs(kmin) + tiny) * star[c];
            paper->axis[c]   = out->axis[c];
        }
    }
}

/* Equation 23, |d| - r with d = (rho - R, axial), written around the anchor.
 *
 * With y = x - anchor, a = y . u, y' = y - a u, b = spoke . y', k = 1/rho0 the
 * equator's curvature and m = 1/r the tube's:
 *
 *   rho - rho0 = delta = (2 b + k |y'|^2) / (sqrt(1 + 2 k b + k^2 |y'|^2) + 1)
 *   rho - R    = delta + side r                      (rho0 = R + side r)
 *   |d| - r    = (m (delta^2 + a^2) + 2 side delta) / (sqrt((m delta + side)^2 + m^2 a^2) + 1)
 *
 * Both are (A^2 - B^2) / (A + B) for the differences |A| - |B| of equation
 * 23, so k = 0 (a cylinder) and m = 0 (a plane) are ordinary inputs. */
double pat_torus_distance(const PatPoint *p, const double x[3], double gradient[3]) {
    const double y[3] = {x[0] - p->anchor[0], x[1] - p->anchor[1], x[2] - p->anchor[2]};
    const double a = y[0] * p->axis[0] + y[1] * p->axis[1] + y[2] * p->axis[2];
    const double f[3] = {y[0] - a * p->axis[0], y[1] - a * p->axis[1], y[2] - a * p->axis[2]};
    const double b = f[0] * p->spoke[0] + f[1] * p->spoke[1] + f[2] * p->spoke[2];
    const double ff = f[0] * f[0] + f[1] * f[1] + f[2] * f[2];
    const double k = p->kmin, m = p->kmax, side = p->side;
    const double radial = sqrt(1.0 + 2.0 * k * b + k * k * ff);
    const double delta = (2.0 * b + k * ff) / (radial + 1.0);
    const double tube = sqrt((m * delta + side) * (m * delta + side) + m * m * a * a);
    if (gradient) {
        /* The unit vector from the nearest point of the tube's centre circle. */
        uint32_t c;
        for (c = 0u; c < 3u; ++c)
            gradient[c] = p->sign * ((m * delta + side) * (p->spoke[c] + k * f[c]) / (radial > 1e-30 ? radial : 1e-30) + m * a * p->axis[c]) /
                          (tube > 1e-30 ? tube : 1e-30);
    }
    return p->sign * (m * (delta * delta + a * a) + 2.0 * side * delta) / (tube + 1.0);
}

/* ---- the distance (section 4.2) ------------------------------------------ */

double pat_distance(const PatPoint *points, uint32_t count, const double x[3], int method, double gradient[3]) {
    uint32_t near[PAT_NEAREST], used = 0u, worst = 0u, i, j, c;
    double   dist[PAT_NEAREST], reach = 0.0, shift, lambda, total = 0.0, value = 0.0, lean[3] = {0, 0, 0}, pull[3] = {0, 0, 0};

    /* The nearest points, unordered; `worst` is the farthest of them. */
    for (i = 0u; i < count; ++i) {
        const double d[3] = {x[0] - points[i].pos[0], x[1] - points[i].pos[1], x[2] - points[i].pos[2]};
        const double r = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (used < PAT_NEAREST) {
            near[used] = i; dist[used] = r;
            if (r > reach) { reach = r; worst = used; }
            ++used;
        } else if (r < reach) {
            near[worst] = i; dist[worst] = r;
            reach = 0.0;
            for (j = 0u; j < used; ++j)
                if (dist[j] > reach) { reach = dist[j]; worst = j; }
        }
    }
    if (method == PAT_SNAP) {
        uint32_t best = 0u;
        for (j = 1u; j < used; ++j)
            if (dist[j] < dist[best]) best = j;
        return pat_torus_distance(&points[near[best]], x, gradient);
    }
    /* (26): the weights are exp(-lambda (|x - p| - shift)) with shift half the farthest distance and
     * lambda = C / shift, so the exponents run from +C at the query to -C at the farthest point. */
    shift  = 0.5 * reach;
    lambda = PAT_C / shift;
    for (j = 0u; j < used; ++j) {
        const PatPoint *p = &points[near[j]];
        const double    d[3] = {x[0] - p->pos[0], x[1] - p->pos[1], x[2] - p->pos[2]};
        const double    w = exp(-lambda * (dist[j] - shift));
        double g, dg[3];
        if (method == PAT_PLANES) {
            g = d[0] * p->normal[0] + d[1] * p->normal[1] + d[2] * p->normal[2];
            dg[0] = p->normal[0]; dg[1] = p->normal[1]; dg[2] = p->normal[2];
        } else g = pat_torus_distance(p, x, dg);
        total += w;
        value += w * g;
        /* Appendix D: grad = sum w (grad g - lambda r^ g) / sum w + value * lambda sum w r^ / sum w,
         * with lambda held fixed (the released code does the same). */
        for (c = 0u; c < 3u; ++c) {
            const double toward = dist[j] > 1e-30 ? d[c] / dist[j] : 0.0;
            lean[c] += w * (dg[c] - lambda * toward * g);
            pull[c] += w * lambda * toward;
        }
    }
    value /= total;
    if (gradient)
        for (c = 0u; c < 3u; ++c) gradient[c] = lean[c] / total + value * pull[c] / total;
    return value;
}
