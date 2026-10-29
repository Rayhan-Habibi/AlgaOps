/*
 * JSON payloads of the node.  With "--emit <dir>" the test also writes sample
 * payloads that gateway/tests/unit/test_firmware_payload_contract.py checks
 * against the JSON schemas and the gateway's strict parser.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "telemetry_payload.h"
#include "test_harness.h"

#define NODE "esp32-a1b2c3d4e5f6"
#define FW   "2.0.0"

static char g_buf[TELEMETRY_PAYLOAD_MAX];

static telemetry_record_t sample_record(void)
{
    telemetry_record_t r;
    memset(&r, 0, sizeof(r));
    r.boot_id = 3735928559u;
    r.seq = 42;
    r.t_end_mono_us = 61280000;                 /* 61.28 s after boot */
    r.window_ms = 1280;
    r.sample_rate_hz = 800.0f;
    r.samples_expected = 1024;
    r.samples_valid = 1024;
    r.vib_valid = true;
    r.vib_rms_g = 0.42f;
    for (int a = 0; a < 3; a++) {
        r.vib_rms_axis_g[a] = 0.2425f;
        r.vib_p2p_g[a] = 1.21f;
    }
    r.temp_valid = true;
    r.temp_c = 31.0625f;
    r.self_test = NODE_SELF_TEST_PASS;
    r.health.queue_depth = 3;
    r.health.uptime_s = 61;
    r.health.rssi_valid = true;
    r.health.rssi_dbm = -58;
    return r;
}

static const time_base_t SYNCED = { true, 1793000000000000LL - 1000000LL };

static int contains(const char *s, const char *part)
{
    return strstr(s, part) != NULL;
}

static int test_node_id_contract(void)
{
    ASSERT(node_id_valid(NODE), "MAC-based id");
    ASSERT(node_id_valid("pump-01.line_2"), "letters, digits, _ . -");
    ASSERT(!node_id_valid("") && !node_id_valid(NULL), "empty");
    ASSERT(!node_id_valid("-pump"), "must start with a letter or digit");
    ASSERT(!node_id_valid("pump 1") && !node_id_valid("pump/1") && !node_id_valid("p\"1"),
           "no spaces, slashes or quotes");
    char id[66];
    memset(id, 'a', 64);
    id[64] = '\0';
    ASSERT(node_id_valid(id), "64 characters");
    id[64] = 'a';
    id[65] = '\0';
    ASSERT(!node_id_valid(id), "65 characters");
    return 0;
}

static int test_time_base(void)
{
    time_base_t none = { false, 0 };
    ASSERT(time_base_to_ms(&none, 61280000) == 61280, "unsynced: ms since boot");
    ASSERT(time_base_to_ms(&SYNCED, 61280000) == 1793000060280LL, "synced: Unix ms");
    ASSERT(time_base_to_ms(NULL, -5) == 0, "never negative");
    return 0;
}

static int test_full_window(void)
{
    telemetry_record_t r = sample_record();
    r.faults = NODE_FAULT_TEMP_POR | NODE_FAULT_ACCEL_OVERRUN;
    int n = telemetry_payload_build(g_buf, sizeof(g_buf), NODE, FW, &r, &SYNCED);
    ASSERT(n > 0 && (size_t)n == strlen(g_buf), "length returned");
    ASSERT(g_buf[0] == '{' && g_buf[n - 1] == '}', "one JSON object");
    ASSERT(contains(g_buf, "\"schema\":\"algaops.telemetry_window.v1\""), "schema id");
    ASSERT(contains(g_buf, "\"node_id\":\"" NODE "\""), "node id");
    ASSERT(contains(g_buf, "\"boot_id\":3735928559,\"seq\":42"), "identity, unsigned");
    ASSERT(contains(g_buf, "\"t_start_ms\":1793000059000,\"t_end_ms\":1793000060280"), "times");
    ASSERT(contains(g_buf, "\"window_ms\":1280,\"time_synced\":true"), "synced flag");
    ASSERT(contains(g_buf, "\"samples_expected\":1024,\"samples_valid\":1024"), "sample counts");
    ASSERT(contains(g_buf, "\"vib\":{\"rms_g\":0.42000,\"rms_axis_g\":[0.24250,0.24250,0.24250]"),
           "vibration");
    ASSERT(contains(g_buf, "\"temp_c\":31.0625"), "temperature");
    ASSERT(!contains(g_buf, "current_a"), "no CT configured: no current field");
    ASSERT(contains(g_buf, "\"faults\":[\"ACCEL_OVERRUN\",\"TEMP_POR\"]"), "faults");
    ASSERT(contains(g_buf, "\"source\":\"hardware\""), "source");
    ASSERT(contains(g_buf, "\"rssi_dbm\":-58"), "rssi");
    ASSERT(!contains(g_buf, "nan") && !contains(g_buf, "inf"), "no non-JSON numbers");
    return 0;
}

