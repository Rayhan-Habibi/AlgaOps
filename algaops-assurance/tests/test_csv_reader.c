/*
 * test_csv_reader.c — v1 positional and v2 named-column CSV parsing.
 */

#include "test_harness.h"
#include "csv_reader.h"

#include <stdlib.h>

static const char *write_file(const char *name, const char *content)
{
    FILE *f = fopen(name, "wb");
    if (!f) return NULL;
    fputs(content, f);
    fclose(f);
    return name;
}

static int test_v1_positional_and_comments(void)
{
    const char *p = write_file("csv_v1.csv",
        "# comment\n"
        "\n"
        "timestamp_ms,sequence_id,vibration_rms_g,temperature_c\n"
        "1000,1,0.420,28.1\r\n"
        "2000,2,0.415,28.2   \n");
    ASSERT(p, "write");
    CsvReader r;
    ASSERT(csv_reader_open(&r, p) == ALGAOPS_OK, "open");
    AlgaRecord rec;
    ASSERT(csv_reader_next(&r, &rec) == ALGAOPS_OK, "row 1");
    ASSERT(rec.timestamp_ms == 1000 && rec.sequence_id == 1, "row 1 ids");
    ASSERT_NEAR(rec.vibration_rms_g, 0.420, 1e-12, "row 1 vib");
    ASSERT(csv_reader_next(&r, &rec) == ALGAOPS_OK, "row 2 with trailing spaces");
    ASSERT_NEAR(rec.temperature_c, 28.2, 1e-12, "row 2 temp");
    ASSERT(csv_reader_next(&r, &rec) == ALGAOPS_ERR_IO, "EOF");
    ASSERT(!r.named, "v1 header keeps positional parsing");
    csv_reader_close(&r);
    remove(p);
    return 0;
}

static int test_v1_rejects_negative_and_overflow(void)
{
    const char *p = write_file("csv_neg.csv",
        "-1000,1,0.42,28.0\n"
        "1000,-1,0.42,28.0\n"
        "1000,4294967296,0.42,28.0\n"
        "1000,4294967295,0.42,28.0\n");
    CsvReader r;
    ASSERT(csv_reader_open(&r, p) == ALGAOPS_OK, "open");
    AlgaRecord rec;
    ASSERT(csv_reader_next(&r, &rec) == ALGAOPS_ERR_PARSE, "negative timestamp rejected");
    ASSERT(csv_reader_next(&r, &rec) == ALGAOPS_ERR_PARSE, "negative sequence rejected");
    ASSERT(csv_reader_next(&r, &rec) == ALGAOPS_ERR_PARSE, "sequence overflow rejected");
    ASSERT(csv_reader_next(&r, &rec) == ALGAOPS_OK, "UINT32_MAX accepted");
    ASSERT(rec.sequence_id == 4294967295u, "UINT32_MAX value");
    csv_reader_close(&r);
    remove(p);
    return 0;
}

static int test_v1_non_finite_policy(void)
{
    const char *p = write_file("csv_nan.csv", "1000,1,nan,28.0\n2000,2,0.4,inf\n");
    CsvReader r;
    AlgaRecord rec;
    AlgaWindow w;
    ASSERT(csv_reader_open(&r, p) == ALGAOPS_OK, "open");
    ASSERT(csv_reader_next(&r, &rec) == ALGAOPS_ERR_PARSE, "legacy reader rejects NaN");
    ASSERT(csv_reader_next(&r, &rec) == ALGAOPS_ERR_PARSE, "legacy reader rejects Inf");
    csv_reader_close(&r);
    ASSERT(csv_reader_open(&r, p) == ALGAOPS_OK, "reopen");
    ASSERT(csv_reader_next_window(&r, &w) == ALGAOPS_ERR_PARSE,
           "positional window reader follows the v1 rule");
    csv_reader_close(&r);
    remove(p);
    return 0;
}

