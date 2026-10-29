/*
 * equipment_health.c — Trust-Gated Equipment Health evaluator.
 *
 * Algorithm summary:
 *   Baseline:  mean and std-dev of vibration_rms_g and temperature_c
 *              computed from an eligible subset of a representative CSV file.
 *
 *   Evaluation:
 *     If !trust_result->is_trusted  →  DIAG_CHECK_SENSOR  (blocked)
 *     Otherwise:
 *       z_vib  = |sample_vib  - baseline_vib_mean | / max(vib_std,  min_std)
 *       z_temp = |sample_temp - baseline_temp_mean| / max(temp_std, min_std)
 *       If z_vib > threshold OR z_temp > threshold  →  DIAG_CHECK_EQUIPMENT
 *       Else                                         →  DIAG_NORMAL
 *
 * Numerics:
 *   - std-dev floor (min_std_dev) prevents division by zero.
 *   - All intermediate results are checked for finiteness.
 *   - Baseline variance of zero is explicitly handled and reported.
 *
 * The statistics (baseline.c Welford builder) and the z formula
 * (health_methods.c health_z) are shared with the v2 engine, so the legacy
 * API and the engine cannot drift apart.
 *
 * DISCLAIMER: Thresholds are preliminary engineering parameters only.
 * They are NOT validated industrial diagnostic thresholds.
 */

#include "../include/equipment_health.h"
#include "../include/baseline.h"
#include "../include/health_methods.h"
#include "csv_reader.h"

#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* =========================================================================
 * Public API
 * ========================================================================= */

AlgaopsStatus equipment_health_init(EquipmentHealthState        *state,
                                     const EquipmentHealthConfig *config)
{
    if (!state || !config) return ALGAOPS_ERR_NULL_PTR;
    memset(state, 0, sizeof(*state));
    state->config = *config;
    state->baseline.is_valid = false;
    return ALGAOPS_OK;
}

AlgaopsStatus equipment_health_load_baseline(EquipmentHealthState   *state,
                                              const char             *csv_path,
                                              const TrustGuardConfig *tg_config)
{
    if (!state || !csv_path) return ALGAOPS_ERR_NULL_PTR;

    state->baseline.is_valid = false;

    CsvReader reader;
    AlgaopsStatus s = csv_reader_open(&reader, csv_path);
    if (s != ALGAOPS_OK) return s;

    /* Optionally screen baseline records with TrustGuard.  The state is
     * large (rolling windows), so keep it off the stack. */
    TrustGuardState *tg_state = NULL;
    if (tg_config != NULL) {
        tg_state = (TrustGuardState *)malloc(sizeof(*tg_state));
        if (tg_state && trustguard_init(tg_state, tg_config) != ALGAOPS_OK) {
            free(tg_state);
            tg_state = NULL;
        }
    }

    AlgaBaselineBuilder builder;
    alga_baseline_builder_init(&builder,
                               ALGA_FMASK(ALGA_F_VIB_RMS) | ALGA_FMASK(ALGA_F_TEMP));

    int skipped = 0;
    AlgaRecord rec;

    while (true) {
        AlgaopsStatus rs = csv_reader_next(&reader, &rec);
        if (rs == ALGAOPS_ERR_IO) break;   /* EOF */
        if (rs == ALGAOPS_ERR_PARSE) {
            skipped++;
            continue;
        }

        /* Basic sanity: finite, non-negative vibration. */
        if (!isfinite(rec.vibration_rms_g) ||
            !isfinite(rec.temperature_c)   ||
            rec.vibration_rms_g < 0.0) {
            fprintf(stderr,
                    "[equipment_health] baseline %s:%d: "
                    "non-finite or negative vibration — skipped\n",
                    csv_path, csv_reader_line(&reader));
            skipped++;
            continue;
        }

        /* Optional TrustGuard screening. */
        if (tg_state) {
            TrustResult tr;
            trustguard_validate(tg_state, &rec, &tr);
            if (!tr.is_trusted) {
                fprintf(stderr,
                        "[equipment_health] baseline %s:%d: "
                        "sample untrusted (%s) — skipped\n",
                        csv_path, csv_reader_line(&reader),
                        tr.explanation);
                skipped++;
                continue;
            }
        }

        double x[ALGA_MAX_FEATURES] = { 0.0, 0.0, 0.0, 0.0 };
        x[ALGA_F_VIB_RMS] = rec.vibration_rms_g;
        x[ALGA_F_TEMP]    = rec.temperature_c;
        alga_baseline_builder_add(&builder, x,
                                  ALGA_FMASK(ALGA_F_VIB_RMS) | ALGA_FMASK(ALGA_F_TEMP));
    }

    csv_reader_close(&reader);
    free(tg_state);

    if (skipped > 0) {
        fprintf(stderr,
                "[equipment_health] baseline '%s': %d sample(s) skipped\n",
                csv_path, skipped);
    }

    int n = (int)builder.n;
    if (n < state->config.min_baseline_samples) {
        fprintf(stderr,
                "[equipment_health] baseline '%s': only %d eligible sample(s), "
                "need at least %d — baseline rejected\n",
                csv_path, n, state->config.min_baseline_samples);
        return ALGAOPS_ERR_BASELINE;
    }

    AlgaBaseline bl;
    if (alga_baseline_builder_finish(&builder, ALGA_MODE_STEADY, 1u, &bl) != ALGAOPS_OK) {
        return ALGAOPS_ERR_BASELINE;
    }

    state->baseline.vib_mean_g   = bl.mean[ALGA_F_VIB_RMS];
    state->baseline.vib_std_g    = bl.std[ALGA_F_VIB_RMS];
    state->baseline.temp_mean_c  = bl.mean[ALGA_F_TEMP];
    state->baseline.temp_std_c   = bl.std[ALGA_F_TEMP];
    state->baseline.sample_count = n;
    state->baseline.is_valid     = true;

    /* Warn if variance is near zero (may indicate stuck sensor in baseline). */
    if (state->baseline.vib_std_g < state->config.min_std_dev) {
        fprintf(stderr,
                "[equipment_health] WARNING: baseline vibration std-dev "
                "(%.2e g) is near zero — applying floor %.2e g.  "
                "Check baseline dataset.\n",
                state->baseline.vib_std_g, state->config.min_std_dev);
    }
    if (state->baseline.temp_std_c < state->config.min_std_dev) {
        fprintf(stderr,
                "[equipment_health] WARNING: baseline temperature std-dev "
                "(%.2e °C) is near zero — applying floor %.2e °C.  "
                "Check baseline dataset.\n",
                state->baseline.temp_std_c, state->config.min_std_dev);
    }

    fprintf(stderr,
            "[equipment_health] baseline '%s': %d samples  "
            "vib=%.4f±%.4f g  temp=%.2f±%.2f °C\n",
            csv_path, n,
            state->baseline.vib_mean_g,  state->baseline.vib_std_g,
            state->baseline.temp_mean_c, state->baseline.temp_std_c);

    return ALGAOPS_OK;
}

