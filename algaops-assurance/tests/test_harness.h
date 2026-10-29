/*
 * test_harness.h — minimal self-contained assertion harness for the v2 test
 * suites (same conventions as the original v1 tests: each test function
 * returns 0 on pass, 1 on failure; main returns non-zero if any failed).
 */

#ifndef ALGAOPS_TEST_HARNESS_H
#define ALGAOPS_TEST_HARNESS_H

#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef ALGAOPS_SOURCE_DIR
#define ALGAOPS_SOURCE_DIR "."
#endif

static int g_tests_run    = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define ASSERT(cond, msg)                                        \
    do {                                                          \
        if (!(cond)) {                                            \
            fprintf(stderr, "  FAIL [%s:%d] %s\n",               \
                    __FILE__, __LINE__, (msg));                    \
            return 1;                                             \
        }                                                         \
    } while (0)

#define ASSERT_NEAR(a, b, tol, msg)                              \
    do {                                                          \
        double a_ = (double)(a), b_ = (double)(b);                \
        if (!(fabs(a_ - b_) <= (tol))) {                          \
            fprintf(stderr, "  FAIL [%s:%d] %s (got %.12g, want %.12g)\n", \
                    __FILE__, __LINE__, (msg), a_, b_);           \
            return 1;                                             \
        }                                                         \
    } while (0)

#define RUN_TEST(fn)                                             \
    do {                                                          \
        g_tests_run++;                                            \
        printf("  %-60s ", #fn);                                  \
        fflush(stdout);                                           \
        int rc_ = fn();                                           \
        if (rc_ == 0) { printf("PASS\n"); g_tests_passed++; }    \
        else          { printf("FAIL\n"); g_tests_failed++; }     \
    } while (0)

#define TEST_SUMMARY()                                           \
    do {                                                          \
        printf("\nResults: %d/%d passed", g_tests_passed, g_tests_run); \
        if (g_tests_failed > 0) printf(" (%d FAILED)", g_tests_failed); \
        printf("\n");                                             \
    } while (0)

#define TEST_EXIT_CODE() ((g_tests_failed == 0 && g_tests_run > 0) ? 0 : 1)

/* Build a path below the source tree. */
static inline const char *src_path(char *buf, size_t len, const char *rel)
{
    snprintf(buf, len, "%s/%s", ALGAOPS_SOURCE_DIR, rel);
    return buf;
}

#endif /* ALGAOPS_TEST_HARNESS_H */