static int test_v2_named_columns(void)
{
    const char *p = write_file("csv_v2.csv",
        "sequence_id,timestamp_ms,boot_id,received_ms,t_start_ms,window_ms,"
        "samples_expected,samples_valid,vibration_rms_g,vib_rms_x_g,vib_rms_y_g,vib_rms_z_g,"
        "vib_p2p_x_g,vib_p2p_y_g,vib_p2p_z_g,temperature_c,current_a,self_test,time_synced,"
        "node_fault_flags,unknown_extra\n"
        "7,10000,42,10150,8720,1280,1024,1020,0.5,0.3,0.3,0.26458,1.1,1.0,0.9,30.5,1.8,pass,1,0,zzz\n"
        "8,11280,42,,,,,,0.5,,,,,,,30.6,,,,,\n"
        "9,12560,42,12700,11280,1280,1024,1024,nan,0.3,0.3,0.3,1,1,1,30.7,,fail,0,3,\n");
    CsvReader r;
    AlgaWindow w;
    ASSERT(csv_reader_open(&r, p) == ALGAOPS_OK, "open");
    ASSERT(csv_reader_next_window(&r, &w) == ALGAOPS_OK, "row 1");
    ASSERT(r.named, "extra columns enable named parsing");
    ASSERT(w.sequence_id == 7 && w.timestamp_ms == 10000 && w.boot_id == 42, "identity");
    uint32_t want = ALGA_HAS_RECEIVED | ALGA_HAS_TIMING | ALGA_HAS_SAMPLE_COUNTS |
                    ALGA_HAS_AXES | ALGA_HAS_P2P | ALGA_HAS_CURRENT | ALGA_HAS_SELF_TEST |
                    ALGA_HAS_TIME_SYNC | ALGA_HAS_NODE_FAULTS;
    ASSERT(w.presence == want, "all optional fields present");
    ASSERT(w.received_ms == 10150 && w.t_start_ms == 8720 && w.window_ms == 1280, "timing");
    ASSERT(w.samples_expected == 1024 && w.samples_valid == 1020, "counts");
    ASSERT_NEAR(w.vib_rms_axis_g[2], 0.26458, 1e-12, "axis z");
    ASSERT_NEAR(w.current_a, 1.8, 1e-12, "current");
    ASSERT(w.self_test == ALGA_SELF_TEST_PASS && w.time_synced == 1, "self test / sync");

    ASSERT(csv_reader_next_window(&r, &w) == ALGAOPS_OK, "row 2 sparse");
    ASSERT(w.presence == 0, "empty optional cells are absent");

    ASSERT(csv_reader_next_window(&r, &w) == ALGAOPS_OK, "row 3 NaN passes through");
    ASSERT(!isfinite(w.vibration_rms_g), "NaN kept for TrustGuard");
    ASSERT(w.self_test == ALGA_SELF_TEST_FAIL && w.node_fault_flags == 3, "fail + faults");
    ASSERT(csv_reader_next_window(&r, &w) == ALGAOPS_ERR_IO, "EOF");
    csv_reader_close(&r);

    /* The legacy reader can still consume a v2 file (v1 columns by name). */
    AlgaRecord rec;
    ASSERT(csv_reader_open(&r, p) == ALGAOPS_OK, "reopen");
    ASSERT(csv_reader_next(&r, &rec) == ALGAOPS_OK && rec.sequence_id == 7, "legacy on v2");
    csv_reader_close(&r);
    remove(p);
    return 0;
}

static int test_v2_errors(void)
{
    const char *p = write_file("csv_v2_bad.csv",
        "timestamp_ms,sequence_id,vibration_rms_g,temperature_c,current_a\n"
        "1000,1,0.4,28\n"                 /* column count mismatch */
        "1000,1,0.4,,1.0\n"               /* missing mandatory temperature */
        "1000,1,0.4,28,abc\n"             /* malformed optional */
        "1000,1,0.4,28,1.5\n");
    CsvReader r;
    AlgaWindow w;
    ASSERT(csv_reader_open(&r, p) == ALGAOPS_OK, "open");
    ASSERT(csv_reader_next_window(&r, &w) == ALGAOPS_ERR_PARSE, "column count");
    ASSERT(csv_reader_next_window(&r, &w) == ALGAOPS_ERR_PARSE, "missing mandatory");
    ASSERT(csv_reader_next_window(&r, &w) == ALGAOPS_ERR_PARSE, "malformed optional");
    ASSERT(csv_reader_next_window(&r, &w) == ALGAOPS_OK, "valid row after errors");
    ASSERT((w.presence & ALGA_HAS_CURRENT) && w.current_a == 1.5, "current parsed");
    csv_reader_close(&r);
    remove(p);
    return 0;
}

static int test_long_line_rejected(void)
{
    char *big = (char *)malloc(3000);
    ASSERT(big != NULL, "alloc");
    memset(big, '1', 2500);
    strcpy(big + 2500, ",1,0.4,28\n2000,2,0.4,28\n");
    const char *p = write_file("csv_long.csv", big);
    free(big);
    CsvReader r;
    AlgaRecord rec;
    ASSERT(csv_reader_open(&r, p) == ALGAOPS_OK, "open");
    ASSERT(csv_reader_next(&r, &rec) == ALGAOPS_ERR_PARSE, "long line rejected");
    ASSERT(csv_reader_next(&r, &rec) == ALGAOPS_OK && rec.sequence_id == 2, "reader recovers");
    csv_reader_close(&r);
    remove(p);
    return 0;
}

static int test_missing_file(void)
{
    CsvReader r;
    ASSERT(csv_reader_open(&r, "definitely_missing_file.csv") == ALGAOPS_ERR_IO, "missing file");
    ASSERT(csv_reader_open(NULL, "x") == ALGAOPS_ERR_NULL_PTR, "null reader");
    csv_reader_close(NULL);
    return 0;
}

int main(void)
{
    printf("=== CSV Reader Tests ===\n");
    RUN_TEST(test_v1_positional_and_comments);
    RUN_TEST(test_v1_rejects_negative_and_overflow);
    RUN_TEST(test_v1_non_finite_policy);
    RUN_TEST(test_v2_named_columns);
    RUN_TEST(test_v2_errors);
    RUN_TEST(test_long_line_rejected);
    RUN_TEST(test_missing_file);
    TEST_SUMMARY();
    return TEST_EXIT_CODE();
}
