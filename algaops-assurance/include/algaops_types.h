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
 *
 * Version 2 additions (engine path) are appended after the original v1
 * definitions; every v1 type, flag value and default is unchanged so that the
 * original API and its test-suite keep their behaviour.
 */

#ifndef ALGAOPS_TYPES_H
#define ALGAOPS_TYPES_H

#include <stdint.h>
#include <stdbool.h>

/* =========================================================================
 * Versioning
 * ========================================================================= */
#define ALGAOPS_VERSION_MAJOR 2
#define ALGAOPS_VERSION_MINOR 0
#define ALGAOPS_VERSION_PATCH 0
#define ALGAOPS_VERSION_STR   "2.0.0"

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

/* ---- v2 flags (engine / telemetry window path) ------------------------- */

/** Window delivered later than max_staleness_ms after its capture time. */
#define INTEGRITY_STALE              (1u << 10)

/** Window timestamp lies further in the future than max_future_skew_ms. */
#define INTEGRITY_FUTURE_TIMESTAMP   (1u << 11)

/** A new boot_id was observed (node reboot). Informational, not blocking. */
#define INTEGRITY_REBOOT             (1u << 12)

/** samples_valid / samples_expected below min_window_completeness. */
#define INTEGRITY_INCOMPLETE_WINDOW  (1u << 13)

/** Accelerometer self-test reported FAIL. */
#define INTEGRITY_SELF_TEST_FAIL     (1u << 14)

/** Node firmware reported a sensor/bus fault during the window. */
#define INTEGRITY_SENSOR_FAULT       (1u << 15)

/** Current and vibration disagree about the running state. */
#define INTEGRITY_CROSS_SENSOR       (1u << 16)

/** Node clock was not synchronised when the window was captured. */
#define INTEGRITY_CLOCK_UNSYNCED     (1u << 17)

/** Internally inconsistent window (e.g. p2p < rms, sample count mismatch). */
#define INTEGRITY_INCONSISTENT       (1u << 18)

/** Too many windows missing in the recent diagnostic window (stream level). */
#define INTEGRITY_LOW_COMPLETENESS   (1u << 19)

/** Physically implausible rate of change (temperature spike). */
#define INTEGRITY_SPIKE              (1u << 20)

/** No telemetry received within stream_timeout_ms (emitted by tick). */
#define INTEGRITY_STREAM_TIMEOUT     (1u << 21)

/** Number of defined flag bits. */
#define INTEGRITY_FLAG_COUNT         22

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
    DIAG_CHECK_EQUIPMENT,

    /**
     * v2 engine only: data is trusted but no configured normal criteria could
     * be evaluated (no valid baseline for the operating mode). This is never
     * reported as NORMAL. The legacy equipment_health API never returns it.
     */
    DIAG_NOT_EVALUATED
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
     * sensor is considered stuck. Default: 0.001 g.
     * NOTE: Natural stable operation may also produce small variation.
     * Tune this threshold using normal-operation baseline recordings.
     */
    double stuck_vib_threshold_g;

    /**
     * Maximum absolute change in temperature (°C) across the window before
     * the sensor is considered stuck. Default: 0.01 °C.
     */
    double stuck_temp_threshold_c;

    /**
     * Minimum trust score (0–100) required to mark a record as trusted.
     * Records below this score are untrusted regardless of individual flags.
     * The trust score is informational; blocking flags always override it.
     * Default: 60.
     */
    int trust_score_threshold;

    /* --- v2 additions (all disabled by default => v1 behaviour) --------- */

    /** Temperature stuck window (samples). 0 => use stuck_window_size. */
    int stuck_temp_window_size;

    /** Minimum samples_valid/samples_expected inside one window. 0 disables. */
    double min_window_completeness;

    /** Maximum receive delay (received_ms - timestamp_ms). 0 disables. */
    uint32_t max_staleness_ms;

    /** Maximum allowed timestamp lead over received_ms. 0 disables. */
    uint32_t max_future_skew_ms;

    /** Flag windows captured while the node clock was not synchronised. */
    bool require_time_sync;

    /** Maximum plausible |dT/dt| in °C/s between windows. 0 disables. */
    double max_temp_rate_c_per_s;

    /** Relative tolerance for internal-consistency checks. Default 0.05. */
    double consistency_rel_tol;

    /** Treat zero peak-to-peak on every axis within a window as stuck. */
    bool flag_zero_p2p_as_stuck;

    /** Plausible current range in A (only when current is present). */
    double current_min_a;
    double current_max_a;

    /** Allowed |(t_end - t_start) - window_ms| in ms. 0 disables. */
    uint32_t window_timing_tolerance_ms;

    /** Forward gap (windows) that restarts the value history. 0 disables. */
    uint32_t history_reset_gap;

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
    c.stuck_temp_window_size     = 0;
    c.min_window_completeness    = 0.0;
    c.max_staleness_ms           = 0;
    c.max_future_skew_ms         = 0;
    c.require_time_sync          = false;
    c.max_temp_rate_c_per_s      = 0.0;
    c.consistency_rel_tol        = 0.05;
    c.flag_zero_p2p_as_stuck     = true;
    c.current_min_a              = 0.0;
    c.current_max_a              = 100.0;
    c.window_timing_tolerance_ms = 0;
    c.history_reset_gap          = 0;
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
#define ALGAOPS_EXPLANATION_LEN 1024

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
    ALGAOPS_ERR_CONFIG    = -7,
    ALGAOPS_ERR_RANGE     = -8,
} AlgaopsStatus;

