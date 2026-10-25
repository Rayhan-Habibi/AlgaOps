/**
 * @file algaops_types.h
 * @brief Shared types, configuration structures, and constants for AlgaOps Assurance.
 *
 * All units are documented inline. Thresholds tagged [CONFIGURABLE] are
 * preliminary engineering parameters — not validated industrial diagnostic
 * thresholds — and are designed to be easy to locate and adjust.
 *
 * This file has no external dependencies and is safe for both desktop and
 * potential future ESP32/edge-gateway reuse.
 */

#ifndef ALGAOPS_TYPES_H
#define ALGAOPS_TYPES_H

#include <stdint.h>
#include <stdbool.h>

/* =========================================================================
 * Versioning
 * ========================================================================= */
#define ALGAOPS_VERSION_MAJOR 1
#define ALGAOPS_VERSION_MINOR 0
#define ALGAOPS_VERSION_PATCH 0
#define ALGAOPS_VERSION_STR   "1.0.0"

/* =========================================================================
 * Physical measurement record
 * ========================================================================= */

/**
 * @brief One validated measurement window from the sensor system.
 *
 * Fields:
 *  - timestamp_ms   Monotonically increasing wall-clock time in milliseconds.
 *  - sequence_id    Monotonically increasing packet counter (no gaps allowed).
 *  - vibration_rms_g  RMS vibration magnitude in g (gravitational units).
 *  - temperature_c  Surface temperature in degrees Celsius.
 */
typedef struct {
    uint64_t timestamp_ms;
    uint32_t sequence_id;
    double   vibration_rms_g;
    double   temperature_c;
} AlgaRecord;

/* =========================================================================
 * TrustGuard integrity flags (bit-mask)
 * ========================================================================= */

/** No integrity issue detected. */
#define INTEGRITY_OK                 (0u)

/** One or more sequence IDs were skipped before this packet. */
#define INTEGRITY_MISSING_PACKET     (1u << 0)

/** Sequence ID is identical to the previous accepted record. */
#define INTEGRITY_DUPLICATE_SEQ      (1u << 1)

/** Sequence ID is lower than the previous accepted record (out-of-order). */
#define INTEGRITY_OUT_OF_ORDER_SEQ   (1u << 2)

/** Timestamp is identical to or earlier than the previous timestamp. */
#define INTEGRITY_BAD_TIMESTAMP      (1u << 3)

/** vibration_rms_g is outside [vib_min_g, vib_max_g]. */
#define INTEGRITY_VIB_OUT_OF_RANGE   (1u << 4)

/** temperature_c is outside [temp_min_c, temp_max_c]. */
#define INTEGRITY_TEMP_OUT_OF_RANGE  (1u << 5)

/** Vibration appears stuck (near-identical values over rolling window). */
#define INTEGRITY_VIB_STUCK          (1u << 6)

/** Temperature appears stuck (near-identical values over rolling window). */
#define INTEGRITY_TEMP_STUCK         (1u << 7)

/** Non-finite (NaN/Inf) or otherwise unrepresentable numeric value. */
#define INTEGRITY_NON_FINITE         (1u << 8)

/** Record could not be parsed from the input source. */
#define INTEGRITY_MALFORMED          (1u << 9)

typedef uint32_t IntegrityFlags;

/* =========================================================================
 * Diagnostic outcomes (Feature B)
 * ========================================================================= */

typedef enum {
    /**
     * Data is trusted and no configured equipment anomaly is detected.
     */
    DIAG_NORMAL = 0,

    /**
     * Data integrity is questionable; equipment-health evaluation is blocked.
     * The sensor or communication path needs inspection first.
     */
    DIAG_CHECK_SENSOR,

    /**
     * Data is trusted but one or more equipment-health indicators deviate
     * significantly from the established baseline.
     * This is an inspection warning, not a definitive fault diagnosis.
     */
    DIAG_CHECK_EQUIPMENT
} DiagnosticOutcome;

/* =========================================================================
 * TrustGuard configuration
 * ========================================================================= */

/**
 * @brief Configuration for the TrustGuard validator module.
 *
 * All fields tagged [CONFIGURABLE] are preliminary parameters.
 * Modify them to suit the physical characteristics of the monitored pump/blower.
 */
