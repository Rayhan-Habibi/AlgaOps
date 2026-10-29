/*
 * main.c — AlgaOps Assurance CLI entry point.
 *
 * Usage:
 *   algaops --baseline <baseline.csv> --input <input.csv> [--output <results.csv>]
 *           [--config <policy.toml>] [--json <decisions.jsonl>]
 *           [--evidence <dir>] [--node-id <id>] [--data-source <label>] [--quiet]
 *   algaops --verify-evidence <dir>
 *   algaops --hash <file>
 *   algaops --version | --help
 *
 * Without --config the engine runs the legacy profile, which reproduces the
 * original v1 behaviour and output format exactly.  For each input record
 * the program prints:
 *   - Timestamp, sequence ID
 *   - Trust score, trust decision, integrity flags, explanation
 *   - Diagnostic outcome and reason
 *
 * An optional CSV output file contains the same data in machine-readable form.
 * --json writes one decision object per line; --evidence additionally writes
 * an Ascon-Hash256 chained audit log, a manifest and a digest list that can
 * be checked with --verify-evidence or tools/verify_evidence.py.
 *
 * Exit codes:
 *   0  All records processed (some may be untrusted/anomalous) / evidence OK.
 *   1  Invalid arguments, missing files, or baseline load failure.
 *   2  No eligible input records were processed.
 *   3  Evidence verification failed.
 */

#include "../include/algaops_types.h"
#include "../include/trustguard.h"
#include "../include/equipment_health.h"
#include "../include/algaops_engine.h"
#include "../include/ascon_hash256.h"
#include "../include/audit_chain.h"
#include "csv_reader.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir((p), 0755)
#endif

/* =========================================================================
 * CLI argument parsing
 * ========================================================================= */

typedef struct {
    const char *baseline_path;
    const char *input_path;
    const char *output_path;   /* NULL if not specified */
    const char *config_path;
    const char *json_path;
    const char *evidence_dir;
    const char *verify_dir;
    const char *hash_path;
    const char *node_id;
    const char *data_source;
    int         quiet;
} CliArgs;

static void print_usage(const char *prog)
{
    fprintf(stderr,
            "AlgaOps Assurance v%s\n"
            "Usage:\n"
            "  %s --baseline <baseline.csv> --input <input.csv> "
            "[--output <results.csv>]\n"
            "      [--config <policy.toml>] [--json <decisions.jsonl>] "
            "[--evidence <dir>]\n"
            "      [--node-id <id>] [--data-source synthetic|hardware|replay|unknown] "
            "[--quiet]\n"
            "  %s --verify-evidence <dir>\n"
            "  %s --hash <file>\n"
            "\n"
            "Options:\n"
            "  --baseline  CSV file with representative normal-operation data.\n"
            "  --input     CSV file with sensor measurements to evaluate.\n"
            "  --output    (Optional) Write evaluation results to this CSV file.\n"
            "  --config    (Optional) Policy file; enables modes, methods, freshness.\n"
            "  --json      (Optional) Write one JSON decision per line.\n"
            "  --evidence  (Optional) Write hash-chained evidence bundle to <dir>.\n"
            "  --verify-evidence  Verify an evidence bundle written by --evidence.\n"
            "  --hash      Print the Ascon-Hash256 digest of a file.\n"
            "  --version   Print version information.\n",
            ALGAOPS_VERSION_STR, prog, prog, prog);
}

