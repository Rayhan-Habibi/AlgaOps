/*
 * test_engine.c — diagnostic engine: legacy equivalence, golden scenarios,
 * trust-gate invariants (randomized), v2 integrity checks, modes, alerts,
 * configuration keys and ABI information.
 */

#include "test_harness.h"
#include "../include/algaops_engine.h"
#include "../include/equipment_health.h"
#include "csv_reader.h"

#include <stdlib.h>

/* ------------------------------------------------------------------------ */
/* helpers                                                                   */
/* ------------------------------------------------------------------------ */

static AlgaDecision g_d;

static AlgaWindow make_window(uint32_t boot, uint32_t seq, uint64_t ts, double vib, double temp)
{
    AlgaWindow w;
    memset(&w, 0, sizeof(w));
    w.boot_id = boot;
    w.sequence_id = seq;
    w.timestamp_ms = ts;
    w.vibration_rms_g = vib;
    w.temperature_c = temp;
    return w;
}

/* Fully populated v2 window as produced by the firmware. */
static AlgaWindow make_v2(uint32_t boot, uint32_t seq, uint64_t ts, double vib, double temp,
                          double cur)
{
    AlgaWindow w = make_window(boot, seq, ts, vib, temp);
    w.presence = ALGA_HAS_RECEIVED | ALGA_HAS_TIMING | ALGA_HAS_SAMPLE_COUNTS | ALGA_HAS_AXES |
                 ALGA_HAS_P2P | ALGA_HAS_SELF_TEST | ALGA_HAS_TIME_SYNC | ALGA_HAS_NODE_FAULTS;
    w.received_ms = ts + 150;
    w.t_start_ms = ts - 1280;
    w.window_ms = 1280;
    w.samples_expected = 1024;
    w.samples_valid = 1024;
    double a = vib / sqrt(3.0);
    for (int i = 0; i < 3; i++) {
        w.vib_rms_axis_g[i] = a;
        w.vib_p2p_axis_g[i] = a * 3.2;
    }
    w.self_test = ALGA_SELF_TEST_PASS;
    w.time_synced = 1;
    if (cur >= 0.0) {
        w.presence |= ALGA_HAS_CURRENT;
        w.current_a = cur;
    }
    return w;
}

static void policy_like_config(AlgaEngineConfig *c)
{
    algaops_engine_config_default(c);
    c->trust.stuck_window_size = 10;
    c->trust.stuck_vib_threshold_g = 0.0;      /* rely on zero-p2p rule */
    c->trust.stuck_temp_window_size = 300;
    c->trust.stuck_temp_threshold_c = 0.001;
    c->trust.min_window_completeness = 0.9;
    c->trust.max_staleness_ms = 10000;
    c->trust.max_future_skew_ms = 2000;
    c->trust.require_time_sync = true;
    c->trust.max_temp_rate_c_per_s = 2.0;
    c->trust.window_timing_tolerance_ms = 100;
    c->trust.history_reset_gap = 30;
    c->window.completeness_window = 60;
    c->window.min_completeness = 0.8;
    c->window.min_span = 30;
    c->window.stream_timeout_ms = 10000;
    c->mode.enabled = true;
    c->mode.startup_ms = 10000;
    c->cross.enabled = true;
    c->cross.grace_ms = 5000;
    c->health.std_floor[ALGA_F_VIB_RMS] = 0.005;
    c->health.std_floor[ALGA_F_TEMP] = 0.3;
    c->health.std_floor[ALGA_F_CURRENT] = 0.05;
}

static AlgaBaseline steady_baseline(void)
{
    AlgaBaselineBuilder b;
    alga_baseline_builder_init(&b, ALGA_DEFAULT_FEATURE_MASK);
    for (int i = 0; i < 60; i++) {
        double x[ALGA_MAX_FEATURES] = { 0.42 + 0.004 * ((i % 5) - 2), 0.0,
                                        30.0 + 0.1 * ((i % 7) - 3), 1.80 + 0.01 * ((i % 3) - 1) };
        alga_baseline_builder_add(&b, x, ALGA_DEFAULT_FEATURE_MASK);
    }
    AlgaBaseline bl;
    alga_baseline_builder_finish(&b, ALGA_MODE_STEADY, 10, &bl);
    bl.id = 77;
    return bl;
}

/* ------------------------------------------------------------------------ */
/* 1. legacy equivalence                                                     */
/* ------------------------------------------------------------------------ */

typedef struct {
    int n;
    AlgaDecision *d;
} Collect;

static void collect_cb(void *user, int row, const AlgaWindow *w, const AlgaDecision *d,
                       int parse_error)
{
    (void)row; (void)w; (void)parse_error;
    Collect *c = (Collect *)user;
    if (c->n < 256) c->d[c->n++] = *d;
}