typedef struct {
    /* --- Range limits [CONFIGURABLE] --- */

    /** Minimum plausible vibration RMS in g. Default: 0.0 g */
    double vib_min_g;

    /** Maximum plausible vibration RMS in g. Default: 50.0 g */
    double vib_max_g;

    /** Minimum plausible surface temperature in °C. Default: -10.0 °C */
    double temp_min_c;

    /** Maximum plausible surface temperature in °C. Default: 120.0 °C */
    double temp_max_c;

    /* --- Stuck-sensor detection [CONFIGURABLE] --- */

    /**
     * Rolling window size (number of recent samples) used for stuck detection.
     * A larger window reduces false positives on naturally stable equipment.
     * Default: 5 samples.
     */
    int stuck_window_size;

    /**
     * Maximum absolute change in vibration (g) across the window before the
     * sensor is considered stuck. Default: 0.005 g.
     * NOTE: Natural stable operation may also produce small variation.
     * Tune this threshold using normal-operation baseline recordings.
     */
    double stuck_vib_threshold_g;

    /**
     * Maximum absolute change in temperature (°C) across the window before
     * the sensor is considered stuck. Default: 0.05 °C.
     */
    double stuck_temp_threshold_c;

    /**
     * Minimum trust score (0–100) required to mark a record as trusted.
     * Records below this score are untrusted regardless of individual flags.
     * The trust score is informational; blocking flags always override it.
     * Default: 60.
     */
    int trust_score_threshold;

} TrustGuardConfig;

/**
 * @brief Returns a TrustGuardConfig populated with documented defaults.
 */
static inline TrustGuardConfig trustguard_default_config(void) {
    TrustGuardConfig c;
    c.vib_min_g              = 0.0;
    c.vib_max_g              = 50.0;
    c.temp_min_c             = -10.0;
    c.temp_max_c             = 120.0;
    c.stuck_window_size      = 5;
    c.stuck_vib_threshold_g  = 0.001;
    c.stuck_temp_threshold_c = 0.01;
    c.trust_score_threshold  = 60;
    return c;
}

/* =========================================================================
 * Equipment Health configuration
 * ========================================================================= */

/**
 * @brief Configuration for the Equipment Health evaluator module.
 */
typedef struct {
    /**
     * Number of standard deviations from the baseline mean that constitutes
     * a significant deviation (z-score threshold). [CONFIGURABLE]
     * Default: 3.0  (≈ 0.3 % false-positive rate for a normal distribution)
     *
     * DISCLAIMER: This is a preliminary engineering parameter.
     * It has not been validated against industrial pump-failure datasets.
     */
    double z_score_threshold;

    /**
     * Minimum number of eligible baseline samples required to compute a
     * usable baseline. Default: 10 samples.
     */
    int min_baseline_samples;

    /**
     * Minimum standard deviation (floor) used to avoid division by zero
     * when the baseline variance is near zero. [CONFIGURABLE]
     * Default: 1e-6 (applied independently to vibration and temperature).
     */
    double min_std_dev;

} EquipmentHealthConfig;

/**
 * @brief Returns an EquipmentHealthConfig populated with documented defaults.
 */
static inline EquipmentHealthConfig equipment_health_default_config(void) {
    EquipmentHealthConfig c;
    c.z_score_threshold   = 3.0;
    c.min_baseline_samples = 10;
    c.min_std_dev         = 1e-6;
    return c;
}

/* =========================================================================
 * TrustGuard result
 * ========================================================================= */

/** Maximum length of the human-readable explanation string. */
#define ALGAOPS_EXPLANATION_LEN 512

/**
 * @brief Result produced by TrustGuard for a single measurement record.
 */
typedef struct {
    /** Computed trust score in range [0, 100]. Informational only. */
    int trust_score;

    /** True if the record passes all blocking integrity checks and the trust
     *  score is at or above trust_score_threshold. */
    bool is_trusted;

    /** Bitmask of detected integrity issues. INTEGRITY_OK (0) means clean. */
    IntegrityFlags flags;

    /** Human-readable description of all detected issues. */
    char explanation[ALGAOPS_EXPLANATION_LEN];
} TrustResult;

/* =========================================================================
 * Equipment Health result
 * ========================================================================= */

/**
 * @brief Result produced by the Equipment Health evaluator.
 */
typedef struct {
    DiagnosticOutcome outcome;

    /** Z-score of vibration deviation (0.0 if not computed). */
    double vib_z_score;

    /** Z-score of temperature deviation (0.0 if not computed). */
    double temp_z_score;

    /** Human-readable classification reason. */
    char reason[ALGAOPS_EXPLANATION_LEN];
} HealthResult;

/* =========================================================================
 * Return codes
 * ========================================================================= */

typedef enum {
    ALGAOPS_OK            =  0,
    ALGAOPS_ERR_GENERIC   = -1,
    ALGAOPS_ERR_NULL_PTR  = -2,
    ALGAOPS_ERR_IO        = -3,
    ALGAOPS_ERR_PARSE     = -4,
    ALGAOPS_ERR_BASELINE  = -5,
    ALGAOPS_ERR_NOMEM     = -6,
} AlgaopsStatus;

#endif /* ALGAOPS_TYPES_H */