static int parse_args(int argc, char **argv, CliArgs *args)
{
    memset(args, 0, sizeof(*args));
    args->node_id = "cli";
    args->data_source = "unknown";

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
#define TAKE(field, what)                                                   \
        if (strcmp(a, what) == 0) {                                          \
            if (++i >= argc) {                                               \
                fprintf(stderr, "Error: %s requires a value.\n", what);      \
                return 1;                                                    \
            }                                                                \
            args->field = argv[i];                                           \
            continue;                                                        \
        }
        TAKE(baseline_path, "--baseline")
        TAKE(input_path, "--input")
        TAKE(output_path, "--output")
        TAKE(config_path, "--config")
        TAKE(json_path, "--json")
        TAKE(evidence_dir, "--evidence")
        TAKE(verify_dir, "--verify-evidence")
        TAKE(hash_path, "--hash")
        TAKE(node_id, "--node-id")
        TAKE(data_source, "--data-source")
#undef TAKE
        if (strcmp(a, "--quiet") == 0) {
            args->quiet = 1;
        } else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            print_usage(argv[0]);
            exit(0);
        } else if (strcmp(a, "--version") == 0) {
            printf("AlgaOps Assurance v%s (ABI %u)\n", ALGAOPS_VERSION_STR,
                   (unsigned)ALGAOPS_ABI_VERSION);
            exit(0);
        } else {
            fprintf(stderr, "Error: unknown option '%s'\n", a);
            return 1;
        }
    }

    if (args->verify_dir || args->hash_path) return 0;
    if (!args->baseline_path && !args->config_path) {
        fprintf(stderr, "Error: --baseline is required.\n");
        return 1;
    }
    if (!args->input_path) {
        fprintf(stderr, "Error: --input is required.\n");
        return 1;
    }
    return 0;
}

/* =========================================================================
 * Small IO helpers
 * ========================================================================= */

static int hash_file(const char *path, uint8_t out[ASCON_HASH256_BYTES])
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    AsconHash256Ctx ctx;
    ascon_hash256_init(&ctx);
    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) ascon_hash256_update(&ctx, buf, n);
    int err = ferror(f);
    fclose(f);
    if (err) return -1;
    ascon_hash256_final(&ctx, out);
    return 0;
}

/* Read one line (without trailing \r\n) into a growing buffer. */
static long read_line(FILE *f, char **buf, size_t *cap)
{
    size_t n = 0;
    int ch;
    if (!*buf) {
        *cap = 4096;
        *buf = (char *)malloc(*cap);
        if (!*buf) return -1;
    }
    while ((ch = fgetc(f)) != EOF) {
        if (n + 2 > *cap) {
            size_t nc = *cap * 2;
            char *nb = (char *)realloc(*buf, nc);
            if (!nb) return -1;
            *buf = nb;
            *cap = nc;
        }
        if (ch == '\n') break;
        (*buf)[n++] = (char)ch;
    }
    if (ch == EOF && n == 0) return -1;
    if (n > 0 && (*buf)[n - 1] == '\r') n--;
    (*buf)[n] = '\0';
    return (long)n;
}

static void path_join(char *out, size_t len, const char *dir, const char *name)
{
    size_t dl = strlen(dir);
    const char *sep = (dl > 0 && (dir[dl - 1] == '/' || dir[dl - 1] == '\\')) ? "" : "/";
    snprintf(out, len, "%s%s%s", dir, sep, name);
}

/* =========================================================================
 * Output formatting (legacy layout)
 * ========================================================================= */

static void print_separator(void)
{
    printf("%-70s\n", "----------------------------------------------------------------------");
}

static double method_z(const AlgaDecision *d, int feature)
{
    const AlgaMethodResult *r = &d->methods[ALGA_HM_ZSCORE];
    return r->status == ALGA_METHOD_OK ? r->feature_z[feature] : 0.0;
}

static void print_record_result(const AlgaWindow *w, const AlgaDecision *d, int extended)
{
    char flags_str[512];
    trustguard_flags_to_str(d->flags, flags_str, sizeof(flags_str));

    printf("  ts=%-8llu  seq=%-5u  vib=%.4fg  temp=%.2f°C\n",
           (unsigned long long)(w ? w->timestamp_ms : 0),
           (unsigned)(w ? w->sequence_id : 0),
           w ? w->vibration_rms_g : 0.0,
           w ? w->temperature_c : 0.0);
    printf("  TrustScore: %-3d  Trusted: %-5s  Flags: %s\n",
           d->trust_score,
           d->is_trusted ? "YES" : "NO",
           flags_str);
    printf("  Integrity:  %s\n", d->explanation);
    printf("  Diagnostic: %-16s  Reason: %s\n",
           diagnostic_outcome_str((DiagnosticOutcome)d->outcome),
           d->reason);
    if (d->outcome == DIAG_CHECK_EQUIPMENT && d->primary_method == ALGA_HM_ZSCORE) {
        printf("  Z-scores:   vib=%.2f  temp=%.2f\n",
               method_z(d, ALGA_F_VIB_RMS), method_z(d, ALGA_F_TEMP));
    }
    if (extended) {
        printf("  Mode:       %-8s  Method: %-11s  Alert: %s",
               algaops_mode_str(d->mode), algaops_method_str(d->primary_method),
               algaops_alert_event_str(d->alert_event));
        if (d->completeness >= 0.0) printf("  Completeness: %.0f%%", d->completeness * 100.0);
        printf("\n");
    }
}

