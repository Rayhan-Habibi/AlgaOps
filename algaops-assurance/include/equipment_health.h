/**
 * @file equipment_health.h
 * @brief Public interface for the Trust-Gated Equipment Health evaluator.
 *
 * This module compares trusted sensor measurements against a pre-computed
 * baseline (mean ± std-dev) and emits one of three diagnostic outcomes:
 *   DIAG_NORMAL         — trusted data, no significant deviation detected.
 *   DIAG_CHECK_SENSOR   — untrusted data; equipment conclusions are suppressed.
 *   DIAG_CHECK_EQUIPMENT — trusted data with a significant baseline deviation.
 *
 * IMPORTANT: These are inspection *warnings*, not definitive fault diagnoses.
 * The baseline thresholds are preliminary engineering parameters.
 *
 * Usage pattern:
 *   1. Call equipment_health_init() with a config and a pre-allocated state.
 *   2. Call equipment_health_load_baseline() with a CSV file of normal-
 *      operation recordings.  Check the return code before proceeding.
 *   3. For each trusted/untrusted record, call equipment_health_evaluate().
 *      Pass the TrustResult from TrustGuard; the function enforces Trust-Gating.
 */

#ifndef EQUIPMENT_HEALTH_H
#define EQUIPMENT_HEALTH_H

#include "algaops_types.h"
#include "trustguard.h"

/* =========================================================================
 * Baseline statistics (computed once from the baseline CSV)
 * ========================================================================= */

/**
 * @brief Statistics computed from a representative normal-operation dataset.
 *
 * DISCLAIMER: These baselines are computed from synthetic or recorded data
 * under controlled conditions.  They are NOT validated industrial thresholds.
 * Re-calibrate using actual pump/blower recordings before operational use.
 */
typedef struct {
    double vib_mean_g;      /**< Mean vibration RMS in g.           */
    double vib_std_g;       /**< Std-dev of vibration RMS in g.     */
    double temp_mean_c;     /**< Mean surface temperature in °C.    */
    double temp_std_c;      /**< Std-dev of surface temperature.    */
    int    sample_count;    /**< Number of eligible samples used.   */
    bool   is_valid;        /**< True when baseline is usable.      */
} Baseline;

/**
 * @brief State for the Equipment Health evaluator.
 */
typedef struct {
    EquipmentHealthConfig config;
    Baseline              baseline;
} EquipmentHealthState;

/* =========================================================================
 * API
 * ========================================================================= */

/**
 * @brief Initialise an EquipmentHealthState with the given configuration.
 *
 * @param state   Non-NULL pointer to caller-owned state storage.
 * @param config  Non-NULL pointer to configuration.  Copied into state.
 * @return ALGAOPS_OK, or ALGAOPS_ERR_NULL_PTR if a pointer is NULL.
 */
AlgaopsStatus equipment_health_init(EquipmentHealthState        *state,
                                     const EquipmentHealthConfig *config);

/**
 * @brief Load and validate a baseline from a CSV file.
 *
 * Only records that pass a basic sanity check (finite, non-negative vibration)
 * are included in the baseline statistics.  At least config.min_baseline_samples
 * eligible records must be present.
 *
 * If the baseline CSV contains records that would fail TrustGuard checks, those
 * records are silently skipped (they are not eligible samples).  A warning
 * is printed to stderr when samples are skipped.
 *
 * @param state     Non-NULL pointer to an initialised state.
 * @param csv_path  Path to the CSV file.  Must not be NULL.
 * @param tg_config TrustGuard config to apply when screening baseline samples.
 *                  Pass NULL to skip TrustGuard screening during baseline load.
 * @return ALGAOPS_OK on success.
 *         ALGAOPS_ERR_IO if the file cannot be opened.
 *         ALGAOPS_ERR_PARSE if the CSV cannot be read.
 *         ALGAOPS_ERR_BASELINE if fewer than min_baseline_samples are eligible.
 */
AlgaopsStatus equipment_health_load_baseline(EquipmentHealthState   *state,
                                              const char             *csv_path,
                                              const TrustGuardConfig *tg_config);

/**
 * @brief Evaluate equipment health for one record using Trust-Gated logic.
 *
 * Trust-gating rule:
 *   - If trust_result->is_trusted is false, the function returns
 *     DIAG_CHECK_SENSOR without computing any z-scores.
 *   - Only trusted records may produce DIAG_CHECK_EQUIPMENT.
 *
 * @param state        Non-NULL pointer to an initialised, baseline-loaded state.
 * @param record       Non-NULL pointer to the measurement record.
 * @param trust_result Non-NULL pointer to the TrustGuard result for this record.
 * @param result       Non-NULL pointer to a HealthResult that receives output.
 * @return ALGAOPS_OK on success, or ALGAOPS_ERR_NULL_PTR / ALGAOPS_ERR_BASELINE.
 */
AlgaopsStatus equipment_health_evaluate(EquipmentHealthState *state,
                                         const AlgaRecord     *record,
                                         const TrustResult    *trust_result,
                                         HealthResult         *result);

/**
 * @brief Return a constant string label for a DiagnosticOutcome value.
 *        ("NORMAL", "CHECK_SENSOR", "CHECK_EQUIPMENT", "NOT_EVALUATED").
 */
ALGAOPS_API const char *diagnostic_outcome_str(DiagnosticOutcome outcome);

#endif /* EQUIPMENT_HEALTH_H */
