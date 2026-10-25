/*
 * main.c — AlgaOps Assurance CLI entry point.
 *
 * Usage:
 *   algaops --baseline <baseline.csv> --input <input.csv> [--output <results.csv>]
 *
 * For each input record the program prints:
 *   - Timestamp, sequence ID
 *   - Trust score, trust decision, integrity flags, explanation
 *   - Diagnostic outcome and reason
 *
 * An optional CSV output file contains the same data in machine-readable form.
 *
 * Exit codes:
 *   0  All records processed (some may be untrusted/anomalous).
 *   1  Invalid arguments, missing files, or baseline load failure.
 *   2  No eligible input records were processed.
 */

#include "../include/algaops_types.h"
#include "../include/trustguard.h"
#include "../include/equipment_health.h"
#include "csv_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* =========================================================================
 * CLI argument parsing
 * ========================================================================= */

typedef struct {
    const char *baseline_path;
    const char *input_path;
    const char *output_path;   /* NULL if not specified */
} CliArgs;

static void print_usage(const char *prog)
{
    fprintf(stderr,
            "AlgaOps Assurance v%s\n"
            "Usage:\n"
            "  %s --baseline <baseline.csv> --input <input.csv> "
            "[--output <results.csv>]\n"
            "\n"
            "Options:\n"
            "  --baseline  CSV file with representative normal-operation data.\n"
            "  --input     CSV file with sensor measurements to evaluate.\n"
            "  --output    (Optional) Write evaluation results to this CSV file.\n",
            ALGAOPS_VERSION_STR, prog);
}

