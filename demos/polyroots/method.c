/* method.c — "High-Performance Polynomial Root Finding for Graphics" (Cem Yuksel,
 * Proc. ACM Comput. Graph. Interact. Tech. 5(3), HPG 2022), for a finite interval.
 *
 * Between two neighbouring zeros of its derivative a polynomial is monotonic, so it has at
 * most one root there, and has one exactly when its values at the two ends differ in sign
 * (section 3.1). So: find the roots of the derivative (the same problem, one degree lower,
 * down to a quadratic that is solved by its formula; section 4), cut the interval at them,
 * and search only the pieces with a sign change.
 *
 * The search is section 3.3's: a Newton step from the middle of the piece; every point that
 * is evaluated becomes an end of the interval that still holds the root (eq. 6); a step that
 * does not land strictly inside that interval is replaced by its midpoint (eq. 7). It stops
 * when the Newton step is at most epsilon.
 *
 * Not here: the paper's faster path for cubics (a fixed number of unsafeguarded steps after a
 * split at the inflection point, section 3.2), deflation (3.4), the extra step that bounds the
 * error by epsilon for higher degrees (eq. 10), infinite intervals (section 5).
 */
#include "method.h"

#include <math.h>

int pr_roots(const double *c, int degree, double from, double to, double epsilon, double *roots, PrWork *work) {
    double slope[PR_MOST], edge[PR_MOST + 1];
    int    pieces, found = 0, i, k;

    while (degree > 0 && c[degree] == 0.0) --degree;
    if (degree == 0) return 0;
    if (degree == 1) {
        roots[0] = -c[0] / c[1];
        return roots[0] >= from && roots[0] <= to;
    }
    if (degree == 2) {
        /* the formula in the form that does not subtract nearly equal numbers */
        const double disc = c[1] * c[1] - 4.0 * c[2] * c[0], q = -0.5 * (c[1] + (c[1] < 0.0 ? -1.0 : 1.0) * sqrt(fmax(disc, 0.0)));
        const double r0 = q / c[2], r1 = q != 0.0 ? c[0] / q : r0, low = fmin(r0, r1), high = fmax(r0, r1);
        if (disc < 0.0) return 0;
        if (low >= from && low <= to) roots[found++] = low;
        if (high >= from && high <= to && disc > 0.0) roots[found++] = high;
        return found;
    }

    for (k = 1; k <= degree; ++k) slope[k - 1] = k * c[k];
    pieces = 1 + pr_roots(slope, degree - 1, from, to, epsilon, edge + 1, work);
    edge[0] = from; edge[pieces] = to;

    for (i = 0; i < pieces; ++i) {
        double lo = edge[i], hi = edge[i + 1], flo = c[degree], fhi = c[degree], x = 0.5 * (lo + hi);
        for (k = degree - 1; k >= 0; --k) { flo = flo * lo + c[k]; fhi = fhi * hi + c[k]; }
        work->pieces += 1;
        if ((flo < 0.0) == (fhi < 0.0)) continue;
        work->searched += 1;
        for (;;) {
            double f = c[degree], df = slope[degree - 1], next;
            for (k = degree - 1; k >= 0; --k) f = f * x + c[k];
            for (k = degree - 2; k >= 0; --k) df = df * x + slope[k];
            if ((f < 0.0) == (flo < 0.0)) lo = x; else hi = x;   /* eq. 6 */
            next = x - f / df;                                    /* eq. 3 */
            work->newton += 1;
            if (fabs(next - x) <= epsilon) { x = fmin(fmax(next, lo), hi); break; }
            if (!(next > lo && next < hi)) {                      /* eq. 7 */
                next = 0.5 * (lo + hi);
                work->bisection += 1;
                if (next == lo || next == hi) { x = next; break; }
            }
            x = next;
        }
        roots[found++] = x;
    }
    return found;
}