static void write_csv_header(FILE *f)
{
    fprintf(f,
            "timestamp_ms,sequence_id,vibration_rms_g,temperature_c,"
            "trust_score,is_trusted,integrity_flags,"
            "diagnostic_outcome,vib_z_score,temp_z_score,reason\n");
}

static void write_csv_row(FILE *f, const AlgaWindow *w, const AlgaDecision *d)
{
    char flags_str[512];
    trustguard_flags_to_str(d->flags, flags_str, sizeof(flags_str));

    /* Escape reason field: double embedded quotes. */
    char reason_escaped[ALGAOPS_EXPLANATION_LEN * 2];
    const char *src = d->reason;
    char *dst = reason_escaped;
    while (*src && (dst - reason_escaped) < (int)sizeof(reason_escaped) - 2) {
        if (*src == '"') { *dst++ = '"'; *dst++ = '"'; }
        else              { *dst++ = *src; }
        src++;
    }
    *dst = '\0';

    fprintf(f,
            "%llu,%u,%.6f,%.4f,%d,%s,%s,%s,%.4f,%.4f,\"%s\"\n",
            (unsigned long long)(w ? w->timestamp_ms : 0),
            (unsigned)(w ? w->sequence_id : 0),
            w ? w->vibration_rms_g : 0.0,
            w ? w->temperature_c : 0.0,
            d->trust_score,
            d->is_trusted ? "true" : "false",
            flags_str,
            diagnostic_outcome_str((DiagnosticOutcome)d->outcome),
            method_z(d, ALGA_F_VIB_RMS),
            method_z(d, ALGA_F_TEMP),
            reason_escaped);
}

/* =========================================================================
 * JSON decision lines (deterministic formatting)
 * ========================================================================= */

typedef struct { char *buf; size_t len; size_t used; int overflow; } Js;

static void js_raw(Js *j, const char *s)
{
    size_t n = strlen(s);
    if (j->used + n + 1 > j->len) { j->overflow = 1; return; }
    memcpy(j->buf + j->used, s, n);
    j->used += n;
    j->buf[j->used] = '\0';
}

static void js_str(Js *j, const char *s)
{
    long n = audit_json_escape(s, strlen(s), j->buf + j->used, j->len - j->used);
    if (n < 0) { j->overflow = 1; return; }
    j->used += (size_t)n;
}

static void js_num(Js *j, double v)
{
    char tmp[64];
    if (!isfinite(v)) snprintf(tmp, sizeof(tmp), "null");
    else              snprintf(tmp, sizeof(tmp), "%.9g", v);
    js_raw(j, tmp);
}

static void js_u64(Js *j, unsigned long long v)
{
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "%llu", v);
    js_raw(j, tmp);
}

static void js_kv_str(Js *j, const char *k, const char *v, int comma)
{
    if (comma) js_raw(j, ",");
    js_str(j, k);
    js_raw(j, ":");
    js_str(j, v);
}

static const char *kind_str(int kind)
{
    switch (kind) {
        case ALGA_DECISION_STREAM_TIMEOUT: return "stream_timeout";
        case ALGA_DECISION_MALFORMED:      return "malformed";
        default:                           return "window";
    }
}

static const char *method_status_str(int s)
{
    switch (s) {
        case ALGA_METHOD_OK:          return "ok";
        case ALGA_METHOD_UNAVAILABLE: return "unavailable";
        default:                      return "skipped";
    }
}