static int parse_args(int argc, char **argv, CliArgs *args)
{
    memset(args, 0, sizeof(*args));

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--baseline") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --baseline requires a file path.\n");
                return 1;
            }
            args->baseline_path = argv[i];
        } else if (strcmp(argv[i], "--input") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --input requires a file path.\n");
                return 1;
            }
            args->input_path = argv[i];
        } else if (strcmp(argv[i], "--output") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --output requires a file path.\n");
                return 1;
            }
            args->output_path = argv[i];
        } else if (strcmp(argv[i], "--help") == 0 ||
                   strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            exit(0);
        } else {
            fprintf(stderr, "Error: unknown option '%s'\n", argv[i]);
            return 1;
        }
    }

    if (!args->baseline_path) {
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
 * Output formatting
 * ========================================================================= */

static void print_separator(void)
{
    printf("%-70s\n", "----------------------------------------------------------------------");
}

static void print_record_result(const AlgaRecord   *rec,
                                const TrustResult  *tr,
                                const HealthResult *hr)
{
    char flags_str[256];
    trustguard_flags_to_str(tr->flags, flags_str, sizeof(flags_str));

    printf("  ts=%-8llu  seq=%-5u  vib=%.4fg  temp=%.2f°C\n",
           (unsigned long long)rec->timestamp_ms,
           rec->sequence_id,
           rec->vibration_rms_g,
           rec->temperature_c);
    printf("  TrustScore: %-3d  Trusted: %-5s  Flags: %s\n",
           tr->trust_score,
           tr->is_trusted ? "YES" : "NO",
           flags_str);
    printf("  Integrity:  %s\n", tr->explanation);
    printf("  Diagnostic: %-16s  Reason: %s\n",
           diagnostic_outcome_str(hr->outcome),
           hr->reason);
    if (hr->outcome == DIAG_CHECK_EQUIPMENT) {
        printf("  Z-scores:   vib=%.2f  temp=%.2f\n",
               hr->vib_z_score, hr->temp_z_score);
    }
}

/* =========================================================================
 * CSV output
 * ========================================================================= */

static void write_csv_header(FILE *f)
{
    fprintf(f,
            "timestamp_ms,sequence_id,vibration_rms_g,temperature_c,"
            "trust_score,is_trusted,integrity_flags,"
            "diagnostic_outcome,vib_z_score,temp_z_score,reason\n");
}

static void write_csv_row(FILE *f,
                          const AlgaRecord   *rec,
                          const TrustResult  *tr,
                          const HealthResult *hr)
{
    char flags_str[256];
    trustguard_flags_to_str(tr->flags, flags_str, sizeof(flags_str));

    /* Escape reason field: replace commas and newlines. */
    char reason_escaped[ALGAOPS_EXPLANATION_LEN * 2];
    const char *src = hr->reason;
    char *dst = reason_escaped;
    while (*src && (dst - reason_escaped) < (int)sizeof(reason_escaped) - 2) {
        if (*src == '"') { *dst++ = '"'; *dst++ = '"'; }
        else              { *dst++ = *src; }
        src++;
    }
    *dst = '\0';

    fprintf(f,
            "%llu,%u,%.6f,%.4f,%d,%s,%s,%s,%.4f,%.4f,\"%s\"\n",
            (unsigned long long)rec->timestamp_ms,
            rec->sequence_id,
            rec->vibration_rms_g,
            rec->temperature_c,
            tr->trust_score,
            tr->is_trusted ? "true" : "false",
            flags_str,
            diagnostic_outcome_str(hr->outcome),
            hr->vib_z_score,
            hr->temp_z_score,
            reason_escaped);
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

    printf("AlgaOps Assurance v%s\n", ALGAOPS_VERSION_STR);
    printf("Baseline: %s\n", args.baseline_path);
    printf("Input:    %s\n", args.input_path);
    if (args.output_path) {
        printf("Output:   %s\n", args.output_path);
    }
    print_separator();

    /* ------------------------------------------------------------------ */
    /* Initialise modules                                                   */
    /* ------------------------------------------------------------------ */
    TrustGuardConfig   tg_cfg = trustguard_default_config();
    TrustGuardState    tg_state;
    if (trustguard_init(&tg_state, &tg_cfg) != ALGAOPS_OK) {
        fprintf(stderr, "Error: failed to initialise TrustGuard.\n");
        return 1;
    }

    EquipmentHealthConfig eh_cfg = equipment_health_default_config();
    EquipmentHealthState  eh_state;
    if (equipment_health_init(&eh_state, &eh_cfg) != ALGAOPS_OK) {
        fprintf(stderr, "Error: failed to initialise EquipmentHealth.\n");
        return 1;
    }

    /* ------------------------------------------------------------------ */
    /* Load baseline                                                        */
    /* ------------------------------------------------------------------ */
    AlgaopsStatus s = equipment_health_load_baseline(&eh_state,
                                                      args.baseline_path,
                                                      &tg_cfg);
    if (s != ALGAOPS_OK) {
        fprintf(stderr, "Error: could not load baseline from '%s'.\n",
                args.baseline_path);
        return 1;
    }
    print_separator();

    /* ------------------------------------------------------------------ */
    /* Open optional CSV output                                             */
    /* ------------------------------------------------------------------ */
    FILE *out_csv = NULL;
    if (args.output_path) {
        out_csv = fopen(args.output_path, "w");
        if (!out_csv) {
            fprintf(stderr, "Error: cannot open output file '%s'.\n",
                    args.output_path);
            return 1;
        }
        write_csv_header(out_csv);
    }

    /* ------------------------------------------------------------------ */
    /* Process input records                                                */
    /* ------------------------------------------------------------------ */
    CsvReader reader;
    if (csv_reader_open(&reader, args.input_path) != ALGAOPS_OK) {
        fprintf(stderr, "Error: cannot open input file '%s'.\n",
                args.input_path);
        if (out_csv) fclose(out_csv);
        return 1;
    }

    trustguard_reset(&tg_state);   /* fresh state for this input stream */

    int total = 0, normal = 0, check_sensor = 0, check_equip = 0, parse_err = 0;
    AlgaRecord  rec;
    TrustResult tr;
    HealthResult hr;

    printf("Processing input: %s\n\n", args.input_path);

    while (true) {
        AlgaopsStatus rs = csv_reader_next(&reader, &rec);
        if (rs == ALGAOPS_ERR_IO) break;   /* EOF */
        if (rs == ALGAOPS_ERR_PARSE) {
            parse_err++;
            /* Create a synthetic untrusted result so the row can be logged. */
            memset(&tr, 0, sizeof(tr));
            tr.trust_score  = 0;
            tr.is_trusted   = false;
            tr.flags        = INTEGRITY_MALFORMED;
            strncpy(tr.explanation, "malformed CSV record",
                    ALGAOPS_EXPLANATION_LEN - 1);
            memset(&hr, 0, sizeof(hr));
            hr.outcome = DIAG_CHECK_SENSOR;
            strncpy(hr.reason,
                    "CHECK_SENSOR: record could not be parsed",
                    ALGAOPS_EXPLANATION_LEN - 1);
            /* Use a zero record for the malformed entry. */
            memset(&rec, 0, sizeof(rec));
        } else {
            total++;
            if (trustguard_validate(&tg_state, &rec, &tr) != ALGAOPS_OK) {
                fprintf(stderr, "Warning: TrustGuard error on record %d\n",
                        total);
                continue;
            }
            if (equipment_health_evaluate(&eh_state, &rec, &tr, &hr) != ALGAOPS_OK) {
                fprintf(stderr, "Warning: EquipmentHealth error on record %d\n",
                        total);
                continue;
            }
        }

        /* Tally outcomes. */
        switch (hr.outcome) {
            case DIAG_NORMAL:          normal++;       break;
            case DIAG_CHECK_SENSOR:    check_sensor++; break;
            case DIAG_CHECK_EQUIPMENT: check_equip++;  break;
        }

        print_record_result(&rec, &tr, &hr);
        printf("\n");

        if (out_csv) {
            write_csv_row(out_csv, &rec, &tr, &hr);
        }
    }

    csv_reader_close(&reader);
    if (out_csv) fclose(out_csv);

    /* ------------------------------------------------------------------ */
    /* Summary                                                              */
    /* ------------------------------------------------------------------ */
    print_separator();
    printf("Summary:\n");
    printf("  Records processed : %d\n",   total);
    printf("  Parse errors      : %d\n",   parse_err);
    printf("  NORMAL            : %d\n",   normal);
    printf("  CHECK_SENSOR      : %d\n",   check_sensor);
    printf("  CHECK_EQUIPMENT   : %d\n",   check_equip);
    if (args.output_path) {
        printf("  Results written to: %s\n", args.output_path);
    }
    print_separator();

    if (total == 0 && parse_err == 0) {
        fprintf(stderr, "Warning: no records found in '%s'.\n",
                args.input_path);
        return 2;
    }

    return 0;
}
