/*
 * test_equipment_health.c — Unit tests for the Equipment Health module.
 *
 * Tests use in-memory baseline construction (via a temporary file) to avoid
 * depending on the CSV path from the build directory.
 */

#include "../include/algaops_types.h"
#include "../include/trustguard.h"
#include "../include/equipment_health.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <stdbool.h>

#ifdef _WIN32
#include <io.h>
#define TMPFILE_FUNC  _tempnam
#else
#include <unistd.h>
#define TMPFILE_FUNC  tempnam
#endif

/* =========================================================================
 * Minimal test framework (identical to test_trustguard.c)
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
        printf("  %-55s ", #fn);                                  \
        int rc = fn();                                            \
        if (rc == 0) { printf("PASS\n"); g_tests_passed++; }     \
        else         { printf("FAIL\n"); g_tests_failed++; }      \
    } while (0)

/* =========================================================================
 * Helper: write a temporary CSV file with synthetic baseline data.
 * Returns the path; caller must free the path string and delete the file.
 * ========================================================================= */
static char *write_temp_baseline_csv(const char *csv_content)
{
    char *path = TMPFILE_FUNC(NULL, "algaops_test_");
    if (!path) return NULL;

    FILE *f = fopen(path, "w");
    if (!f) { free(path); return NULL; }
    fputs(csv_content, f);
    fclose(f);
    return path;
}

/** Build a CSV string with 20 normal-operation records. */
static const char BASELINE_CSV_NORMAL[] =
    "timestamp_ms,sequence_id,vibration_rms_g,temperature_c\n"
    "1000,1,0.420,28.1\n"
    "2000,2,0.415,28.2\n"
    "3000,3,0.425,28.0\n"
    "4000,4,0.418,28.3\n"
    "5000,5,0.422,28.1\n"
    "6000,6,0.419,28.2\n"
    "7000,7,0.421,28.1\n"
    "8000,8,0.417,28.3\n"
    "9000,9,0.423,28.0\n"
    "10000,10,0.420,28.2\n"
    "11000,11,0.416,28.1\n"
    "12000,12,0.422,28.3\n"
    "13000,13,0.418,28.0\n"
    "14000,14,0.421,28.2\n"
    "15000,15,0.420,28.1\n"
    "16000,16,0.419,28.3\n"
    "17000,17,0.423,28.0\n"
    "18000,18,0.415,28.2\n"
    "19000,19,0.421,28.1\n"
    "20000,20,0.420,28.2\n";

/** A trusted TrustResult helper. */
static TrustResult make_trusted_result(void)
{
    TrustResult tr;
    memset(&tr, 0, sizeof(tr));
    tr.is_trusted  = true;
    tr.trust_score = 100;
    tr.flags       = INTEGRITY_OK;
    strncpy(tr.explanation, "OK", ALGAOPS_EXPLANATION_LEN - 1);
    return tr;
}

/** An untrusted TrustResult helper. */
static TrustResult make_untrusted_result(IntegrityFlags flags,
                                          const char *reason)
{
    TrustResult tr;
    memset(&tr, 0, sizeof(tr));
    tr.is_trusted  = false;
    tr.trust_score = 0;
    tr.flags       = flags;
    strncpy(tr.explanation, reason, ALGAOPS_EXPLANATION_LEN - 1);
    return tr;
}

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
 * EH-01: Normal samples near baseline
 * ========================================================================= */