static int decision_json(char *out, size_t len, int row, const char *node_id,
                         const AlgaDecision *d)
{
    Js j = { out, len, 0, 0 };
    out[0] = '\0';
    js_raw(&j, "{");
    js_kv_str(&j, "type", "decision", 0);
    js_raw(&j, ",\"row\":");          js_u64(&j, (unsigned long long)row);
    js_kv_str(&j, "kind", kind_str(d->kind), 1);
    js_kv_str(&j, "node_id", node_id, 1);
    js_raw(&j, ",\"boot_id\":");      js_u64(&j, d->boot_id);
    js_raw(&j, ",\"seq\":");          js_u64(&j, d->sequence_id);
    js_raw(&j, ",\"timestamp_ms\":"); js_u64(&j, (unsigned long long)d->timestamp_ms);
    js_raw(&j, ",\"received_ms\":");  js_u64(&j, (unsigned long long)d->received_ms);
    js_raw(&j, ",\"trust_score\":");  js_u64(&j, (unsigned long long)(d->trust_score < 0 ? 0 : d->trust_score));
    js_raw(&j, d->is_trusted ? ",\"is_trusted\":true" : ",\"is_trusted\":false");
    js_raw(&j, ",\"flags\":[");
    int first = 1;
    for (int b = 0; b < INTEGRITY_FLAG_COUNT; b++) {
        IntegrityFlags bit = 1u << b;
        if (!(d->flags & bit)) continue;
        if (!first) js_raw(&j, ",");
        js_str(&j, trustguard_flag_name(bit));
        first = 0;
    }
    js_raw(&j, "]");
    js_kv_str(&j, "outcome", diagnostic_outcome_str((DiagnosticOutcome)d->outcome), 1);
    js_kv_str(&j, "mode", algaops_mode_str(d->mode), 1);
    js_kv_str(&j, "method", algaops_method_str(d->primary_method), 1);
    js_raw(&j, ",\"baseline_id\":");  js_u64(&j, d->baseline_id);
    js_raw(&j, ",\"scores\":{");
    for (int m = 0; m < ALGA_HM_COUNT; m++) {
        const AlgaMethodResult *r = &d->methods[m];
        if (m) js_raw(&j, ",");
        js_str(&j, algaops_method_str(m));
        js_raw(&j, ":{");
        js_kv_str(&j, "status", method_status_str(r->status), 0);
        js_raw(&j, r->anomalous ? ",\"anomalous\":true" : ",\"anomalous\":false");
        js_raw(&j, ",\"score\":");     js_num(&j, r->score);
        js_raw(&j, ",\"threshold\":"); js_num(&j, r->threshold);
        js_raw(&j, "}");
    }
    js_raw(&j, "}");
    js_kv_str(&j, "alert_event", algaops_alert_event_str(d->alert_event), 1);
    js_raw(&j, ",\"completeness\":"); js_num(&j, d->completeness >= 0.0 ? d->completeness : NAN);
    js_kv_str(&j, "explanation", d->explanation, 1);
    js_kv_str(&j, "reason", d->reason, 1);
    js_raw(&j, "}");
    return j.overflow ? -1 : (int)j.used;
}

/* =========================================================================
 * Evaluation run
 * ========================================================================= */

typedef struct {
    const CliArgs *args;
    int    extended;
    FILE  *out_csv;
    FILE  *out_json;
    FILE  *ev_decisions;
    FILE  *ev_events;
    AuditChain chain;
    int    total, normal, check_sensor, check_equip, not_eval, parse_err;
    char  *line;
    char  *escaped;
    int    io_error;
} RunCtx;

#define LINE_CAP (16 * 1024)

