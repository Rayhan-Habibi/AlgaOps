/**
 * @file linalg_small.h
 * @brief Tiny dense linear algebra for the regularized Mahalanobis method.
 *
 * Row-major n×n matrices with n <= LINALG_MAX_N.  No allocation.
 */

#ifndef LINALG_SMALL_H
#define LINALG_SMALL_H

#define LINALG_MAX_N 8

/**
 * @brief In-place Cholesky factorisation A = L·Lᵀ (lower triangle kept).
 * @return 0 on success, -1 if A is not symmetric positive definite or
 *         contains non-finite values, -2 on invalid size.
 */
int linalg_cholesky(double *a, int n);

/** Solve L·y = b (forward substitution) with the factor from linalg_cholesky. */
void linalg_forward_solve(const double *l, int n, const double *b, double *y);

/** Squared Mahalanobis distance dᵀ·A⁻¹·d using the Cholesky factor of A. */
double linalg_mahalanobis_sq(const double *l, int n, const double *d);

#endif /* LINALG_SMALL_H */