static int check_legacy_file(const char *rel)
{
    char base[512], input[512];
    src_path(base, sizeof(base), "data/baseline_normal.csv");
    src_path(input, sizeof(input), rel);

    /* Engine path (legacy profile). */
    AlgaEngineConfig cfg;
    algaops_engine_config_default(&cfg);
    AlgaEngine *e = algaops_engine_create(&cfg);
    ASSERT(e != NULL, "engine create");
    AlgaBaseline bl[ALGA_MODE_COUNT];
    int nv = 0;
    ASSERT(algaops_baselines_from_csv(&cfg, base, bl, &nv) == ALGAOPS_OK && nv == 1,
           "single legacy STEADY baseline");
    algaops_engine_set_baseline(e, &bl[ALGA_MODE_STEADY]);
    Collect col = { 0, (AlgaDecision *)calloc(256, sizeof(AlgaDecision)) };
    ASSERT(col.d != NULL, "alloc");
    algaops_replay_csv(e, input, collect_cb, &col, NULL);
    algaops_engine_destroy(e);

    /* Original v1 path. */
    TrustGuardConfig tg_cfg = trustguard_default_config();
    TrustGuardState *tg = (TrustGuardState *)malloc(sizeof(*tg));
    ASSERT(tg != NULL, "alloc");
    trustguard_init(tg, &tg_cfg);
    EquipmentHealthConfig eh_cfg = equipment_health_default_config();
    EquipmentHealthState eh;
    equipment_health_init(&eh, &eh_cfg);
    ASSERT(equipment_health_load_baseline(&eh, base, &tg_cfg) == ALGAOPS_OK, "legacy baseline");
    ASSERT_NEAR(eh.baseline.vib_mean_g, bl[ALGA_MODE_STEADY].mean[ALGA_F_VIB_RMS], 1e-15,
                "baseline mean identical");
    ASSERT_NEAR(eh.baseline.temp_std_c, bl[ALGA_MODE_STEADY].std[ALGA_F_TEMP], 1e-15,
                "baseline std identical");

    CsvReader r;
    ASSERT(csv_reader_open(&r, input) == ALGAOPS_OK, "open");
    int i = 0;
    for (;;) {
        AlgaRecord rec;
        AlgaopsStatus s = csv_reader_next(&r, &rec);
        if (s == ALGAOPS_ERR_IO) break;
        ASSERT(i < col.n, "engine produced a decision per row");
        const AlgaDecision *d = &col.d[i++];
        if (s == ALGAOPS_ERR_PARSE) {
            ASSERT(d->outcome == DIAG_CHECK_SENSOR && d->flags == INTEGRITY_MALFORMED,
                   "malformed row => CHECK_SENSOR/MALFORMED");
            continue;
        }
        TrustResult tr;
        HealthResult hr;
        trustguard_validate(tg, &rec, &tr);
        equipment_health_evaluate(&eh, &rec, &tr, &hr);
        ASSERT(d->outcome == (int32_t)hr.outcome, "outcome identical");
        ASSERT(d->trust_score == tr.trust_score, "trust score identical");
        ASSERT(d->flags == tr.flags, "flags identical");
        ASSERT((d->is_trusted != 0) == tr.is_trusted, "trust decision identical");
        ASSERT(strcmp(d->explanation, tr.explanation) == 0, "explanation identical");
        ASSERT(strcmp(d->reason, hr.reason) == 0, "reason identical");
        if (hr.outcome != DIAG_CHECK_SENSOR) {
            ASSERT(d->methods[ALGA_HM_ZSCORE].feature_z[ALGA_F_VIB_RMS] == hr.vib_z_score,
                   "vib z identical");
            ASSERT(d->methods[ALGA_HM_ZSCORE].feature_z[ALGA_F_TEMP] == hr.temp_z_score,
                   "temp z identical");
        }
    }
    ASSERT(i == col.n, "row counts identical");
    csv_reader_close(&r);
    free(tg);
    free(col.d);
    return 0;
}