static int test_normal_near_baseline(void)
{
    char *path = write_temp_baseline_csv(BASELINE_CSV_NORMAL);
    ASSERT(path != NULL, "temp file creation failed");

    EquipmentHealthConfig cfg = equipment_health_default_config();
    EquipmentHealthState  state;
    ASSERT(equipment_health_init(&state, &cfg) == ALGAOPS_OK, "init failed");
    ASSERT(equipment_health_load_baseline(&state, path, NULL) == ALGAOPS_OK,
           "baseline load failed");

    /* A record near the mean should be NORMAL. */
    AlgaRecord r   = make_record(21000, 21, 0.420, 28.2);
    TrustResult tr = make_trusted_result();
    HealthResult hr;
    ASSERT(equipment_health_evaluate(&state, &r, &tr, &hr) == ALGAOPS_OK,
           "evaluate failed");
    ASSERT(hr.outcome == DIAG_NORMAL, "expected NORMAL for near-baseline data");
    ASSERT(hr.vib_z_score  < cfg.z_score_threshold, "vib z-score should be small");
    ASSERT(hr.temp_z_score < cfg.z_score_threshold, "temp z-score should be small");

    remove(path);
    free(path);
    return 0;
}

/* =========================================================================
 * EH-02: Large vibration deviation with trusted data
 * ========================================================================= */
static int test_vibration_anomaly(void)
{
    char *path = write_temp_baseline_csv(BASELINE_CSV_NORMAL);
    ASSERT(path != NULL, "temp file creation failed");

    EquipmentHealthConfig cfg = equipment_health_default_config();
    EquipmentHealthState  state;
    ASSERT(equipment_health_init(&state, &cfg) == ALGAOPS_OK, "init failed");
    ASSERT(equipment_health_load_baseline(&state, path, NULL) == ALGAOPS_OK,
           "baseline load failed");

    /* Vibration 5× higher than baseline — should be CHECK_EQUIPMENT. */
    AlgaRecord r   = make_record(21000, 21, 2.1, 28.2);
    TrustResult tr = make_trusted_result();
    HealthResult hr;
    ASSERT(equipment_health_evaluate(&state, &r, &tr, &hr) == ALGAOPS_OK,
           "evaluate failed");
    ASSERT(hr.outcome == DIAG_CHECK_EQUIPMENT,
           "expected CHECK_EQUIPMENT for large vib deviation");
    ASSERT(hr.vib_z_score > cfg.z_score_threshold,
           "vib z-score should exceed threshold");

    remove(path);
    free(path);
    return 0;
}

/* =========================================================================
 * EH-03: Significant temperature deviation with trusted data
 * ========================================================================= */
static int test_temperature_anomaly(void)
{
    char *path = write_temp_baseline_csv(BASELINE_CSV_NORMAL);
    ASSERT(path != NULL, "temp file creation failed");

    EquipmentHealthConfig cfg = equipment_health_default_config();
    EquipmentHealthState  state;
    ASSERT(equipment_health_init(&state, &cfg) == ALGAOPS_OK, "init failed");
    ASSERT(equipment_health_load_baseline(&state, path, NULL) == ALGAOPS_OK,
           "baseline load failed");

    /* Temperature 20 °C above baseline — should be CHECK_EQUIPMENT. */
    AlgaRecord r   = make_record(21000, 21, 0.420, 50.0);
    TrustResult tr = make_trusted_result();
    HealthResult hr;
    ASSERT(equipment_health_evaluate(&state, &r, &tr, &hr) == ALGAOPS_OK,
           "evaluate failed");
    ASSERT(hr.outcome == DIAG_CHECK_EQUIPMENT,
           "expected CHECK_EQUIPMENT for large temp deviation");
    ASSERT(hr.temp_z_score > cfg.z_score_threshold,
           "temp z-score should exceed threshold");

    remove(path);
    free(path);
    return 0;
}

/* =========================================================================
 * EH-04: Invalid / insufficient baseline (too few samples)
 * ========================================================================= */