static void on_decision(void *user, int row, const AlgaWindow *w,
                        const AlgaDecision *d, int parse_error)
{
    RunCtx *c = (RunCtx *)user;
    if (parse_error) c->parse_err++;
    else             c->total++;
    switch (d->outcome) {
        case DIAG_NORMAL:          c->normal++;       break;
        case DIAG_CHECK_SENSOR:    c->check_sensor++; break;
        case DIAG_CHECK_EQUIPMENT: c->check_equip++;  break;
        default:                   c->not_eval++;     break;
    }
    if (!c->args->quiet) {
        print_record_result(w, d, c->extended);
        printf("\n");
    }
    if (c->out_csv) write_csv_row(c->out_csv, w, d);

    if (c->out_json || c->ev_decisions) {
        int n = decision_json(c->line, LINE_CAP, row, c->args->node_id, d);
        if (n < 0) { c->io_error = 1; return; }
        if (c->out_json) fprintf(c->out_json, "%s\n", c->line);
        if (c->ev_decisions) {
            fprintf(c->ev_decisions, "%s\n", c->line);
            uint8_t h[AUDIT_HASH_BYTES];
            uint8_t prev[AUDIT_HASH_BYTES];
            memcpy(prev, c->chain.head, sizeof(prev));
            audit_chain_append(&c->chain, (const uint8_t *)c->line, (size_t)n, h);
            char ph[65], hh[65];
            ascon_hash256_hex(prev, ph);
            ascon_hash256_hex(h, hh);
            if (audit_json_escape(c->line, (size_t)n, c->escaped, 2 * LINE_CAP) < 0) {
                c->io_error = 1;
                return;
            }
            fprintf(c->ev_events,
                    "{\"seq\":%llu,\"ts_ms\":%llu,\"type\":\"decision\",\"node_id\":",
                    (unsigned long long)c->chain.seq,
                    (unsigned long long)d->timestamp_ms);
            char nid[256];
            if (audit_json_escape(c->args->node_id, strlen(c->args->node_id), nid,
                                  sizeof(nid)) < 0) {
                c->io_error = 1;
                return;
            }
            fprintf(c->ev_events, "%s,\"payload\":%s,\"prev_hash\":\"%s\",\"hash\":\"%s\"}\n",
                    nid, c->escaped, ph, hh);
        }
    }
}

static int write_manifest(const CliArgs *a, const AlgaEngineConfig *cfg, const RunCtx *c)
{
    char p_dec[1024], p_ev[1024], p_man[1024], p_sums[1024];
    path_join(p_dec, sizeof(p_dec), a->evidence_dir, "decisions.jsonl");
    path_join(p_ev, sizeof(p_ev), a->evidence_dir, "audit_events.jsonl");
    path_join(p_man, sizeof(p_man), a->evidence_dir, "manifest.json");
    path_join(p_sums, sizeof(p_sums), a->evidence_dir, "SHA-ASCON256SUMS");

    uint8_t h_dec[32], h_ev[32], h_in[32], h_bl[32], h_cfg[32];
    char x_dec[65], x_ev[65], x_in[65], x_bl[65] = "", x_cfg[65] = "", head[65];
    if (hash_file(p_dec, h_dec) || hash_file(p_ev, h_ev) || hash_file(a->input_path, h_in)) {
        return -1;
    }
    ascon_hash256_hex(h_dec, x_dec);
    ascon_hash256_hex(h_ev, x_ev);
    ascon_hash256_hex(h_in, x_in);
    if (a->baseline_path && hash_file(a->baseline_path, h_bl) == 0) ascon_hash256_hex(h_bl, x_bl);
    if (a->config_path && hash_file(a->config_path, h_cfg) == 0) ascon_hash256_hex(h_cfg, x_cfg);
    ascon_hash256_hex(c->chain.head, head);

    FILE *s = fopen(p_sums, "wb");
    if (!s) return -1;
    fprintf(s, "%s  decisions.jsonl\n%s  audit_events.jsonl\n", x_dec, x_ev);
    fclose(s);

    FILE *m = fopen(p_man, "wb");
    if (!m) return -1;
    char tbuf[32] = "";
    time_t now = time(NULL);
    struct tm *g = gmtime(&now);
    if (g) strftime(tbuf, sizeof(tbuf), "%Y-%m-%dT%H:%M:%SZ", g);
    fprintf(m, "{\n  \"schema\": \"algaops.evidence_manifest.v1\",\n");
    fprintf(m, "  \"producer\": \"algaops-cli %s\",\n", ALGAOPS_VERSION_STR);
    fprintf(m, "  \"created_utc\": \"%s\",\n", tbuf);
    fprintf(m, "  \"data_source\": \"%s\",\n", a->data_source);
    fprintf(m, "  \"hash_algorithm\": \"Ascon-Hash256 (NIST SP 800-232)\",\n");
    fprintf(m, "  \"chain\": {\"domain\": \"%s\", \"events\": %llu, \"head_hash\": \"%s\"},\n",
            AUDIT_DOMAIN, (unsigned long long)c->chain.seq, head);
    fprintf(m, "  \"files\": {\"decisions.jsonl\": \"%s\", \"audit_events.jsonl\": \"%s\"},\n",
            x_dec, x_ev);
    fprintf(m, "  \"inputs\": {\"input\": \"%s\", \"baseline\": \"%s\", \"config\": \"%s\"},\n",
            x_in, x_bl, x_cfg);
    fprintf(m, "  \"summary\": {\"records\": %d, \"parse_errors\": %d, \"NORMAL\": %d, "
               "\"CHECK_SENSOR\": %d, \"CHECK_EQUIPMENT\": %d, \"NOT_EVALUATED\": %d},\n",
            c->total, c->parse_err, c->normal, c->check_sensor, c->check_equip, c->not_eval);
    fprintf(m, "  \"policy\": {");
    for (int i = 0; i < algaops_config_key_count(); i++) {
        const char *k = algaops_config_key_name(i);
        if (i) fprintf(m, ", ");
        if (algaops_config_key_is_string(k) == 1) {
            fprintf(m, "\"%s\": \"%s\"", k, algaops_config_get_string(cfg, k));
        } else {
            double v = 0.0;
            algaops_config_get_number(cfg, k, &v);
            fprintf(m, "\"%s\": %.9g", k, v);
        }
    }
    fprintf(m, "},\n");
    fprintf(m, "  \"disclaimer\": \"Inspection warnings for technicians only; not a validated "
               "industrial diagnosis. Synthetic inputs demonstrate behaviour on designed "
               "scenarios, not field performance.\"\n}\n");
    fclose(m);
    return 0;
}