AlgaopsStatus equipment_health_evaluate(EquipmentHealthState *state,
                                         const AlgaRecord     *record,
                                         const TrustResult    *trust_result,
                                         HealthResult         *result)
{
    if (!state || !record || !trust_result || !result) {
        return ALGAOPS_ERR_NULL_PTR;
    }
    if (!state->baseline.is_valid) {
        return ALGAOPS_ERR_BASELINE;
    }

    memset(result, 0, sizeof(*result));

    /* ------------------------------------------------------------------ */
    /* Trust-Gating: suppressed evaluation for untrusted data.             */
    /* ------------------------------------------------------------------ */
    if (!trust_result->is_trusted) {
        result->outcome    = DIAG_CHECK_SENSOR;
        result->vib_z_score  = 0.0;
        result->temp_z_score = 0.0;
        snprintf(result->reason, ALGAOPS_EXPLANATION_LEN,
                 "CHECK_SENSOR: data integrity blocked evaluation — %.900s",
                 trust_result->explanation);
        return ALGAOPS_OK;
    }

    /* ------------------------------------------------------------------ */
    /* Z-score computation.                                                */
    /* ------------------------------------------------------------------ */
    const EquipmentHealthConfig *cfg = &state->config;
    const Baseline              *bl  = &state->baseline;

    double z_vib  = health_z(record->vibration_rms_g, bl->vib_mean_g,
                             bl->vib_std_g, cfg->min_std_dev);
    double z_temp = health_z(record->temperature_c, bl->temp_mean_c,
                             bl->temp_std_c, cfg->min_std_dev);

    result->vib_z_score  = z_vib;
    result->temp_z_score = z_temp;

    /* ------------------------------------------------------------------ */
    /* Classification.                                                     */
    /* ------------------------------------------------------------------ */
    bool vib_anomaly  = (z_vib  > cfg->z_score_threshold);
    bool temp_anomaly = (z_temp > cfg->z_score_threshold);

    if (vib_anomaly || temp_anomaly) {
        result->outcome = DIAG_CHECK_EQUIPMENT;
        int pos = 0;
        pos += snprintf(result->reason + pos,
                        (size_t)(ALGAOPS_EXPLANATION_LEN - pos),
                        "CHECK_EQUIPMENT: ");
        if (vib_anomaly) {
            pos += snprintf(result->reason + pos,
                            (size_t)(ALGAOPS_EXPLANATION_LEN - pos),
                            "vibration z=%.2f > threshold %.2f "
                            "(%.4f g vs baseline %.4f±%.4f g)",
                            z_vib, cfg->z_score_threshold,
                            record->vibration_rms_g,
                            bl->vib_mean_g, bl->vib_std_g);
        }
        if (vib_anomaly && temp_anomaly) {
            pos += snprintf(result->reason + pos,
                            (size_t)(ALGAOPS_EXPLANATION_LEN - pos), "; ");
        }
        if (temp_anomaly) {
            pos += snprintf(result->reason + pos,
                            (size_t)(ALGAOPS_EXPLANATION_LEN - pos),
                            "temperature z=%.2f > threshold %.2f "
                            "(%.2f °C vs baseline %.2f±%.2f °C)",
                            z_temp, cfg->z_score_threshold,
                            record->temperature_c,
                            bl->temp_mean_c, bl->temp_std_c);
        }
    } else {
        result->outcome = DIAG_NORMAL;
        snprintf(result->reason, ALGAOPS_EXPLANATION_LEN,
                 "NORMAL: vib z=%.2f, temp z=%.2f (both <= threshold %.2f)",
                 z_vib, z_temp, cfg->z_score_threshold);
    }

    return ALGAOPS_OK;
}

const char *diagnostic_outcome_str(DiagnosticOutcome outcome)
{
    switch (outcome) {
        case DIAG_NORMAL:          return "NORMAL";
        case DIAG_CHECK_SENSOR:    return "CHECK_SENSOR";
        case DIAG_CHECK_EQUIPMENT: return "CHECK_EQUIPMENT";
        case DIAG_NOT_EVALUATED:   return "NOT_EVALUATED";
        default:                   return "UNKNOWN";
    }
}
