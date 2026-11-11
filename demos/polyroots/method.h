/* method.h — real roots of a polynomial in an interval by monotonic pieces and safeguarded
 * Newton iterations (Yuksel 2022). See method.c. */
#ifndef POLYROOTS_METHOD_H
#define POLYROOTS_METHOD_H

#define PR_MOST 24   /* highest degree */

typedef struct PrWork {
    long pieces, searched;   /* monotonic pieces looked at; those with a sign change */
    long newton, bisection;  /* iterations; how many of them fell back to halving the interval */
} PrWork;

/* The real roots of c[0] + c[1] x + .. + c[degree] x^degree in from .. to, ascending, into
 * `roots` (room for `degree`); returns how many. An iteration stops when its Newton step is
 * at most `epsilon`; 0 asks for all the precision a double has. Adds to *work. */
int pr_roots(const double *c, int degree, double from, double to, double epsilon, double *roots, PrWork *work);

#endif /* POLYROOTS_METHOD_H */
