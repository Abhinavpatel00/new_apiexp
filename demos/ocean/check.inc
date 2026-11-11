/* check.inc — ocean's acceptance test; included by main.c.
 *
 * One wave against its closed form; then the full spectrum: the spectrum pass
 * against the same formula on the CPU, and the transform against a direct
 * (not fast) Fourier sum of the spectrum the GPU produced.
 */

/* An n x n grid of four floats. */
static float *read_target(App *a, Target t) {
    const size_t bytes = (size_t)a->n * a->n * 4u * sizeof(float);
    float       *out   = (float *)malloc(bytes);
    demo_readback(&a->demo, t, out, bytes);
    return out;
}

static void run_frame(App *a, float time) {
    a->manual = true; a->manual_time = time;
    app_frame(a);
}

/* out[x] = sum over j of in[j] e^(+2 pi i j x / n), along one axis of an n x n complex field; O(n^3) in all. */
static void slow_transform(const double *in, double *out, uint32_t n, bool vertical, const double *table) {
    uint32_t line, x, j;
    for (line = 0u; line < n; ++line)
        for (x = 0u; x < n; ++x) {
            double re = 0.0, im = 0.0;
            for (j = 0u; j < n; ++j) {
                const double *v = in + (size_t)(vertical ? line + j * n : j + line * n) * 2u;
                const double *w = table + (size_t)((uint64_t)j * x % n) * 2u;
                re += v[0] * w[0] - v[1] * w[1];
                im += v[0] * w[1] + v[1] * w[0];
            }
            out[(size_t)(vertical ? line + x * n : x + line * n) * 2u]      = re;
            out[(size_t)(vertical ? line + x * n : x + line * n) * 2u + 1u] = im;
        }
}

