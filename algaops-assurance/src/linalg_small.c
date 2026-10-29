/*
 * linalg_small.c — Cholesky factorisation and triangular solve.
 */

#include "linalg_small.h"

#include <math.h>

int linalg_cholesky(double *a, int n)
{
    if (!a || n < 1 || n > LINALG_MAX_N) return -2;
    for (int i = 0; i < n * n; i++) {
        if (!isfinite(a[i])) return -1;
    }
    for (int j = 0; j < n; j++) {
        double d = a[j * n + j];
        for (int k = 0; k < j; k++) d -= a[j * n + k] * a[j * n + k];
        if (!(d > 0.0) || !isfinite(d)) return -1;
        double ljj = sqrt(d);
        a[j * n + j] = ljj;
        for (int i = j + 1; i < n; i++) {
            double s = a[i * n + j];
            for (int k = 0; k < j; k++) s -= a[i * n + k] * a[j * n + k];
            a[i * n + j] = s / ljj;
        }
        for (int i = 0; i < j; i++) a[i * n + j] = 0.0;  /* clear upper part */
    }
    return 0;
}

void linalg_forward_solve(const double *l, int n, const double *b, double *y)
{
    for (int i = 0; i < n; i++) {
        double s = b[i];
        for (int k = 0; k < i; k++) s -= l[i * n + k] * y[k];
        y[i] = s / l[i * n + i];
    }
}

double linalg_mahalanobis_sq(const double *l, int n, const double *d)
{
    double y[LINALG_MAX_N];
    linalg_forward_solve(l, n, d, y);
    double s = 0.0;
    for (int i = 0; i < n; i++) s += y[i] * y[i];
    return s;
}