static int test_invalid_measurements_are_null(void)
{
    telemetry_record_t r = sample_record();
    r.vib_valid = false;
    r.temp_valid = false;
    r.current_present = true;
    r.current_valid = false;
    r.health.rssi_valid = false;
    time_base_t none = { false, 0 };
    int n = telemetry_payload_build(g_buf, sizeof(g_buf), NODE, FW, &r, &none);
    ASSERT(n > 0, "built");
    ASSERT(contains(g_buf, "\"vib\":{\"rms_g\":null}"), "no vibration value");
    ASSERT(contains(g_buf, "\"temp_c\":null"), "no temperature value");
    ASSERT(contains(g_buf, "\"current_a\":null"), "configured CT without value");
    ASSERT(contains(g_buf, "\"time_synced\":false") && contains(g_buf, "\"t_end_ms\":61280"),
           "unsynced clock flagged");
    ASSERT(!contains(g_buf, "rssi_dbm"), "unknown RSSI omitted");

    r = sample_record();
    r.vib_rms_g = NAN;
    r.vib_p2p_g[1] = INFINITY;
    n = telemetry_payload_build(g_buf, sizeof(g_buf), NODE, FW, &r, &SYNCED);
    ASSERT(n > 0 && contains(g_buf, "\"rms_g\":null") && contains(g_buf, "1.21000,null,1.21000"),
           "non-finite numbers become null");
    return 0;
}

static int test_rejects_bad_input(void)
{
    telemetry_record_t r = sample_record();
    ASSERT(telemetry_payload_build(g_buf, 64, NODE, FW, &r, &SYNCED) == -1, "buffer too small");
    ASSERT(telemetry_payload_build(g_buf, sizeof(g_buf), "bad id", FW, &r, &SYNCED) == -1,
           "invalid node id");
    ASSERT(telemetry_payload_build(g_buf, sizeof(g_buf), NODE, "2.0\"x", &r, &SYNCED) == -1,
           "fw string needing escapes");
    ASSERT(telemetry_payload_build(g_buf, sizeof(g_buf), NODE, "0123456789012345678901234567890123",
                                   &r, &SYNCED) == -1, "fw longer than 32");
    r.window_ms = 0;
    ASSERT(telemetry_payload_build(g_buf, sizeof(g_buf), NODE, FW, &r, &SYNCED) == -1,
           "zero window");
    return 0;
}

static node_status_t online_status(void)
{
    node_status_t s;
    memset(&s, 0, sizeof(s));
    s.state = "online";
    s.has_boot = true;
    s.boot_id = 3735928559u;
    s.fw = FW;
    s.has_runtime = true;
    s.uptime_s = 3600;
    s.rssi_valid = true;
    s.rssi_dbm = -61;
    s.queue_depth = 2;
    s.dropped_windows = 5;
    s.self_test = NODE_SELF_TEST_PASS;
    s.accel_ok = true;
    s.temp_ok = true;
    s.current_present = true;
    s.current_ok = false;
    s.reset_reason = "brownout";
    s.ts_ms = 1793000003600000LL / 1000;
    return s;
}