static int test_insufficient_baseline(void)
{
    /* Only 3 records — fewer than the default minimum of 10. */
    const char *tiny_csv =
        "timestamp_ms,sequence_id,vibration_rms_g,temperature_c\n"
        "1000,1,0.42,28.0\n"
        "2000,2,0.43,28.1\n"
        "3000,3,0.41,28.2\n";

    char *path = write_temp_baseline_csv(tiny_csv);
    ASSERT(path != NULL, "temp file creation failed");

    EquipmentHealthConfig cfg = equipment_health_default_config();
    EquipmentHealthState  state;
    ASSERT(equipment_health_init(&state, &cfg) == ALGAOPS_OK, "init failed");
    AlgaopsStatus s = equipment_health_load_baseline(&state, path, NULL);
    ASSERT(s == ALGAOPS_ERR_BASELINE, "expected ALGAOPS_ERR_BASELINE");
    ASSERT(!state.baseline.is_valid, "baseline should not be valid");

    remove(path);
    free(path);
    return 0;
}

/* =========================================================================
 * EH-05: Baseline with near-zero variance (min_std_dev floor)
 * ========================================================================= */
static int test_near_zero_variance_baseline(void)
{
    /* All vibration values identical → std_dev = 0. */
    const char *flat_csv =
        "timestamp_ms,sequence_id,vibration_rms_g,temperature_c\n"
        "1000,1,0.42000,28.1\n"
        "2000,2,0.42000,28.3\n"
        "3000,3,0.42000,28.0\n"
        "4000,4,0.42000,28.2\n"
        "5000,5,0.42000,28.1\n"
        "6000,6,0.42000,28.3\n"
        "7000,7,0.42000,28.0\n"
        "8000,8,0.42000,28.2\n"
        "9000,9,0.42000,28.1\n"
        "10000,10,0.42000,28.2\n";

    char *path = write_temp_baseline_csv(flat_csv);
    ASSERT(path != NULL, "temp file creation failed");

    EquipmentHealthConfig cfg = equipment_health_default_config();
    EquipmentHealthState  state;
    ASSERT(equipment_health_init(&state, &cfg) == ALGAOPS_OK, "init failed");
    /* Load should succeed; a warning about near-zero std-dev should print. */
    ASSERT(equipment_health_load_baseline(&state, path, NULL) == ALGAOPS_OK,
           "baseline load failed for flat data");
    ASSERT(state.baseline.is_valid, "baseline should be valid");

    /* Now evaluate a record with the same vibration: z-score should be 0. */
    AlgaRecord r   = make_record(11000, 11, 0.42000, 28.2);
    TrustResult tr = make_trusted_result();
    HealthResult hr;
    ASSERT(equipment_health_evaluate(&state, &r, &tr, &hr) == ALGAOPS_OK,
           "evaluate failed");
    /* With min_std_dev floor, z=0 → NORMAL */
    ASSERT(hr.outcome == DIAG_NORMAL, "expected NORMAL with zero deviation");
    /* z-score must be finite (not NaN or Inf). */
    ASSERT(isfinite(hr.vib_z_score),  "vib z-score must be finite");
    ASSERT(isfinite(hr.temp_z_score), "temp z-score must be finite");

    remove(path);
    free(path);
    return 0;
}

/* =========================================================================
 * EH-06: Deterministic and numerically safe results
 * ========================================================================= */
static int test_deterministic_results(void)
{
    char *path = write_temp_baseline_csv(BASELINE_CSV_NORMAL);
    ASSERT(path != NULL, "temp file creation failed");

    EquipmentHealthConfig cfg = equipment_health_default_config();
    EquipmentHealthState  state;
    ASSERT(equipment_health_init(&state, &cfg) == ALGAOPS_OK, "init failed");
    ASSERT(equipment_health_load_baseline(&state, path, NULL) == ALGAOPS_OK,
           "baseline load failed");

    AlgaRecord r   = make_record(21000, 21, 2.1, 28.2);
    TrustResult tr = make_trusted_result();
    HealthResult hr1, hr2;

    ASSERT(equipment_health_evaluate(&state, &r, &tr, &hr1) == ALGAOPS_OK,
           "first evaluate failed");
    ASSERT(equipment_health_evaluate(&state, &r, &tr, &hr2) == ALGAOPS_OK,
           "second evaluate failed");

    /* Results must be identical across repeated calls. */
    ASSERT(hr1.outcome    == hr2.outcome,     "outcomes must be deterministic");
    ASSERT(fabs(hr1.vib_z_score  - hr2.vib_z_score)  < 1e-12,
           "vib z-score must be deterministic");
    ASSERT(fabs(hr1.temp_z_score - hr2.temp_z_score) < 1e-12,
           "temp z-score must be deterministic");
    ASSERT(isfinite(hr1.vib_z_score),  "vib z-score must be finite");
    ASSERT(isfinite(hr1.temp_z_score), "temp z-score must be finite");

    remove(path);
    free(path);
    return 0;
}