static int run_evaluation(const CliArgs *args)
{
    int extended = args->config_path != NULL;

    printf("AlgaOps Assurance v%s\n", ALGAOPS_VERSION_STR);
    if (args->baseline_path) printf("Baseline: %s\n", args->baseline_path);
    printf("Input:    %s\n", args->input_path);
    if (args->output_path) {
        printf("Output:   %s\n", args->output_path);
    }
    if (args->config_path) printf("Policy:   %s\n", args->config_path);
    print_separator();

    /* ------------------------------------------------------------------ */
    /* Configuration                                                        */
    /* ------------------------------------------------------------------ */
    AlgaEngineConfig cfg;
    algaops_engine_config_default(&cfg);
    if (args->config_path) {
        char err[512];
        if (algaops_config_load_file(&cfg, args->config_path, err, sizeof(err)) != ALGAOPS_OK) {
            fprintf(stderr, "Error: %s\n", err);
            return 1;
        }
    }

    AlgaEngine *engine = algaops_engine_create(&cfg);
    if (!engine) {
        fprintf(stderr, "Error: failed to initialise the diagnostic engine.\n");
        return 1;
    }

    /* ------------------------------------------------------------------ */
    /* Load baseline(s)                                                     */
    /* ------------------------------------------------------------------ */
    if (args->baseline_path) {
        AlgaBaseline bl[ALGA_MODE_COUNT];
        int n_valid = 0;
        AlgaopsStatus s = algaops_baselines_from_csv(&cfg, args->baseline_path, bl, &n_valid);
        if (s != ALGAOPS_OK) {
            fprintf(stderr, "Error: could not load baseline from '%s'.\n",
                    args->baseline_path);
            algaops_engine_destroy(engine);
            return 1;
        }
        for (int m = ALGA_MODE_OFF; m < ALGA_MODE_COUNT; m++) {
            if (!bl[m].valid) continue;
            algaops_engine_set_baseline(engine, &bl[m]);
            fprintf(stderr, "[baseline] '%s' mode %s: %u samples  vib=%.4f±%.4f g  ",
                    args->baseline_path, algaops_mode_str(m), (unsigned)bl[m].n,
                    bl[m].mean[ALGA_F_VIB_RMS], bl[m].std[ALGA_F_VIB_RMS]);
            if (bl[m].feature_mask & ALGA_FMASK(ALGA_F_TEMP)) {
                fprintf(stderr, "temp=%.2f±%.2f °C\n", bl[m].mean[ALGA_F_TEMP],
                        bl[m].std[ALGA_F_TEMP]);
            } else {
                fprintf(stderr, "temp=not a baseline feature (fixed limit only)\n");
            }
        }
        print_separator();
    }

    /* ------------------------------------------------------------------ */
    /* Open outputs                                                         */
    /* ------------------------------------------------------------------ */
    RunCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.args = args;
    ctx.extended = extended;
    audit_chain_init(&ctx.chain);
    ctx.line = (char *)malloc(LINE_CAP);
    ctx.escaped = (char *)malloc(2 * LINE_CAP + 8);
    int rc = 0;
    if (!ctx.line || !ctx.escaped) { rc = 1; goto done; }

    if (args->output_path) {
        ctx.out_csv = fopen(args->output_path, "w");
        if (!ctx.out_csv) {
            fprintf(stderr, "Error: cannot open output file '%s'.\n", args->output_path);
            rc = 1;
            goto done;
        }
        write_csv_header(ctx.out_csv);
    }
    if (args->json_path) {
        ctx.out_json = fopen(args->json_path, "wb");
        if (!ctx.out_json) {
            fprintf(stderr, "Error: cannot open JSON output '%s'.\n", args->json_path);
            rc = 1;
            goto done;
        }
    }
    if (args->evidence_dir) {
        MKDIR(args->evidence_dir);
        char p[1024];
        path_join(p, sizeof(p), args->evidence_dir, "decisions.jsonl");
        ctx.ev_decisions = fopen(p, "wb");
        path_join(p, sizeof(p), args->evidence_dir, "audit_events.jsonl");
        ctx.ev_events = fopen(p, "wb");
        if (!ctx.ev_decisions || !ctx.ev_events) {
            fprintf(stderr, "Error: cannot write evidence to '%s'.\n", args->evidence_dir);
            rc = 1;
            goto done;
        }
    }

    /* ------------------------------------------------------------------ */
    /* Process input records                                                */
    /* ------------------------------------------------------------------ */
    printf("Processing input: %s\n\n", args->input_path);
    int rows = 0;
    if (algaops_replay_csv(engine, args->input_path, on_decision, &ctx, &rows) != ALGAOPS_OK) {
        fprintf(stderr, "Error: cannot open input file '%s'.\n", args->input_path);
        rc = 1;
        goto done;
    }

    /* ------------------------------------------------------------------ */
    /* Summary                                                              */
    /* ------------------------------------------------------------------ */
    print_separator();
    printf("Summary:\n");
    printf("  Records processed : %d\n",   ctx.total);
    printf("  Parse errors      : %d\n",   ctx.parse_err);
    printf("  NORMAL            : %d\n",   ctx.normal);
    printf("  CHECK_SENSOR      : %d\n",   ctx.check_sensor);
    printf("  CHECK_EQUIPMENT   : %d\n",   ctx.check_equip);
    if (extended || ctx.not_eval) {
        printf("  NOT_EVALUATED     : %d\n", ctx.not_eval);
    }
    if (args->output_path) {
        printf("  Results written to: %s\n", args->output_path);
    }
    print_separator();

    if (ctx.total == 0 && ctx.parse_err == 0) {
        fprintf(stderr, "Warning: no records found in '%s'.\n", args->input_path);
        rc = 2;
    }

done:
    if (ctx.out_csv) fclose(ctx.out_csv);
    if (ctx.out_json) fclose(ctx.out_json);
    if (ctx.ev_decisions) fclose(ctx.ev_decisions);
    if (ctx.ev_events) fclose(ctx.ev_events);
    if (rc == 0 && ctx.io_error) {
        fprintf(stderr, "Error: failed to serialise a decision.\n");
        rc = 1;
    }
    if (rc == 0 && args->evidence_dir) {
        if (write_manifest(args, &cfg, &ctx) != 0) {
            fprintf(stderr, "Error: cannot write evidence manifest.\n");
            rc = 1;
        } else {
            char head[65];
            ascon_hash256_hex(ctx.chain.head, head);
            printf("Evidence: %s (%llu chained events, head %s)\n", args->evidence_dir,
                   (unsigned long long)ctx.chain.seq, head);
        }
    }
    free(ctx.line);
    free(ctx.escaped);
    algaops_engine_destroy(engine);
    return rc;
}