static int test_status_messages(void)
{
    node_status_t s = online_status();
    int n = status_payload_build(g_buf, sizeof(g_buf), NODE, &s);
    ASSERT(n > 0, "built");
    ASSERT(contains(g_buf, "\"schema\":\"algaops.node_status.v1\""), "schema");
    ASSERT(contains(g_buf, "\"state\":\"online\""), "state");
    ASSERT(contains(g_buf, "\"sensors\":{\"accelerometer\":true,\"temperature\":true,\"current\":false}"),
           "sensor flags");
    ASSERT(contains(g_buf, "\"reset_reason\":\"brownout\""), "reset reason");

    node_status_t lwt;
    memset(&lwt, 0, sizeof(lwt));
    lwt.state = "offline";
    lwt.has_boot = true;
    lwt.boot_id = 7;
    lwt.ts_ms = -1;
    n = status_payload_build(g_buf, sizeof(g_buf), NODE, &lwt);
    ASSERT(n > 0 && strcmp(g_buf, "{\"schema\":\"algaops.node_status.v1\",\"node_id\":\"" NODE
                           "\",\"state\":\"offline\",\"source\":\"hardware\",\"boot_id\":7}") == 0,
           "minimal last will");
    lwt.state = "sleeping";
    ASSERT(status_payload_build(g_buf, sizeof(g_buf), NODE, &lwt) == -1, "unknown state");
    return 0;
}

/* ---- contract samples for the gateway test ------------------------------------- */

static int write_file(const char *dir, const char *name, const char *text)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) return 1;
    fputs(text, f);
    fclose(f);
    return 0;
}

static int emit(const char *dir)
{
    int rc = 0;
    telemetry_record_t r = sample_record();
    time_base_t none = { false, 0 };
    rc |= telemetry_payload_build(g_buf, sizeof(g_buf), NODE, FW, &r, &SYNCED) < 0;
    rc |= write_file(dir, "telemetry_full.json", g_buf);

    r.faults = 0xFF;
    r.self_test = NODE_SELF_TEST_FAIL;
    r.current_present = true;
    r.current_valid = true;
    r.current_a = 1.804f;
    r.samples_valid = 1023;
    rc |= telemetry_payload_build(g_buf, sizeof(g_buf), NODE, FW, &r, &SYNCED) < 0;
    rc |= write_file(dir, "telemetry_all_faults.json", g_buf);

    r = sample_record();
    r.vib_valid = false;
    r.temp_valid = false;
    r.current_present = true;
    r.health.rssi_valid = false;
    rc |= telemetry_payload_build(g_buf, sizeof(g_buf), NODE, FW, &r, &none) < 0;
    rc |= write_file(dir, "telemetry_nulls_unsynced.json", g_buf);

    node_status_t s = online_status();
    rc |= status_payload_build(g_buf, sizeof(g_buf), NODE, &s) < 0;
    rc |= write_file(dir, "status_online.json", g_buf);
    node_status_t lwt;
    memset(&lwt, 0, sizeof(lwt));
    lwt.state = "offline";
    lwt.has_boot = true;
    lwt.boot_id = 7;
    lwt.fw = FW;
    lwt.ts_ms = -1;
    rc |= status_payload_build(g_buf, sizeof(g_buf), NODE, &lwt) < 0;
    rc |= write_file(dir, "status_lwt.json", g_buf);
    if (rc) fprintf(stderr, "could not write the samples to %s\n", dir);
    return rc;
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--emit") == 0) return emit(argv[2]);
    printf("telemetry_payload\n");
    RUN_TEST(test_node_id_contract);
    RUN_TEST(test_time_base);
    RUN_TEST(test_full_window);
    RUN_TEST(test_invalid_measurements_are_null);
    RUN_TEST(test_rejects_bad_input);
    RUN_TEST(test_status_messages);
    TEST_SUMMARY();
    return TEST_EXIT_CODE();
}