/* =========================================================================
 * EH-07: Untrusted data produces CHECK_SENSOR (Trust-Gating)
 * ========================================================================= */
static int test_trust_gating_suppresses_evaluation(void)
{
    char *path = write_temp_baseline_csv(BASELINE_CSV_NORMAL);
    ASSERT(path != NULL, "temp file creation failed");

    EquipmentHealthConfig cfg = equipment_health_default_config();
    EquipmentHealthState  state;
    ASSERT(equipment_health_init(&state, &cfg) == ALGAOPS_OK, "init failed");
    ASSERT(equipment_health_load_baseline(&state, path, NULL) == ALGAOPS_OK,
           "baseline load failed");

    /* Anomalous-looking data but the record is UNTRUSTED. */
    AlgaRecord r   = make_record(21000, 21, 5.0, 80.0);
    TrustResult tr = make_untrusted_result(INTEGRITY_VIB_STUCK,
                                            "vibration stuck");
    HealthResult hr;
    ASSERT(equipment_health_evaluate(&state, &r, &tr, &hr) == ALGAOPS_OK,
           "evaluate failed");
    ASSERT(hr.outcome == DIAG_CHECK_SENSOR,
           "untrusted data must produce CHECK_SENSOR, never CHECK_EQUIPMENT");
    ASSERT(hr.vib_z_score  == 0.0, "z-scores must be zero for blocked record");
    ASSERT(hr.temp_z_score == 0.0, "z-scores must be zero for blocked record");

    remove(path);
    free(path);
    return 0;
}

/* =========================================================================
 * EH-08: NULL pointer robustness
 * ========================================================================= */
static int test_null_pointer_robustness(void)
{
    EquipmentHealthConfig cfg = equipment_health_default_config();
    ASSERT(equipment_health_init(NULL, &cfg) == ALGAOPS_ERR_NULL_PTR,
           "null state should fail");
    ASSERT(equipment_health_init((EquipmentHealthState*)&cfg, NULL) == ALGAOPS_ERR_NULL_PTR,
           "null config should fail");

    EquipmentHealthState state;
    equipment_health_init(&state, &cfg);
    ASSERT(equipment_health_load_baseline(&state, NULL, NULL) == ALGAOPS_ERR_NULL_PTR,
           "null path should fail");
    ASSERT(equipment_health_load_baseline(NULL, "x.csv", NULL) == ALGAOPS_ERR_NULL_PTR,
           "null state should fail for load");

    return 0;
}

/* =========================================================================
 * Entry point
 * ========================================================================= */
int main(void)
{
    printf("=== Equipment Health Unit Tests ===\n");

    RUN_TEST(test_normal_near_baseline);
    RUN_TEST(test_vibration_anomaly);
    RUN_TEST(test_temperature_anomaly);
    RUN_TEST(test_insufficient_baseline);
    RUN_TEST(test_near_zero_variance_baseline);
    RUN_TEST(test_deterministic_results);
    RUN_TEST(test_trust_gating_suppresses_evaluation);
    RUN_TEST(test_null_pointer_robustness);

    printf("\nResults: %d/%d passed", g_tests_passed, g_tests_run);
    if (g_tests_failed > 0) printf(" (%d FAILED)", g_tests_failed);
    printf("\n");

    return (g_tests_failed == 0) ? 0 : 1;
}