static void app_check(App *a) {
    static const float k_times[2] = {0.7f, 12.9f};
    Demo          *d = &a->demo;
    const uint32_t n = a->n;
    const size_t   cells = (size_t)n * n;
    double        *table = (double *)malloc((size_t)n * 2u * sizeof(double));
    double        *in = (double *)malloc(cells * 2u * sizeof(double)), *mid = (double *)malloc(cells * 2u * sizeof(double));
    double        *out = (double *)malloc(cells * 2u * sizeof(double));
    char     label[160];
    uint32_t t, x, y, c;
    size_t   i;

    for (x = 0u; x < n; ++x) {
        table[x * 2u] = cos(6.283185307179586 * x / n); table[x * 2u + 1u] = sin(6.283185307179586 * x / n);
    }

    /* One wave, wave number (3, 2), h0 = 0.25 + 0.10 i: height = 2 Re(h0 e^(i (k.x - w t))). */
    a->single = true;
    make_spectrum(a);
    demo_upload(d, a->h0, a->h0_cpu, cells * 16u);
    for (t = 0u; t < 2u; ++t) {
        const double kx = 6.283185307179586 * 3.0 / a->patch, kz = 6.283185307179586 * 2.0 / a->patch, len = sqrt(kx * kx + kz * kz);
        const double w = sqrt(GRAVITY * len);
        double worst[3] = {0.0, 0.0, 0.0}, spare = 0.0;
        float *f;
        run_frame(a, k_times[t]);
        f = read_target(a, a->field[1]);
        for (y = 0u; y < n; ++y)
            for (x = 0u; x < n; ++x) {
                const double  phase = 6.283185307179586 * (3.0 * x + 2.0 * y) / n - w * (double)k_times[t];
                const double  re = 0.25 * cos(phase) - 0.10 * sin(phase), im = 0.25 * sin(phase) + 0.10 * cos(phase);
                const double  want[3] = {2.0 * re, 2.0 * im * kx / len, 2.0 * im * kz / len};
                const float  *g = f + (size_t)(x + y * n) * 4u;
                for (c = 0u; c < 3u; ++c)
                    if (fabs(g[c] - want[c]) > worst[c]) worst[c] = fabs(g[c] - want[c]);
                if (fabs(g[3]) > spare) spare = fabs(g[3]);
            }
        free(f);
        snprintf(label, sizeof label, "one wave, t = %.1f s: height vs 2 Re(h0 e^(i(k.x - wt))), worst (amplitude 0.54)", (double)k_times[t]);
        demo_check(d, label, worst[0], "< 2e-6", worst[0] < 2e-6);
        snprintf(label, sizeof label, "one wave, t = %.1f s: displacement x, worst", (double)k_times[t]);
        demo_check(d, label, worst[1], "< 2e-6", worst[1] < 2e-6);
        snprintf(label, sizeof label, "one wave, t = %.1f s: displacement z, worst", (double)k_times[t]);
        demo_check(d, label, worst[2], "< 2e-6", worst[2] < 2e-6);
        snprintf(label, sizeof label, "one wave, t = %.1f s: the unused fourth field, largest", (double)k_times[t]);
        demo_check(d, label, spare, "< 2e-6", spare < 2e-6);
    }

    /* The full spectrum. */
    a->single = false;
    make_spectrum(a);
    demo_upload(d, a->h0, a->h0_cpu, cells * 16u);
    for (t = 0u; t < 2u; ++t) {
        float *spectrum, *field;
        double worst_spectrum = 0.0, largest = 0.0, worst[3] = {0.0, 0.0, 0.0}, square = 0.0, sum = 0.0, squeeze = 0.0;
        run_frame(a, k_times[t]);
        spectrum = read_target(a, a->spectrum);
        field = read_target(a, a->field[1]);

        /* The spectrum pass against the formula, in double. */
        for (y = 0u; y < n; ++y)
            for (x = 0u; x < n; ++x) {
                const float *h0 = a->h0_cpu + (size_t)(x + y * n) * 4u, *g = spectrum + (size_t)(x + y * n) * 4u;
                const double kx = 6.283185307179586 * wave_number(a, x) / a->patch, kz = 6.283185307179586 * wave_number(a, y) / a->patch;
                const double len = sqrt(kx * kx + kz * kz), angle = -sqrt(GRAVITY * len) * (double)k_times[t];
                const double cs = cos(angle), sn = sin(angle);
                const double hr = h0[0] * cs - h0[1] * sn + h0[2] * cs - h0[3] * sn;       /* h0 e^(ia) + conj(h0') e^(-ia) */
                const double hi = h0[0] * sn + h0[1] * cs - h0[2] * sn - h0[3] * cs;
                const bool   odd = len > 0.0 && x != n / 2u && y != n / 2u;
                const double dxr = odd ? hi * kx / len : 0.0, dxi = odd ? -hr * kx / len : 0.0;   /* -i k/|k| H */
                const double dzr = odd ? hi * kz / len : 0.0, dzi = odd ? -hr * kz / len : 0.0;
                const double want[4] = {hr - dxi, hi + dxr, dzr, dzi};
                for (c = 0u; c < 4u; ++c) {
                    if (fabs(g[c] - want[c]) > worst_spectrum) worst_spectrum = fabs(g[c] - want[c]);
                    if (fabs(want[c]) > largest) largest = fabs(want[c]);
                }
            }
        snprintf(label, sizeof label, "t = %.1f s: spectrum pass vs the formula in double, worst / largest coefficient", (double)k_times[t]);
        demo_check(d, label, worst_spectrum / largest, "< 1e-5", worst_spectrum < 1e-5 * largest);

        /* The transform: a direct Fourier sum of what the spectrum pass wrote. Two complex fields. */
        for (c = 0u; c < 2u; ++c) {
            for (i = 0u; i < cells; ++i) { in[i * 2u] = spectrum[i * 4u + c * 2u]; in[i * 2u + 1u] = spectrum[i * 4u + c * 2u + 1u]; }
            slow_transform(in, mid, n, false, table);
            slow_transform(mid, out, n, true, table);
            for (i = 0u; i < cells; ++i) {
                const double e0 = fabs(field[i * 4u + c * 2u] - out[i * 2u]), e1 = fabs(field[i * 4u + c * 2u + 1u] - out[i * 2u + 1u]);
                if (c == 0u) { if (e0 > worst[0]) worst[0] = e0; if (e1 > worst[1]) worst[1] = e1; }
                else { if (e0 > worst[2]) worst[2] = e0; if (e1 > squeeze) squeeze = e1; }
            }
        }
        for (i = 0u; i < cells; ++i) { square += (double)field[i * 4u] * field[i * 4u]; sum += field[i * 4u]; }
        snprintf(label, sizeof label, "t = %.1f s: height vs a direct Fourier sum, worst (m)", (double)k_times[t]);
        demo_check(d, label, worst[0], "< 2e-6", worst[0] < 2e-6);
        snprintf(label, sizeof label, "t = %.1f s: displacement x and z vs a direct Fourier sum, worst (m)", (double)k_times[t]);
        demo_check(d, label, worst[1] > worst[2] ? worst[1] : worst[2], "< 2e-6", worst[1] < 2e-6 && worst[2] < 2e-6);
        snprintf(label, sizeof label, "t = %.1f s: the unused fourth field vs a direct Fourier sum, worst", (double)k_times[t]);
        demo_check(d, label, squeeze, "< 2e-6", squeeze < 2e-6);
        snprintf(label, sizeof label, "t = %.1f s: height, standard deviation (m; asked for %.2f)", (double)k_times[t], (double)a->height);
        demo_check(d, label, sqrt(square / (double)cells), "within 25 %",
                         fabs(sqrt(square / (double)cells) - (double)a->height) < 0.25 * (double)a->height);
        snprintf(label, sizeof label, "t = %.1f s: height, mean (m)", (double)k_times[t]);
        demo_check(d, label, sum / (double)cells, "|x| < 1e-6", fabs(sum / (double)cells) < 1e-6);
        free(spectrum); free(field);
    }
    a->manual = false;
    free(table); free(in); free(mid); free(out);
}