/* =========================================================================
 * v2: telemetry window (engine input)
 * ========================================================================= */

/** Presence bits for optional AlgaWindow fields. */
#define ALGA_HAS_RECEIVED      (1u << 0)  /**< received_ms is valid          */
#define ALGA_HAS_TIMING        (1u << 1)  /**< t_start_ms / window_ms valid  */
#define ALGA_HAS_SAMPLE_COUNTS (1u << 2)  /**< samples_expected/valid valid  */
#define ALGA_HAS_AXES          (1u << 3)  /**< vib_rms_axis_g valid          */
#define ALGA_HAS_P2P           (1u << 4)  /**< vib_p2p_axis_g valid          */
#define ALGA_HAS_CURRENT       (1u << 5)  /**< current_a valid               */
#define ALGA_HAS_SELF_TEST     (1u << 6)  /**< self_test valid               */
#define ALGA_HAS_TIME_SYNC     (1u << 7)  /**< time_synced valid             */
#define ALGA_HAS_NODE_FAULTS   (1u << 8)  /**< node_fault_flags valid        */
#define ALGA_HAS_BANDS         (1u << 9)  /**< band_rms_g valid              */

/** Self-test states reported by the node. */
#define ALGA_SELF_TEST_NOT_RUN 0
#define ALGA_SELF_TEST_PASS    1
#define ALGA_SELF_TEST_FAIL    2

/** Node-reported fault bits (firmware sensor_health.h mirrors these). */
#define ALGA_NODE_FAULT_ACCEL_BUS      (1u << 0)  /**< I2C error talking to ADXL345 */
#define ALGA_NODE_FAULT_ACCEL_OVERRUN  (1u << 1)  /**< FIFO overrun, samples lost   */
#define ALGA_NODE_FAULT_ACCEL_MISSING  (1u << 2)  /**< ADXL345 DEVID not detected    */
#define ALGA_NODE_FAULT_TEMP_CRC       (1u << 3)  /**< DS18B20 scratchpad CRC error  */
#define ALGA_NODE_FAULT_TEMP_MISSING   (1u << 4)  /**< no 1-Wire presence pulse      */
#define ALGA_NODE_FAULT_TEMP_POR       (1u << 5)  /**< 85 °C power-on-reset value    */
#define ALGA_NODE_FAULT_CURRENT_ADC    (1u << 6)  /**< current ADC read failure      */
#define ALGA_NODE_FAULT_CURRENT_CLIP   (1u << 7)  /**< current ADC saturated         */
#define ALGA_NODE_FAULT_COUNT          8

/** Maximum number of spectral bands carried by a window. */
#define ALGA_MAX_BANDS 4

/**
 * @brief One telemetry window (summary of N raw samples) from a sensor node.
 *
 * Mandatory fields: sequence_id, timestamp_ms, vibration_rms_g, temperature_c.
 * All other fields are optional and guarded by the presence mask; NAN marks
 * a missing measurement that should have been present (=> NON_FINITE).
 */
typedef struct {
    uint32_t boot_id;            /**< node boot identity (0 for v1 records)   */
    uint32_t sequence_id;        /**< per-boot window counter                 */
    uint64_t timestamp_ms;       /**< window end, node clock (epoch ms)       */
    uint64_t received_ms;        /**< gateway receive time (epoch ms)         */
    uint64_t t_start_ms;         /**< window start, node clock                */
    uint32_t window_ms;          /**< nominal window duration                 */
    uint32_t samples_expected;
    uint32_t samples_valid;
    uint32_t presence;           /**< ALGA_HAS_* bits                         */
    uint32_t node_fault_flags;   /**< ALGA_NODE_FAULT_* bits                  */
    int32_t  self_test;          /**< ALGA_SELF_TEST_*                        */
    int32_t  time_synced;        /**< 1 synced, 0 not synced                  */
    int32_t  n_bands;
    double   vibration_rms_g;    /**< AC vector magnitude RMS                 */
    double   vib_rms_axis_g[3];
    double   vib_p2p_axis_g[3];
    double   temperature_c;
    double   current_a;          /**< RMS current                             */
    double   band_rms_g[ALGA_MAX_BANDS];
} AlgaWindow;

/** Operating modes of the monitored pump/blower. */
typedef enum {
    ALGA_MODE_UNKNOWN = 0,
    ALGA_MODE_OFF     = 1,
    ALGA_MODE_STARTUP = 2,
    ALGA_MODE_STEADY  = 3
} AlgaMode;
#define ALGA_MODE_COUNT 4

/** Health feature indices (vector used by baselines and health methods). */
#define ALGA_F_VIB_RMS  0
#define ALGA_F_VIB_P2P  1
#define ALGA_F_TEMP     2
#define ALGA_F_CURRENT  3
#define ALGA_MAX_FEATURES 4
#define ALGA_FMASK(i)   (1u << (i))

/** Health methods. */
typedef enum {
    ALGA_HM_FIXED       = 0,
    ALGA_HM_ZSCORE      = 1,
    ALGA_HM_MAHALANOBIS = 2
} AlgaHealthMethod;
#define ALGA_HM_COUNT 3

/** Per-method evaluation status. */
#define ALGA_METHOD_SKIPPED     0   /**< not run: window untrusted           */
#define ALGA_METHOD_OK          1   /**< evaluated                           */
#define ALGA_METHOD_UNAVAILABLE 2   /**< no baseline / insufficient samples  */

#endif /* ALGAOPS_TYPES_H */
