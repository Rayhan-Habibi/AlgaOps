/*
 * test_trustguard.c — Unit tests for the TrustGuard module.
 *
 * Test framework: minimal self-contained assertions.
 * Each test function returns 0 on pass, 1 on failure.
 * All test cases assert actual return values and output fields.
 */

#include "../include/algaops_types.h"
#include "../include/trustguard.h"

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>

/* =========================================================================
 * Minimal test framework
 * ========================================================================= */

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

#define RUN_TEST(fn)                                             \
    do {                                                          \
        g_tests_run++;                                            \
        printf("  %-50s ", #fn);                                  \
        int rc = fn();                                            \
        if (rc == 0) { printf("PASS\n"); g_tests_passed++; }     \
        else         { printf("FAIL\n"); g_tests_failed++; }      \
    } while (0)

/* =========================================================================
 * Helper: build a valid baseline record
 * ========================================================================= */
static AlgaRecord make_record(uint64_t ts, uint32_t seq,
                               double vib, double temp)
{
    AlgaRecord r;
    r.timestamp_ms    = ts;
    r.sequence_id     = seq;
    r.vibration_rms_g = vib;
    r.temperature_c   = temp;
    return r;
}

/* =========================================================================
 * TG-01: Valid monotonically increasing samples
 * ========================================================================= */
static int test_valid_monotonic(void)
{
    TrustGuardConfig cfg = trustguard_default_config();
    TrustGuardState  state;
    ASSERT(trustguard_init(&state, &cfg) == ALGAOPS_OK, "init failed");

    TrustResult tr;
    AlgaRecord r;

    for (int i = 1; i <= 5; i++) {
        r = make_record((uint64_t)i * 1000, (uint32_t)i, 0.42 + i*0.01, 28.0 + i*0.1);
        ASSERT(trustguard_validate(&state, &r, &tr) == ALGAOPS_OK, "validate failed");
        ASSERT(tr.is_trusted,           "record should be trusted");
        ASSERT(tr.flags == INTEGRITY_OK, "flags should be OK");
        ASSERT(tr.trust_score == 100,    "score should be 100");
    }
    return 0;
}

/* =========================================================================
 * TG-02: Missing sequence IDs
 * ========================================================================= */
static int test_missing_sequence(void)
{
    TrustGuardConfig cfg = trustguard_default_config();
    TrustGuardState  state;
    ASSERT(trustguard_init(&state, &cfg) == ALGAOPS_OK, "init failed");

    TrustResult tr;
    AlgaRecord r;

    /* First packet: seq=1, should be clean. */
    r = make_record(1000, 1, 0.42, 28.0);
    ASSERT(trustguard_validate(&state, &r, &tr) == ALGAOPS_OK, "validate 1 failed");
    ASSERT(tr.flags == INTEGRITY_OK, "first record should be clean");

    /* Next packet: seq=4, skipping 2 and 3. */
    r = make_record(4000, 4, 0.43, 28.1);
    ASSERT(trustguard_validate(&state, &r, &tr) == ALGAOPS_OK, "validate 2 failed");
    ASSERT(tr.flags & INTEGRITY_MISSING_PACKET, "missing packet flag expected");
    /* Missing packet is informational, not blocking — record may be trusted. */
    ASSERT(tr.trust_score < 100, "score should be reduced");

    return 0;
}

/* =========================================================================
 * TG-03: Duplicate sequence IDs
 * ========================================================================= */
static int test_duplicate_sequence(void)
{
    TrustGuardConfig cfg = trustguard_default_config();
    TrustGuardState  state;
    ASSERT(trustguard_init(&state, &cfg) == ALGAOPS_OK, "init failed");

    TrustResult tr;
    AlgaRecord r;

    r = make_record(1000, 1, 0.42, 28.0);
    ASSERT(trustguard_validate(&state, &r, &tr) == ALGAOPS_OK, "validate 1 failed");

    /* Send the same sequence_id again. */
    r = make_record(2000, 1, 0.42, 28.0);
    ASSERT(trustguard_validate(&state, &r, &tr) == ALGAOPS_OK, "validate 2 failed");
    ASSERT(tr.flags & INTEGRITY_DUPLICATE_SEQ, "duplicate flag expected");
    ASSERT(!tr.is_trusted, "duplicate record must not be trusted");

    return 0;
}

/* =========================================================================
 * TG-04: Out-of-order sequence IDs
 * ========================================================================= */
static int test_out_of_order_sequence(void)
{
    TrustGuardConfig cfg = trustguard_default_config();
    TrustGuardState  state;
    ASSERT(trustguard_init(&state, &cfg) == ALGAOPS_OK, "init failed");

    TrustResult tr;
    AlgaRecord r;

    r = make_record(1000, 5, 0.42, 28.0);
    ASSERT(trustguard_validate(&state, &r, &tr) == ALGAOPS_OK, "validate 1 failed");

    /* Send a lower sequence ID. */
    r = make_record(2000, 3, 0.43, 28.1);
    ASSERT(trustguard_validate(&state, &r, &tr) == ALGAOPS_OK, "validate 2 failed");
    ASSERT(tr.flags & INTEGRITY_OUT_OF_ORDER_SEQ, "out-of-order flag expected");
    ASSERT(!tr.is_trusted, "out-of-order record must not be trusted");

    return 0;
}

/* =========================================================================
 * TG-05: Non-monotonic timestamps
 * ========================================================================= */
static int test_non_monotonic_timestamp(void)
{
    TrustGuardConfig cfg = trustguard_default_config();
    TrustGuardState  state;
    ASSERT(trustguard_init(&state, &cfg) == ALGAOPS_OK, "init failed");

    TrustResult tr;
    AlgaRecord r;

    r = make_record(5000, 1, 0.42, 28.0);
    ASSERT(trustguard_validate(&state, &r, &tr) == ALGAOPS_OK, "validate 1 failed");

    /* Send an older timestamp. */
    r = make_record(3000, 2, 0.43, 28.1);
    ASSERT(trustguard_validate(&state, &r, &tr) == ALGAOPS_OK, "validate 2 failed");
    ASSERT(tr.flags & INTEGRITY_BAD_TIMESTAMP, "bad timestamp flag expected");
    ASSERT(!tr.is_trusted, "bad timestamp record must not be trusted");

    /* Equal timestamp should also fail. */
    TrustGuardState state2;
    ASSERT(trustguard_init(&state2, &cfg) == ALGAOPS_OK, "init2 failed");
    r = make_record(5000, 1, 0.42, 28.0);
    ASSERT(trustguard_validate(&state2, &r, &tr) == ALGAOPS_OK, "validate A failed");
    r = make_record(5000, 2, 0.43, 28.1);
    ASSERT(trustguard_validate(&state2, &r, &tr) == ALGAOPS_OK, "validate B failed");
    ASSERT(tr.flags & INTEGRITY_BAD_TIMESTAMP, "equal timestamp flag expected");

    return 0;
}

/* =========================================================================
 * TG-06: Values outside configured ranges
 * ========================================================================= */
static int test_out_of_range(void)
{
    TrustGuardConfig cfg = trustguard_default_config();
    TrustGuardState  state;
    ASSERT(trustguard_init(&state, &cfg) == ALGAOPS_OK, "init failed");

    TrustResult tr;
    AlgaRecord r;

    /* Negative vibration is out of range (min=0). */
    r = make_record(1000, 1, -0.5, 28.0);
    ASSERT(trustguard_validate(&state, &r, &tr) == ALGAOPS_OK, "validate vib failed");
    ASSERT(tr.flags & INTEGRITY_VIB_OUT_OF_RANGE, "vib out-of-range expected");
    ASSERT(!tr.is_trusted, "out-of-range vib record must not be trusted");

    /* Temperature above max=120 °C. */
    ASSERT(trustguard_reset(&state) == ALGAOPS_OK, "reset failed");
    r = make_record(1000, 1, 0.42, 150.0);
    ASSERT(trustguard_validate(&state, &r, &tr) == ALGAOPS_OK, "validate temp failed");
    ASSERT(tr.flags & INTEGRITY_TEMP_OUT_OF_RANGE, "temp out-of-range expected");
    ASSERT(!tr.is_trusted, "out-of-range temp record must not be trusted");

    return 0;
}

/* =========================================================================
 * TG-07: Stuck vibration sensor
 * ========================================================================= */
static int test_stuck_vibration(void)
{
    TrustGuardConfig cfg = trustguard_default_config();
    cfg.stuck_window_size     = 5;
    cfg.stuck_vib_threshold_g = 0.005;
    TrustGuardState state;
    ASSERT(trustguard_init(&state, &cfg) == ALGAOPS_OK, "init failed");

    TrustResult tr;
    AlgaRecord r;

    /* Feed exactly stuck_window_size records with identical vibration. */
    for (int i = 1; i <= 5; i++) {
        r = make_record((uint64_t)i * 1000, (uint32_t)i,
                        0.420000,                /* identical: range=0 < threshold */
                        28.0 + (double)i * 0.1); /* temp varies enough */
        ASSERT(trustguard_validate(&state, &r, &tr) == ALGAOPS_OK,
               "validate stuck vib failed");
    }
    ASSERT(tr.flags & INTEGRITY_VIB_STUCK, "vib stuck flag expected after window fill");
    ASSERT(!tr.is_trusted, "stuck-vib record must not be trusted");

    return 0;
}

/* =========================================================================
 * TG-08: Malformed / non-finite values
 * ========================================================================= */
static int test_non_finite(void)
{
    TrustGuardConfig cfg = trustguard_default_config();
    TrustGuardState  state;
    ASSERT(trustguard_init(&state, &cfg) == ALGAOPS_OK, "init failed");

    TrustResult tr;
    AlgaRecord r;

    /* NaN vibration. */
    r = make_record(1000, 1, (double)NAN, 28.0);
    ASSERT(trustguard_validate(&state, &r, &tr) == ALGAOPS_OK, "validate NaN failed");
    ASSERT(tr.flags & INTEGRITY_NON_FINITE, "non-finite flag expected");
    ASSERT(!tr.is_trusted, "NaN record must not be trusted");
    ASSERT(tr.trust_score <= 50, "score should be severely penalised");

    /* Infinite temperature. */
    ASSERT(trustguard_reset(&state) == ALGAOPS_OK, "reset failed");
    r = make_record(1000, 1, 0.42, (double)INFINITY);
    ASSERT(trustguard_validate(&state, &r, &tr) == ALGAOPS_OK, "validate Inf failed");
    ASSERT(tr.flags & INTEGRITY_NON_FINITE, "non-finite flag for Inf expected");
    ASSERT(!tr.is_trusted, "Inf record must not be trusted");

    return 0;
}

/* =========================================================================
 * TG-09: Initialisation and reset behaviour
 * ========================================================================= */
static int test_init_and_reset(void)
{
    TrustGuardConfig cfg = trustguard_default_config();
    TrustGuardState  state;

    /* NULL pointer checks. */
    ASSERT(trustguard_init(NULL, &cfg) == ALGAOPS_ERR_NULL_PTR, "null state check");
    ASSERT(trustguard_init(&state, NULL) == ALGAOPS_ERR_NULL_PTR, "null cfg check");
    ASSERT(trustguard_reset(NULL) == ALGAOPS_ERR_NULL_PTR, "null reset check");

    ASSERT(trustguard_init(&state, &cfg) == ALGAOPS_OK, "init failed");
    ASSERT(!state.has_previous, "fresh state should have no previous record");

    /* Validate one record so state is populated. */
    TrustResult tr;
    AlgaRecord r = make_record(1000, 1, 0.42, 28.0);
    ASSERT(trustguard_validate(&state, &r, &tr) == ALGAOPS_OK, "validate failed");
    ASSERT(state.has_previous, "state should record prev after first validate");

    /* Reset should clear has_previous. */
    ASSERT(trustguard_reset(&state) == ALGAOPS_OK, "reset failed");
    ASSERT(!state.has_previous, "state.has_previous should be false after reset");
    ASSERT(state.window_count == 0, "window should be empty after reset");

    /* After reset, first record should be clean again. */
    r = make_record(500, 1, 0.42, 28.0);   /* reuse seq_id=1 (allowed after reset) */
    ASSERT(trustguard_validate(&state, &r, &tr) == ALGAOPS_OK, "validate after reset failed");
    ASSERT(tr.flags == INTEGRITY_OK, "first record after reset should be clean");
    ASSERT(tr.is_trusted,             "first record after reset should be trusted");

    return 0;
}

/* =========================================================================
 * TG-10: Stuck temperature sensor
 * ========================================================================= */
static int test_stuck_temperature(void)
{
    TrustGuardConfig cfg = trustguard_default_config();
    cfg.stuck_window_size      = 5;
    cfg.stuck_temp_threshold_c = 0.05;
    TrustGuardState state;
    ASSERT(trustguard_init(&state, &cfg) == ALGAOPS_OK, "init failed");

    TrustResult tr;
    AlgaRecord r;

    for (int i = 1; i <= 5; i++) {
        r = make_record((uint64_t)i * 1000, (uint32_t)i,
                        0.42 + (double)i * 0.01, /* vibration varies */
                        28.000);                  /* temp identical */
        ASSERT(trustguard_validate(&state, &r, &tr) == ALGAOPS_OK,
               "validate stuck temp failed");
    }
    ASSERT(tr.flags & INTEGRITY_TEMP_STUCK, "temp stuck flag expected");
    ASSERT(!tr.is_trusted, "stuck-temp record must not be trusted");

    return 0;
}

/* =========================================================================
 * Entry point
 * ========================================================================= */
int main(void)
{
    printf("=== TrustGuard Unit Tests ===\n");

    RUN_TEST(test_valid_monotonic);
    RUN_TEST(test_missing_sequence);
    RUN_TEST(test_duplicate_sequence);
    RUN_TEST(test_out_of_order_sequence);
    RUN_TEST(test_non_monotonic_timestamp);
    RUN_TEST(test_out_of_range);
    RUN_TEST(test_stuck_vibration);
    RUN_TEST(test_non_finite);
    RUN_TEST(test_init_and_reset);
    RUN_TEST(test_stuck_temperature);

    printf("\nResults: %d/%d passed", g_tests_passed, g_tests_run);
    if (g_tests_failed > 0) printf(" (%d FAILED)", g_tests_failed);
    printf("\n");

    return (g_tests_failed == 0) ? 0 : 1;
}