static int test_legacy_equivalence_all_v1_data(void)
{
    static const char *FILES[] = {
        "data/scenario_normal.csv", "data/scenario_missing_packet.csv",
        "data/scenario_stuck_sensor.csv", "data/scenario_equipment_anomaly.csv",
        "data/scenario_bad_timestamp.csv", "data/scenario_duplicate_sequence.csv",
        "data/scenario_out_of_order.csv", "data/scenario_out_of_range.csv",
        "data/scenario_stuck_temperature.csv", "data/scenario_suspicious_plus_blocking.csv",
        "data/scenario_malformed.csv", "data/baseline_normal.csv",
    };
    for (size_t k = 0; k < sizeof(FILES) / sizeof(FILES[0]); k++) {
        if (check_legacy_file(FILES[k]) != 0) {
            fprintf(stderr, "  legacy mismatch in %s\n", FILES[k]);
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* 2. golden scenarios                                                       */
/* ------------------------------------------------------------------------ */

typedef struct {
    char outcomes[512];
    int  n;
    IntegrityFlags seen;
} Golden;

static void golden_cb(void *user, int row, const AlgaWindow *w, const AlgaDecision *d,
                      int parse_error)
{
    (void)row; (void)w; (void)parse_error;
    Golden *g = (Golden *)user;
    static const char L[4] = { 'N', 'S', 'E', 'X' };
    if (g->n < (int)sizeof(g->outcomes) - 1 && d->outcome >= 0 && d->outcome <= 3) {
        g->outcomes[g->n++] = L[d->outcome];
        g->outcomes[g->n] = '\0';
    }
    g->seen |= d->flags;
}

static IntegrityFlags flag_by_name(const char *name)
{
    for (int b = 0; b < INTEGRITY_FLAG_COUNT; b++) {
        if (strcmp(trustguard_flag_name(1u << b), name) == 0) return 1u << b;
    }
    return 0;
}

static int test_golden_scenarios(void)
{
    char path[512];
    FILE *f = fopen(src_path(path, sizeof(path), "tests/golden/scenario_outcomes.csv"), "r");
    ASSERT(f != NULL, "golden file");
    char line[1024];
    int checked = 0, failed = 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || strncmp(line, "scenario,", 9) == 0) continue;
        line[strcspn(line, "\r\n")] = '\0';
        if (!line[0]) continue;
        char *fields[5] = { 0 };
        int nf = 0;
        char *p = line;
        while (nf < 5) {
            fields[nf++] = p;
            char *c = strchr(p, ',');
            if (!c) break;
            *c = '\0';
            p = c + 1;
        }
        if (nf < 4) { failed++; continue; }

        AlgaEngineConfig cfg;
        algaops_engine_config_default(&cfg);
        if (strcmp(fields[1], "policy") == 0) {
            char pol[512], err[512];
            src_path(pol, sizeof(pol), "../gateway/config/policy.default.toml");
            if (algaops_config_load_file(&cfg, pol, err, sizeof(err)) != ALGAOPS_OK) {
                fprintf(stderr, "  policy load failed: %s\n", err);
                failed++;
                continue;
            }
        }
        AlgaEngine *e = algaops_engine_create(&cfg);
        if (!e) { failed++; continue; }
        if (fields[2][0]) {
            char bp[512];
            AlgaBaseline bl[ALGA_MODE_COUNT];
            int nv = 0;
            src_path(bp, sizeof(bp), fields[2]);
            if (algaops_baselines_from_csv(&cfg, bp, bl, &nv) == ALGAOPS_OK) {
                for (int m = 1; m < ALGA_MODE_COUNT; m++) {
                    if (bl[m].valid) algaops_engine_set_baseline(e, &bl[m]);
                }
            }
        }
        char ip[512];
        Golden g;
        memset(&g, 0, sizeof(g));
        algaops_replay_csv(e, src_path(ip, sizeof(ip), fields[0]), golden_cb, &g, NULL);
        algaops_engine_destroy(e);
        checked++;
        if (strcmp(g.outcomes, fields[3]) != 0) {
            fprintf(stderr, "  %s: got %s want %s\n", fields[0], g.outcomes, fields[3]);
            failed++;
            continue;
        }
        if (nf >= 5 && fields[4][0]) {
            char *tok = fields[4];
            while (tok && *tok) {
                char *bar = strchr(tok, '|');
                if (bar) *bar = '\0';
                IntegrityFlags fl = flag_by_name(tok);
                if (!fl || !(g.seen & fl)) {
                    fprintf(stderr, "  %s: required flag %s not raised\n", fields[0], tok);
                    failed++;
                }
                tok = bar ? bar + 1 : NULL;
            }
        }
    }
    fclose(f);
    ASSERT(checked >= 11, "all golden scenarios evaluated");
    ASSERT(failed == 0, "golden outcomes must match");
    return 0;
}

/* ------------------------------------------------------------------------ */
/* 3. randomized trust-gate invariants                                       */
/* ------------------------------------------------------------------------ */

static uint32_t g_rng = 0xC0FFEEu;
static int g_fixed_always_on = 0;
static uint32_t rnd(void) { g_rng = g_rng * 1664525u + 1013904223u; return g_rng >> 8; }
static double rndu(void) { return (double)(rnd() & 0xFFFFF) / (double)0xFFFFF; }

static int check_invariants(const AlgaDecision *d)
{
    ASSERT(d->trust_score >= 0 && d->trust_score <= 100, "score range");
    ASSERT(d->reason[0] != '\0' && d->explanation[0] != '\0', "always explained");
    if (d->flags & trustguard_blocking_flags()) ASSERT(!d->is_trusted, "blocking => untrusted");
    if (!d->is_trusted) {
        ASSERT(d->outcome == DIAG_CHECK_SENSOR, "INVARIANT 1: untrusted => CHECK_SENSOR");
        for (int m = 0; m < ALGA_HM_COUNT; m++) {
            ASSERT(d->methods[m].status == ALGA_METHOD_SKIPPED,
                   "INVARIANT 2: health methods withheld on untrusted data");
        }
        ASSERT(d->mode == ALGA_MODE_UNKNOWN, "mode not inferred from untrusted data");
    } else {
        ASSERT(d->outcome != DIAG_CHECK_SENSOR, "trusted data never reported as sensor issue");
    }
    if (d->outcome == DIAG_CHECK_EQUIPMENT) {
        ASSERT(d->is_trusted, "INVARIANT 3: CHECK_EQUIPMENT only from trusted data");
        ASSERT(d->decided_by >= 0 && d->methods[d->decided_by].status == ALGA_METHOD_OK &&
               d->methods[d->decided_by].anomalous, "CHECK_EQUIPMENT backed by a method");
    }
    if (d->outcome == DIAG_NORMAL) {
        ASSERT(d->is_trusted, "INVARIANT 4: NORMAL only from trusted data");
        ASSERT(d->primary_method >= 0 && d->decided_by == d->primary_method &&
               d->methods[d->primary_method].status == ALGA_METHOD_OK &&
               !d->methods[d->primary_method].anomalous, "NORMAL meets configured criteria");
        ASSERT(!(d->methods[ALGA_HM_FIXED].status == ALGA_METHOD_OK &&
                 d->methods[ALGA_HM_FIXED].anomalous && d->decided_by != ALGA_HM_FIXED &&
                 g_fixed_always_on), "NORMAL never above an always-on fixed limit");
    }
    if (d->outcome == DIAG_NOT_EVALUATED) {
        ASSERT(d->is_trusted, "NOT_EVALUATED only for trusted data");
        ASSERT(d->primary_method < 0 || d->methods[d->primary_method].status != ALGA_METHOD_OK,
               "NOT_EVALUATED only when the primary method cannot run");
    }
    return 0;
}

static int test_trust_gate_invariants_randomized(void)
{
    AlgaEngineConfig cfg;
    policy_like_config(&cfg);
    cfg.health.fixed_always_on = true;
    g_fixed_always_on = 1;
    AlgaEngine *e = algaops_engine_create(&cfg);
    ASSERT(e != NULL, "engine");
    AlgaBaseline bl = steady_baseline();
    ASSERT(algaops_engine_set_baseline(e, &bl) == ALGAOPS_OK, "baseline");

    uint32_t boot = 1, seq = 0;
    uint64_t ts = 1700000000000ull;
    int counts[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < 6000; i++) {
        seq++;
        ts += 1280;
        double vib = 0.42 + (rndu() - 0.5) * 0.02;
        if (rnd() % 23 == 0) vib *= 3.0 + rndu() * 4.0;          /* equipment-like anomaly */
        double cur = (i / 400) % 2 ? 1.8 + (rndu() - 0.5) * 0.04 : 0.05;
        if (cur < 0.5) vib = 0.01 + rndu() * 0.01;              /* pump off */
        AlgaWindow w = make_v2(boot, seq, ts, vib, 30.0 + (rndu() - 0.5) * 0.4, cur);
        switch (rnd() % 40) {
            case 0: w.vibration_rms_g = NAN; break;
            case 1: w.temperature_c = 999.0; break;
            case 2: w.sequence_id = seq - 1; break;                 /* duplicate */
            case 3: seq += 3; w.sequence_id = seq; break;            /* gap */
            case 4: w.received_ms = ts + 60000; break;               /* stale */
            case 5: w.received_ms = ts - 9000; break;                /* future */
            case 6: w.time_synced = 0; break;
            case 7: w.samples_valid = 700; break;
            case 8: w.self_test = ALGA_SELF_TEST_FAIL; break;
            case 9: w.node_fault_flags = ALGA_NODE_FAULT_TEMP_CRC; break;
            case 10: w.vib_p2p_axis_g[1] = 0.0001; break;            /* p2p < rms */
            case 11: for (int a = 0; a < 3; a++) w.vib_p2p_axis_g[a] = 0.0; break;
            case 12: w.temperature_c += 25.0; break;                 /* spike */
            case 13: boot++; seq = 1; w.boot_id = boot; w.sequence_id = 1; break;
            case 14: w.current_a = 0.0; break;                       /* CT disagreement */
            case 15: w.timestamp_ms = ts - 5000; break;              /* regression */
            default: break;
        }
        algaops_engine_process(e, &w, &g_d);
        if (check_invariants(&g_d) != 0) {
            fprintf(stderr, "  violated at i=%d flags=0x%x reason=%s\n", i, g_d.flags, g_d.reason);
            algaops_engine_destroy(e);
            return 1;
        }
        counts[g_d.outcome]++;
    }
    algaops_engine_destroy(e);
    ASSERT(counts[DIAG_NORMAL] > 100, "random stream exercises NORMAL");
    ASSERT(counts[DIAG_CHECK_SENSOR] > 100, "random stream exercises CHECK_SENSOR");
    ASSERT(counts[DIAG_CHECK_EQUIPMENT] > 20, "random stream exercises CHECK_EQUIPMENT");
    return 0;
}

/* ------------------------------------------------------------------------ */
/* 4. v2 integrity checks                                                    */
/* ------------------------------------------------------------------------ */

typedef struct { const char *name; IntegrityFlags expect; } Case;

static int run_v2_case(int which, IntegrityFlags expect)
{
    AlgaEngineConfig cfg;
    policy_like_config(&cfg);
    cfg.cross.enabled = false;
    AlgaEngine *e = algaops_engine_create(&cfg);
    AlgaBaseline bl = steady_baseline();
    algaops_engine_set_baseline(e, &bl);
    uint64_t ts = 1700000000000ull;
    for (uint32_t s = 1; s <= 3; s++) {
        AlgaWindow ok = make_v2(5, s, ts + s * 1280, 0.42, 30.0 + 0.05 * s, 1.8);
        algaops_engine_process(e, &ok, &g_d);
    }
    AlgaWindow w = make_v2(5, 4, ts + 4 * 1280, 0.42, 30.1, 1.8);
    switch (which) {
        case 0: w.received_ms = w.timestamp_ms + 20000; break;
        case 1: w.received_ms = w.timestamp_ms - 5000; break;
        case 2: w.time_synced = 0; break;
        case 3: w.samples_valid = 500; break;
        case 4: w.self_test = ALGA_SELF_TEST_FAIL; break;
        case 5: w.node_fault_flags = ALGA_NODE_FAULT_ACCEL_BUS | ALGA_NODE_FAULT_TEMP_POR; break;
        case 6: w.vib_p2p_axis_g[0] = 0.01; break;
        case 7: w.vibration_rms_g = 0.9; break;                    /* != sqrt(sum axes^2) */
        case 8: w.temperature_c = 45.0; break;                     /* +15 C in 1.28 s */
        case 9: for (int a = 0; a < 3; a++) w.vib_p2p_axis_g[a] = 0.0; break;
        case 10: w.current_a = 500.0; break;
        case 11: w.t_start_ms = w.timestamp_ms - 5000; break;      /* duration mismatch */
        case 12: w.samples_valid = 2000; break;                    /* valid > expected */
        default: break;
    }
    algaops_engine_process(e, &w, &g_d);
    algaops_engine_destroy(e);
    /* 0 = expected flag raised and outcome CHECK_SENSOR, 1 = anything else */
    if (!(g_d.flags & expect)) return 1;
    return (!g_d.is_trusted && g_d.outcome == DIAG_CHECK_SENSOR) ? 0 : 1;
}

static int test_v2_integrity_checks(void)
{
    static const Case CASES[] = {
        { "stale", INTEGRITY_STALE }, { "future", INTEGRITY_FUTURE_TIMESTAMP },
        { "unsynced", INTEGRITY_CLOCK_UNSYNCED }, { "incomplete", INTEGRITY_INCOMPLETE_WINDOW },
        { "self-test", INTEGRITY_SELF_TEST_FAIL }, { "node fault", INTEGRITY_SENSOR_FAULT },
        { "p2p<rms", INTEGRITY_INCONSISTENT }, { "magnitude", INTEGRITY_INCONSISTENT },
        { "spike", INTEGRITY_SPIKE }, { "frozen", INTEGRITY_VIB_STUCK },
        { "current range", INTEGRITY_INCONSISTENT }, { "timing", INTEGRITY_INCONSISTENT },
        { "counts", INTEGRITY_INCONSISTENT },
    };
    for (int i = 0; i < (int)(sizeof(CASES) / sizeof(CASES[0])); i++) {
        if (run_v2_case(i, CASES[i].expect) != 0) {
            fprintf(stderr, "  case '%s' failed (flags=0x%x)\n", CASES[i].name, g_d.flags);
            return 1;
        }
    }
    /* A clean v2 window stays trusted (control case). */
    run_v2_case(99, INTEGRITY_OK);
    ASSERT(g_d.is_trusted && g_d.flags == INTEGRITY_OK, "control: clean v2 window is trusted");
    ASSERT(g_d.outcome == DIAG_NORMAL, "control: clean window within limits is NORMAL");
    return 0;
}

static int test_future_timestamp_does_not_poison_reference(void)
{
    AlgaEngineConfig cfg;
    policy_like_config(&cfg);
    cfg.cross.enabled = false;
    AlgaEngine *e = algaops_engine_create(&cfg);
    uint64_t ts = 1700000000000ull;
    for (uint32_t s = 1; s <= 5; s++) {
        AlgaWindow w = make_v2(4, s, ts + s * 1280, 0.42 + 0.002 * (s % 3), 30.0, 1.8);
        algaops_engine_process(e, &w, &g_d);
    }
    AlgaWindow f = make_v2(4, 6, ts + 6 * 1280, 0.42, 30.0, 1.8);
    f.timestamp_ms += 60000;               /* node clock jumps 60 s ahead once */
    f.t_start_ms += 60000;
    f.received_ms = ts + 6 * 1280 + 150;
    algaops_engine_process(e, &f, &g_d);
    ASSERT(g_d.flags & INTEGRITY_FUTURE_TIMESTAMP, "future timestamp flagged");
    AlgaWindow n = make_v2(4, 7, ts + 7 * 1280, 0.421, 30.0, 1.8);
    algaops_engine_process(e, &n, &g_d);
    ASSERT(!(g_d.flags & INTEGRITY_BAD_TIMESTAMP), "next correct window is not regressive");
    ASSERT(g_d.is_trusted, "stream recovers immediately");
    algaops_engine_destroy(e);
    return 0;
}

static int test_reboot_and_superseded_session(void)
{
    AlgaEngineConfig cfg;
    algaops_engine_config_default(&cfg);
    AlgaEngine *e = algaops_engine_create(&cfg);
    AlgaBaseline bl = steady_baseline();
    algaops_engine_set_baseline(e, &bl);
    for (uint32_t s = 100; s < 106; s++) {
        AlgaWindow w = make_window(0xAAAAu, s, 1000ull * s, 0.42 + 0.002 * (s % 3), 30.0 + 0.1 * (s % 4));
        algaops_engine_process(e, &w, &g_d);
    }
    /* reboot: new boot id, sequence restarts at 1 */
    AlgaWindow r = make_window(0xBBBBu, 1, 107000, 0.421, 30.2);
    algaops_engine_process(e, &r, &g_d);
    ASSERT(g_d.flags & INTEGRITY_REBOOT, "reboot detected");
    ASSERT(!(g_d.flags & INTEGRITY_OUT_OF_ORDER_SEQ), "sequence restart is not out-of-order");
    ASSERT(g_d.is_trusted && g_d.outcome == DIAG_NORMAL, "reboot alone is informational");
    AlgaWindow n = make_window(0xBBBBu, 2, 108000, 0.419, 30.1);
    algaops_engine_process(e, &n, &g_d);
    ASSERT(g_d.flags == INTEGRITY_OK, "continues cleanly in the new session");
    /* late packet from the old boot session */
    AlgaWindow old = make_window(0xAAAAu, 106, 109000, 0.42, 30.0);
    algaops_engine_process(e, &old, &g_d);
    ASSERT(g_d.flags & INTEGRITY_OUT_OF_ORDER_SEQ, "superseded session => out-of-order");
    ASSERT(g_d.outcome == DIAG_CHECK_SENSOR, "and CHECK_SENSOR");
    algaops_engine_destroy(e);
    return 0;
}

static int test_low_completeness_blocks(void)
{
    AlgaEngineConfig cfg;
    algaops_engine_config_default(&cfg);
    cfg.window.completeness_window = 20;
    cfg.window.min_completeness = 0.8;
    cfg.window.min_span = 10;
    AlgaEngine *e = algaops_engine_create(&cfg);
    AlgaBaseline bl = steady_baseline();
    algaops_engine_set_baseline(e, &bl);
    uint32_t seq = 0;
    int low_seen = 0;
    for (int i = 0; i < 20; i++) {
        seq += (i % 2) ? 3 : 1;    /* lose two of every four */
        AlgaWindow w = make_window(1, seq, 1000ull * seq, 0.42 + 0.003 * (i % 3), 30.0 + 0.1 * (i % 5));
        algaops_engine_process(e, &w, &g_d);
        if (g_d.flags & INTEGRITY_LOW_COMPLETENESS) {
            low_seen = 1;
            ASSERT(g_d.outcome == DIAG_CHECK_SENSOR, "sustained loss => CHECK_SENSOR");
            ASSERT(g_d.completeness < 0.8, "completeness reported");
        }
    }
    ASSERT(low_seen, "LOW_COMPLETENESS raised under sustained loss");
    algaops_engine_destroy(e);
    return 0;
}

static int test_stream_timeout_and_malformed(void)
{
    AlgaEngineConfig cfg;
    algaops_engine_config_default(&cfg);
    cfg.window.stream_timeout_ms = 5000;
    AlgaEngine *e = algaops_engine_create(&cfg);
    AlgaBaseline bl = steady_baseline();
    algaops_engine_set_baseline(e, &bl);
    AlgaWindow w = make_window(1, 1, 10000, 0.42, 30.0);
    w.presence = ALGA_HAS_RECEIVED;
    w.received_ms = 10100;
    algaops_engine_process(e, &w, &g_d);
    ASSERT(g_d.outcome == DIAG_NORMAL, "fresh data NORMAL");
    int emitted = 0;
    algaops_engine_tick(e, 14000, &g_d, &emitted);
    ASSERT(!emitted, "no timeout before 5 s");
    algaops_engine_tick(e, 16000, &g_d, &emitted);
    ASSERT(emitted && g_d.kind == ALGA_DECISION_STREAM_TIMEOUT, "timeout decision emitted");
    ASSERT(g_d.outcome == DIAG_CHECK_SENSOR && (g_d.flags & INTEGRITY_STREAM_TIMEOUT),
           "silence is never left showing NORMAL");
    ASSERT(g_d.alert_event == ALGA_ALERT_OPENED, "alert opened for stale stream");
    algaops_engine_tick(e, 20000, &g_d, &emitted);
    ASSERT(!emitted, "emitted once per silence period");

    algaops_engine_report_malformed(e, 21000, "invalid JSON", &g_d);
    ASSERT(g_d.kind == ALGA_DECISION_MALFORMED && g_d.outcome == DIAG_CHECK_SENSOR, "malformed");
    ASSERT(g_d.flags == INTEGRITY_MALFORMED && strstr(g_d.reason, "invalid JSON"), "explained");
    algaops_engine_destroy(e);

    /* With confirm windows > 1 a silent stream must still open an alert: no
     * further windows arrive to confirm it, the timeout itself is the filter. */
    cfg.alert_confirm_windows = 3;
    e = algaops_engine_create(&cfg);
    algaops_engine_set_baseline(e, &bl);
    algaops_engine_process(e, &w, &g_d);
    algaops_engine_tick(e, 16000, &g_d, &emitted);
    ASSERT(emitted && g_d.alert_event == ALGA_ALERT_OPENED && g_d.alert_kind == DIAG_CHECK_SENSOR,
           "timeout opens alert despite confirm_windows=3");
    algaops_engine_report_malformed(e, 17000, "bad", &g_d);
    algaops_engine_destroy(e);
    e = algaops_engine_create(&cfg);
    algaops_engine_report_malformed(e, 17000, "bad", &g_d);
    ASSERT(g_d.alert_event == ALGA_ALERT_NONE, "a single malformed record still needs confirmation");
    algaops_engine_destroy(e);

    /* Warm start: an alert persisted by the gateway (here: from a stream
     * timeout) is restored and then cleared by fresh NORMAL windows. */
    cfg.alert_clear_windows = 2;
    e = algaops_engine_create(&cfg);
    algaops_engine_set_baseline(e, &bl);
    ASSERT(algaops_engine_restore_alert(e, 7) == ALGAOPS_ERR_RANGE, "invalid kind rejected");
    ASSERT(algaops_engine_restore_alert(e, DIAG_CHECK_SENSOR) == ALGAOPS_OK, "restored");
    AlgaWindow w2 = make_window(1, 1, 30000, 0.42, 30.0);
    algaops_engine_process(e, &w2, &g_d);
    ASSERT(g_d.outcome == DIAG_NORMAL && g_d.alert_event == ALGA_ALERT_NONE &&
           g_d.alert_kind == DIAG_CHECK_SENSOR, "restored alert stays open");
    w2 = make_window(1, 2, 31280, 0.421, 30.0);
    algaops_engine_process(e, &w2, &g_d);
    ASSERT(g_d.alert_event == ALGA_ALERT_CLEARED, "restored alert clears");
    algaops_engine_destroy(e);
    return 0;
}

static int test_modes_methods_and_not_evaluated(void)
{
    AlgaEngineConfig cfg;
    policy_like_config(&cfg);
    cfg.cross.enabled = false;
    AlgaEngine *e = algaops_engine_create(&cfg);
    uint64_t ts = 1700000000000ull;
    uint32_t seq = 0;
    /* OFF: fixed limits */
    AlgaWindow w = make_v2(9, ++seq, ts += 1280, 0.01, 29.0, 0.05);
    algaops_engine_process(e, &w, &g_d);
    ASSERT(g_d.mode == ALGA_MODE_OFF && g_d.primary_method == ALGA_HM_FIXED, "OFF uses fixed");
    ASSERT(g_d.outcome == DIAG_NORMAL, "idle within limits");
    /* switch on: STARTUP for 10 s, fixed limits */
    w = make_v2(9, ++seq, ts += 1280, 0.45, 29.1, 1.9);
    algaops_engine_process(e, &w, &g_d);
    ASSERT(g_d.mode == ALGA_MODE_STARTUP && g_d.mode_changed, "STARTUP after switch-on");
    /* run past startup without a STEADY baseline => NOT_EVALUATED */
    for (int i = 0; i < 10; i++) {
        w = make_v2(9, ++seq, ts += 1280, 0.42 + 0.002 * (i % 3), 29.2 + 0.05 * (i % 4), 1.8);
        algaops_engine_process(e, &w, &g_d);
    }
    ASSERT(g_d.mode == ALGA_MODE_STEADY, "STEADY after startup");
    ASSERT(g_d.outcome == DIAG_NOT_EVALUATED, "no STEADY baseline => NOT_EVALUATED, not NORMAL");
    ASSERT(strstr(g_d.reason, "baseline") != NULL, "reason names the missing baseline");
    /* with a baseline: z-score decides; Mahalanobis + fixed computed as shadows */
    AlgaBaseline bl = steady_baseline();
    algaops_engine_set_baseline(e, &bl);
    w = make_v2(9, ++seq, ts += 1280, 0.42, 30.0, 1.8);
    algaops_engine_process(e, &w, &g_d);
    ASSERT(g_d.outcome == DIAG_NORMAL && g_d.primary_method == ALGA_HM_ZSCORE, "zscore NORMAL");
    ASSERT(g_d.baseline_id == 77, "baseline id traced");
    ASSERT(g_d.methods[ALGA_HM_MAHALANOBIS].status == ALGA_METHOD_OK, "shadow Mahalanobis");
    ASSERT(g_d.methods[ALGA_HM_FIXED].status == ALGA_METHOD_OK, "shadow fixed");
    w = make_v2(9, ++seq, ts += 1280, 0.80, 30.0, 1.8);
    algaops_engine_process(e, &w, &g_d);
    ASSERT(g_d.outcome == DIAG_CHECK_EQUIPMENT, "trusted deviation => CHECK_EQUIPMENT");
    ASSERT(strstr(g_d.reason, "STEADY") && strstr(g_d.reason, "vibration"), "reason");
    algaops_engine_destroy(e);
    return 0;
}

static int test_fixed_limits_always_on(void)
{
    AlgaEngineConfig cfg;
    policy_like_config(&cfg);
    cfg.cross.enabled = false;
    cfg.mode.startup_ms = 0;
    cfg.health.fixed_always_on = true;
    cfg.health.feature_mask &= ~ALGA_FMASK(ALGA_F_TEMP);   /* temperature via fixed limit only */
    cfg.health.fixed_temp_max_c[ALGA_MODE_STEADY] = 45.0;
    cfg.trust.max_temp_rate_c_per_s = 0.0;
    AlgaEngine *e = algaops_engine_create(&cfg);
    uint64_t ts = 1700000000000ull;
    AlgaWindow w = make_v2(3, 1, ts, 0.42, 30.0, 1.8);
    algaops_engine_process(e, &w, &g_d);
    ASSERT(g_d.mode == ALGA_MODE_STEADY && g_d.outcome == DIAG_NOT_EVALUATED,
           "no baseline, within limits => NOT_EVALUATED");
    w = make_v2(3, 2, ts + 1280, 0.42, 47.5, 1.8);
    algaops_engine_process(e, &w, &g_d);
    ASSERT(g_d.outcome == DIAG_CHECK_EQUIPMENT && g_d.decided_by == ALGA_HM_FIXED,
           "trusted window above the absolute limit => CHECK_EQUIPMENT even without baseline");
    ASSERT(strstr(g_d.reason, "fixed limit") && strstr(g_d.reason, "not evaluated"), "reason");
    AlgaBaseline bl = steady_baseline();
    algaops_engine_set_baseline(e, &bl);
    w = make_v2(3, 3, ts + 2560, 0.42, 47.6, 1.8);
    algaops_engine_process(e, &w, &g_d);
    ASSERT(g_d.outcome == DIAG_CHECK_EQUIPMENT && g_d.decided_by == ALGA_HM_FIXED &&
           !g_d.methods[ALGA_HM_ZSCORE].anomalous, "hard limit wins over a quiet z-score");
    w = make_v2(3, 4, ts + 3840, 0.42, 31.0, 1.8);
    algaops_engine_process(e, &w, &g_d);
    ASSERT(g_d.outcome == DIAG_NORMAL && g_d.decided_by == ALGA_HM_ZSCORE, "back to NORMAL");
    algaops_engine_destroy(e);
    return 0;
}

static int test_alert_lifecycle(void)
{
    AlgaEngineConfig cfg;
    algaops_engine_config_default(&cfg);
    cfg.alert_confirm_windows = 3;
    cfg.alert_clear_windows = 2;
    AlgaEngine *e = algaops_engine_create(&cfg);
    AlgaBaseline bl = steady_baseline();
    algaops_engine_set_baseline(e, &bl);
    /* outcome script: S S S N N E E E S S S */
    const char *script = "SSSNNEEESSS";
    const int expect[] = { ALGA_ALERT_NONE, ALGA_ALERT_NONE, ALGA_ALERT_OPENED, ALGA_ALERT_NONE,
                           ALGA_ALERT_CLEARED, ALGA_ALERT_NONE, ALGA_ALERT_NONE, ALGA_ALERT_OPENED,
                           ALGA_ALERT_NONE, ALGA_ALERT_NONE, ALGA_ALERT_CHANGED };
    for (int i = 0; script[i]; i++) {
        uint32_t seq = (uint32_t)(i + 1);
        double vib = script[i] == 'E' ? 0.9 + 0.01 * i : 0.42 + 0.003 * (i % 3);
        AlgaWindow w = make_window(1, seq, 1000ull * seq, vib, 30.0 + 0.1 * (i % 4));
        if (script[i] == 'S') w.vibration_rms_g = -1.0;
        algaops_engine_process(e, &w, &g_d);
        ASSERT(g_d.outcome == (script[i] == 'S' ? DIAG_CHECK_SENSOR :
                               script[i] == 'E' ? DIAG_CHECK_EQUIPMENT : DIAG_NORMAL), "script");
        if (g_d.alert_event != expect[i]) {
            fprintf(stderr, "  step %d: event %d want %d\n", i, g_d.alert_event, expect[i]);
            algaops_engine_destroy(e);
            return 1;
        }
    }
    ASSERT(g_d.alert_kind == DIAG_CHECK_SENSOR && g_d.alert_prev_kind == DIAG_CHECK_EQUIPMENT,
           "changed alert records both kinds");
    algaops_engine_destroy(e);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* 5. configuration and ABI                                                  */
/* ------------------------------------------------------------------------ */

static int test_config_keys_roundtrip(void)
{
    AlgaEngineConfig c;
    algaops_engine_config_default(&c);
    int n = algaops_config_key_count();
    ASSERT(n >= 55, "policy key table populated");
    for (int i = 0; i < n; i++) {
        const char *k = algaops_config_key_name(i);
        if (algaops_config_key_is_string(k) == 1) {
            const char *v = algaops_config_get_string(&c, k);
            ASSERT(v && algaops_config_set_string(&c, k, v) == ALGAOPS_OK, "string round trip");
        } else {
            double v;
            ASSERT(algaops_config_get_number(&c, k, &v) == ALGAOPS_OK, "get");
            ASSERT(algaops_config_set_number(&c, k, v) == ALGAOPS_OK, "set same value");
        }
    }
    ASSERT(algaops_config_validate(&c, NULL, 0) == ALGAOPS_OK, "defaults validate");
    ASSERT(algaops_config_set_number(&c, "trust.nope", 1) == ALGAOPS_ERR_CONFIG, "unknown key");
    ASSERT(algaops_config_set_number(&c, "method.steady", 1) == ALGAOPS_ERR_CONFIG, "wrong type");
    ASSERT(algaops_config_set_number(&c, "trust.max_staleness_ms", -5) == ALGAOPS_ERR_RANGE,
           "negative u32");
    ASSERT(algaops_config_set_number(&c, "mode.enabled", 0.5) == ALGAOPS_ERR_RANGE, "bool range");
    ASSERT(algaops_config_set_number(&c, "trust.vib_max_g", NAN) == ALGAOPS_ERR_RANGE, "NaN");
    ASSERT(algaops_config_set_string(&c, "method.steady", "mahalanobis") == ALGAOPS_OK, "method");
    ASSERT(strcmp(algaops_config_get_string(&c, "method.steady"), "mahalanobis") == 0, "read back");
    ASSERT(algaops_config_set_string(&c, "method.off", "none") == ALGAOPS_OK, "none allowed");
    ASSERT(algaops_config_set_string(&c, "method.off", "magic") == ALGAOPS_ERR_RANGE, "bad method");
    ASSERT(algaops_config_set_number(&c, "health.features.current_a", 0) == ALGAOPS_OK, "fmask off");
    double v;
    algaops_config_get_number(&c, "health.features.current_a", &v);
    ASSERT(v == 0.0, "feature bit cleared");
    char err[256];
    algaops_config_set_number(&c, "mode.vib_off_g", 5.0);
    ASSERT(algaops_config_validate(&c, err, sizeof(err)) == ALGAOPS_ERR_CONFIG, "inconsistent");
    ASSERT(strstr(err, "vib_off_g") != NULL, "validation message names the key");
    ASSERT(algaops_engine_create(&c) == NULL, "engine refuses invalid config");
    return 0;
}

static int test_config_file_loader(void)
{
    const char *p = "engine_test_policy.toml";
    FILE *f = fopen(p, "wb");
    ASSERT(f, "write policy");
    fputs("# test policy\n[meta]\nname = \"test\"  # ignored\n\n[trust]\nvib_max_g = 20.5\n"
          "require_time_sync = true\n[method]\nsteady = \"mahalanobis\"\n"
          "health.z_threshold = 4.0\n", f);
    fclose(f);
    AlgaEngineConfig c;
    algaops_engine_config_default(&c);
    char err[256];
    /* dotted key inside [method] resolves to method.health.z_threshold => unknown */
    ASSERT(algaops_config_load_file(&c, p, err, sizeof(err)) == ALGAOPS_ERR_CONFIG,
           "unknown key rejected with location");
    ASSERT(strstr(err, ":10:") != NULL, "line number reported");
    f = fopen(p, "wb");
    fputs("health.z_threshold = 4.0\n[trust]\nvib_max_g = 20.5\nrequire_time_sync = true\n"
          "[method]\nsteady = \"mahalanobis\"\n", f);
    fclose(f);
    algaops_engine_config_default(&c);
    ASSERT(algaops_config_load_file(&c, p, err, sizeof(err)) == ALGAOPS_OK, "valid file");
    ASSERT(c.trust.vib_max_g == 20.5 && c.trust.require_time_sync, "values applied");
    ASSERT(c.primary_method[ALGA_MODE_STEADY] == ALGA_HM_MAHALANOBIS, "string value applied");
    ASSERT(c.health.z_threshold == 4.0, "top-level dotted key");
    remove(p);
    return 0;
}

static int test_abi_info(void)
{
    AlgaAbiInfo a;
    algaops_abi_info(&a);
    ASSERT(a.abi_version == ALGAOPS_ABI_VERSION, "abi version");
    ASSERT(a.size_window == sizeof(AlgaWindow) && a.size_decision == sizeof(AlgaDecision),
           "struct sizes");
    ASSERT(a.off_decision_reason == offsetof(AlgaDecision, reason), "offsets");
    ASSERT(strcmp(algaops_version(), ALGAOPS_VERSION_STR) == 0, "version");
    return 0;
}

int main(void)
{
    printf("=== Diagnostic Engine Tests ===\n");
    RUN_TEST(test_legacy_equivalence_all_v1_data);
    RUN_TEST(test_golden_scenarios);
    RUN_TEST(test_trust_gate_invariants_randomized);
    RUN_TEST(test_v2_integrity_checks);
    RUN_TEST(test_future_timestamp_does_not_poison_reference);
    RUN_TEST(test_reboot_and_superseded_session);
    RUN_TEST(test_low_completeness_blocks);
    RUN_TEST(test_stream_timeout_and_malformed);
    RUN_TEST(test_modes_methods_and_not_evaluated);
    RUN_TEST(test_fixed_limits_always_on);
    RUN_TEST(test_alert_lifecycle);
    RUN_TEST(test_config_keys_roundtrip);
    RUN_TEST(test_config_file_loader);
    RUN_TEST(test_abi_info);
    TEST_SUMMARY();
    return TEST_EXIT_CODE();
}