/* =========================================================================
 * Evidence verification
 * ========================================================================= */

static int verify_evidence(const char *dir)
{
    char p[1024];
    int failures = 0;

    /* 1. File digests. */
    path_join(p, sizeof(p), dir, "SHA-ASCON256SUMS");
    FILE *s = fopen(p, "rb");
    if (!s) {
        fprintf(stderr, "FAIL: cannot open %s\n", p);
        return 3;
    }
    char *line = NULL;
    size_t cap = 0;
    while (read_line(s, &line, &cap) >= 0) {
        if (!line[0]) continue;
        char hex[65], name[512];
        if (sscanf(line, "%64s  %511[^\n]", hex, name) != 2) {
            printf("FAIL  digest list: malformed line\n");
            failures++;
            continue;
        }
        char fp[1024];
        path_join(fp, sizeof(fp), dir, name);
        uint8_t h[32];
        char hx[65];
        if (hash_file(fp, h) != 0) {
            printf("FAIL  %s: missing or unreadable\n", name);
            failures++;
            continue;
        }
        ascon_hash256_hex(h, hx);
        if (strcmp(hx, hex) != 0) {
            printf("FAIL  %s: digest mismatch (file modified)\n", name);
            failures++;
        } else {
            printf("OK    %s\n", name);
        }
    }
    fclose(s);

    /* 2. Hash chain. */
    path_join(p, sizeof(p), dir, "audit_events.jsonl");
    FILE *e = fopen(p, "rb");
    if (!e) {
        fprintf(stderr, "FAIL: cannot open %s\n", p);
        free(line);
        return 3;
    }
    AuditChain chain;
    audit_chain_init(&chain);
    char *payload = NULL;
    size_t payload_cap = 0;
    long lineno = 0;
    int chain_ok = 1;
    while (read_line(e, &line, &cap) >= 0) {
        lineno++;
        if (!line[0]) continue;
        size_t need = strlen(line) + 1;
        if (need > payload_cap) {
            char *np = (char *)realloc(payload, need);
            if (!np) { chain_ok = 0; break; }
            payload = np;
            payload_cap = need;
        }
        uint64_t seq = 0;
        char prev[80], hash[80];
        long plen = audit_json_get_string(line, "payload", payload, payload_cap);
        if (audit_json_get_u64(line, "seq", &seq) != 0 || plen < 0 ||
            audit_json_get_string(line, "prev_hash", prev, sizeof(prev)) != 64 ||
            audit_json_get_string(line, "hash", hash, sizeof(hash)) != 64) {
            printf("FAIL  audit_events.jsonl line %ld: %s\n", lineno,
                   audit_verify_code_str(AUDIT_VERIFY_BAD_ENCODING));
            chain_ok = 0;
            break;
        }
        AuditVerifyCode code = audit_chain_verify_step(&chain, seq, prev, hash,
                                                       (const uint8_t *)payload, (size_t)plen);
        if (code != AUDIT_VERIFY_OK) {
            printf("FAIL  audit_events.jsonl line %ld (seq %llu): %s\n", lineno,
                   (unsigned long long)seq, audit_verify_code_str(code));
            chain_ok = 0;
            break;
        }
    }
    fclose(e);
    free(payload);
    free(line);
    if (chain_ok) {
        char head[65];
        ascon_hash256_hex(chain.head, head);
        printf("OK    hash chain: %llu events, head %s\n", (unsigned long long)chain.seq, head);
    } else {
        failures++;
    }
    printf("%s\n", failures ? "EVIDENCE VERIFICATION FAILED" : "EVIDENCE VERIFIED");
    return failures ? 3 : 0;
}

/* =========================================================================
 * Main
 * ========================================================================= */

int main(int argc, char **argv)
{
    CliArgs args;
    if (parse_args(argc, argv, &args) != 0) {
        print_usage(argv[0]);
        return 1;
    }

    if (args.hash_path) {
        uint8_t h[32];
        char hx[65];
        if (hash_file(args.hash_path, h) != 0) {
            fprintf(stderr, "Error: cannot read '%s'.\n", args.hash_path);
            return 1;
        }
        ascon_hash256_hex(h, hx);
        printf("%s  %s\n", hx, args.hash_path);
        return 0;
    }
    if (args.verify_dir) return verify_evidence(args.verify_dir);
    return run_evaluation(&args);
}
