/*
 * test_integration.c — Integration tests for TrustGuard + Equipment Health.
 *
 * These tests drive both modules together and verify the complete
 * Trust-Gated Diagnostics pipeline end-to-end.
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
#define TMPFILE_FUNC _tempnam
#else
#define TMPFILE_FUNC tempnam
#endif

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
        printf("  %-60s ", #fn);                                  \
        int rc = fn();                                            \
        if (rc == 0) { printf("PASS\n"); g_tests_passed++; }     \
        else         { printf("FAIL\n"); g_tests_failed++; }      \
    } while (0)

/* =========================================================================
 * Common baseline CSV (20 normal records)
 * ========================================================================= */
static const char BASELINE_CSV[] =
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

static char *write_temp_csv(const char *content)
{
    char *path = TMPFILE_FUNC(NULL, "algaops_inttest_");
    if (!path) return NULL;
    FILE *f = fopen(path, "w");
    if (!f) { free(path); return NULL; }
    fputs(content, f);
    fclose(f);
    return path;
}

/** Helper: initialise TG + EH from a baseline file path. */
static int setup_pipeline(TrustGuardState        *tg_state,
                           EquipmentHealthState   *eh_state,
                           const char             *baseline_path)
{
    TrustGuardConfig   tg_cfg = trustguard_default_config();
    EquipmentHealthConfig eh_cfg = equipment_health_default_config();

    if (trustguard_init(tg_state, &tg_cfg)       != ALGAOPS_OK) return -1;
    if (equipment_health_init(eh_state, &eh_cfg)  != ALGAOPS_OK) return -1;
    if (equipment_health_load_baseline(eh_state, baseline_path, &tg_cfg)
        != ALGAOPS_OK) return -1;
    return 0;
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
 * INT-01: Trusted normal data → DIAG_NORMAL
 * ========================================================================= */
static int test_trusted_normal_produces_normal(void)
{
    char *path = write_temp_csv(BASELINE_CSV);
    ASSERT(path != NULL, "temp file failed");

    TrustGuardState      tg; EquipmentHealthState eh;
    ASSERT(setup_pipeline(&tg, &eh, path) == 0, "pipeline setup failed");

    AlgaRecord r = make_record(21000, 21, 0.420, 28.2);
    TrustResult   tr; HealthResult hr;

    ASSERT(trustguard_validate(&tg, &r, &tr) == ALGAOPS_OK,   "tg failed");
    ASSERT(tr.is_trusted, "record should be trusted");
    ASSERT(equipment_health_evaluate(&eh, &r, &tr, &hr) == ALGAOPS_OK, "eh failed");
    ASSERT(hr.outcome == DIAG_NORMAL, "expected DIAG_NORMAL");

    remove(path); free(path);
    return 0;
}

/* =========================================================================
 * INT-02: Trusted anomalous data → DIAG_CHECK_EQUIPMENT
 * ========================================================================= */
static int test_trusted_anomaly_produces_check_equipment(void)
{
    char *path = write_temp_csv(BASELINE_CSV);
    ASSERT(path != NULL, "temp file failed");

    TrustGuardState      tg; EquipmentHealthState eh;
    ASSERT(setup_pipeline(&tg, &eh, path) == 0, "pipeline setup failed");

    /* Very high vibration — clearly anomalous. */
    AlgaRecord r = make_record(21000, 21, 3.5, 28.2);
    TrustResult   tr; HealthResult hr;

    ASSERT(trustguard_validate(&tg, &r, &tr) == ALGAOPS_OK, "tg failed");
    ASSERT(tr.is_trusted, "high-vib record with valid sequence should be trusted");
    ASSERT(equipment_health_evaluate(&eh, &r, &tr, &hr) == ALGAOPS_OK, "eh failed");
    ASSERT(hr.outcome == DIAG_CHECK_EQUIPMENT,
           "expected DIAG_CHECK_EQUIPMENT for anomalous trusted data");

    remove(path); free(path);
    return 0;
}

/* =========================================================================
 * INT-03: Untrusted data → DIAG_CHECK_SENSOR
 * ========================================================================= */
static int test_untrusted_data_produces_check_sensor(void)
{
    char *path = write_temp_csv(BASELINE_CSV);
    ASSERT(path != NULL, "temp file failed");

    TrustGuardState      tg; EquipmentHealthState eh;
    ASSERT(setup_pipeline(&tg, &eh, path) == 0, "pipeline setup failed");

    /* Duplicate sequence ID → untrusted. */
    AlgaRecord r1 = make_record(21000, 21, 0.42, 28.2);
    AlgaRecord r2 = make_record(22000, 21, 0.42, 28.2); /* dup seq */
    TrustResult   tr; HealthResult hr;

    ASSERT(trustguard_validate(&tg, &r1, &tr) == ALGAOPS_OK, "tg r1 failed");
    ASSERT(tr.is_trusted, "first record should be trusted");

    ASSERT(trustguard_validate(&tg, &r2, &tr) == ALGAOPS_OK, "tg r2 failed");
    ASSERT(!tr.is_trusted, "duplicate seq should be untrusted");

    ASSERT(equipment_health_evaluate(&eh, &r2, &tr, &hr) == ALGAOPS_OK, "eh failed");
    ASSERT(hr.outcome == DIAG_CHECK_SENSOR,
           "untrusted data must produce CHECK_SENSOR");

    remove(path); free(path);
    return 0;
}

/* =========================================================================
 * INT-04: Suspicious measurements + blocking integrity failure → CHECK_SENSOR,
 *         never CHECK_EQUIPMENT.
 * ========================================================================= */
static int test_suspicious_plus_blocking_yields_check_sensor(void)
{
    char *path = write_temp_csv(BASELINE_CSV);
    ASSERT(path != NULL, "temp file failed");

    TrustGuardState      tg; EquipmentHealthState eh;
    ASSERT(setup_pipeline(&tg, &eh, path) == 0, "pipeline setup failed");

    /* Very high vibration AND non-monotonic timestamp → blocking flag. */
    AlgaRecord r_first = make_record(10000, 1, 0.42, 28.0);
    AlgaRecord r_bad   = make_record(5000,  2, 5.0,  80.0); /* ts < prev */
    TrustResult   tr; HealthResult hr;

    ASSERT(trustguard_validate(&tg, &r_first, &tr) == ALGAOPS_OK, "tg r1 failed");
    ASSERT(trustguard_validate(&tg, &r_bad,   &tr) == ALGAOPS_OK, "tg r2 failed");

    ASSERT(!tr.is_trusted, "record with bad timestamp must be untrusted");
    ASSERT(tr.flags & INTEGRITY_BAD_TIMESTAMP, "bad timestamp flag expected");

    ASSERT(equipment_health_evaluate(&eh, &r_bad, &tr, &hr) == ALGAOPS_OK,
           "eh failed");
    ASSERT(hr.outcome != DIAG_CHECK_EQUIPMENT,
           "blocking integrity failure MUST suppress CHECK_EQUIPMENT");
    ASSERT(hr.outcome == DIAG_CHECK_SENSOR,
           "expected CHECK_SENSOR, not CHECK_EQUIPMENT");

    remove(path); free(path);
    return 0;
}

/* =========================================================================
 * INT-05: Malformed input CSV is handled safely
 * ========================================================================= */
static int test_malformed_input_safe(void)
{
    char *bpath = write_temp_csv(BASELINE_CSV);
    ASSERT(bpath != NULL, "baseline temp file failed");

    const char *malformed_csv =
        "timestamp_ms,sequence_id,vibration_rms_g,temperature_c\n"
        "1000,1,0.42,28.0\n"
        "NOTANUMBER,2,0.43,28.1\n"   /* malformed timestamp */
        "3000,3,abc,28.2\n"          /* malformed vibration */
        "4000,4,0.41\n"              /* missing temperature */
        "5000,5,0.42,28.3\n";        /* good */

    char *ipath = write_temp_csv(malformed_csv);
    ASSERT(ipath != NULL, "input temp file failed");

    TrustGuardState      tg; EquipmentHealthState eh;
    ASSERT(setup_pipeline(&tg, &eh, bpath) == 0, "pipeline setup failed");
    trustguard_reset(&tg);

    /* Simulate reading through the CSV manually. */
    #include "csv_reader.h"
    CsvReader reader;
    ASSERT(csv_reader_open(&reader, ipath) == ALGAOPS_OK, "open failed");

    int good_count = 0, parse_err_count = 0;
    AlgaRecord rec;
    while (true) {
        AlgaopsStatus s = csv_reader_next(&reader, &rec);
        if (s == ALGAOPS_ERR_IO) break;
        if (s == ALGAOPS_ERR_PARSE) { parse_err_count++; continue; }
        good_count++;
    }
    csv_reader_close(&reader);

    /* Should have parsed 2 good records (seq 1 and 5) and 3 parse errors. */
    ASSERT(good_count    == 2, "expected 2 good records");
    ASSERT(parse_err_count == 3, "expected 3 parse errors");

    remove(bpath); free(bpath);
    remove(ipath); free(ipath);
    return 0;
}

/* =========================================================================
 * INT-06: Missing-packet flag does not block equipment evaluation
 *         (missing packet is informational, not a blocking flag)
 * ========================================================================= */
static int test_missing_packet_not_blocking(void)
{
    char *path = write_temp_csv(BASELINE_CSV);
    ASSERT(path != NULL, "temp file failed");

    TrustGuardState      tg; EquipmentHealthState eh;
    ASSERT(setup_pipeline(&tg, &eh, path) == 0, "pipeline setup failed");

    AlgaRecord r1 = make_record(21000, 21, 0.42, 28.2);
    AlgaRecord r3 = make_record(23000, 23, 0.42, 28.2); /* seq 22 missing */
    TrustResult   tr; HealthResult hr;

    ASSERT(trustguard_validate(&tg, &r1, &tr) == ALGAOPS_OK, "tg r1 failed");
    ASSERT(trustguard_validate(&tg, &r3, &tr) == ALGAOPS_OK, "tg r3 failed");

    ASSERT(tr.flags & INTEGRITY_MISSING_PACKET, "missing packet flag expected");
    /* Missing packet is NOT a blocking flag — record may still be trusted. */
    ASSERT(tr.is_trusted, "missing-packet-only record should be trusted");

    ASSERT(equipment_health_evaluate(&eh, &r3, &tr, &hr) == ALGAOPS_OK,
           "eh failed");
    /* Since data is trusted, outcome should be NORMAL (data is near baseline). */
    ASSERT(hr.outcome == DIAG_NORMAL,
           "trusted record with only MISSING_PACKET should be NORMAL (near baseline)");

    remove(path); free(path);
    return 0;
}

/* =========================================================================
 * Entry point
 * ========================================================================= */
int main(void)
{
    printf("=== Integration Tests ===\n");

    RUN_TEST(test_trusted_normal_produces_normal);
    RUN_TEST(test_trusted_anomaly_produces_check_equipment);
    RUN_TEST(test_untrusted_data_produces_check_sensor);
    RUN_TEST(test_suspicious_plus_blocking_yields_check_sensor);
    RUN_TEST(test_malformed_input_safe);
    RUN_TEST(test_missing_packet_not_blocking);

    printf("\nResults: %d/%d passed", g_tests_passed, g_tests_run);
    if (g_tests_failed > 0) printf(" (%d FAILED)", g_tests_failed);
    printf("\n");

    return (g_tests_failed == 0) ? 0 : 1;
}
