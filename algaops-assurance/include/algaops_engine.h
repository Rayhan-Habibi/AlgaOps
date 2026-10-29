/**
 * @file algaops_engine.h
 * @brief Stateful per-node diagnostic engine: the single source of truth for
 *        every AlgaOps decision (CLI, gateway via ctypes, golden tests).
 *
 * Pipeline per telemetry window:
 *   TrustGuard (record checks) -> window policy (completeness) ->
 *   cross-sensor consistency -> trust decision ->
 *     untrusted: CHECK_SENSOR, health methods skipped
 *     trusted:   operating mode -> health methods (fixed / z-score /
 *                Mahalanobis as shadow results) -> primary method of the mode
 *                decides NORMAL / CHECK_EQUIPMENT, or NOT_EVALUATED when the
 *                primary method has no valid baseline
 *   -> alert state (open / clear / change with confirmation counts).
 *
 * algaops_engine_tick() turns stream silence into a CHECK_SENSOR decision.
 * The engine is read-only with respect to the plant: it never produces
 * commands, only decisions for human inspection.
 */

#ifndef ALGAOPS_ENGINE_H
#define ALGAOPS_ENGINE_H

#include "algaops_types.h"
#include "algaops_export.h"
#include "trustguard.h"
#include "window_policy.h"
#include "operating_mode.h"
#include "cross_sensor.h"
#include "baseline.h"
#include "health_methods.h"

#include <stddef.h>

/** ABI version of the structs below (bump on any layout change). */
#define ALGAOPS_ABI_VERSION 2u

typedef struct {
    TrustGuardConfig    trust;
    WindowPolicyConfig  window;
    OperatingModeConfig mode;
    CrossSensorConfig   cross;
    HealthMethodsConfig health;
    int32_t  primary_method[ALGA_MODE_COUNT]; /**< AlgaHealthMethod or -1 */
    uint32_t min_baseline_samples;
    uint32_t alert_confirm_windows;
    uint32_t alert_clear_windows;
} AlgaEngineConfig;

/* Decision kinds */
#define ALGA_DECISION_WINDOW         0
#define ALGA_DECISION_STREAM_TIMEOUT 1
#define ALGA_DECISION_MALFORMED      2

/* Alert events */
#define ALGA_ALERT_NONE    0
#define ALGA_ALERT_OPENED  1
#define ALGA_ALERT_CLEARED 2
#define ALGA_ALERT_CHANGED 3   /**< open alert replaced by an alert of another kind */

typedef struct {
    int32_t  kind;                 /**< ALGA_DECISION_*                        */
    uint32_t boot_id;
    uint32_t sequence_id;
    uint64_t timestamp_ms;
    uint64_t received_ms;
    int32_t  trust_score;
    int32_t  is_trusted;
    uint32_t flags;                /**< IntegrityFlags                         */
    int32_t  outcome;              /**< DiagnosticOutcome                      */
    int32_t  mode;                 /**< AlgaMode (UNKNOWN when untrusted)      */
    int32_t  mode_changed;
    uint64_t time_in_mode_ms;
    int32_t  primary_method;       /**< AlgaHealthMethod or -1                 */
    int32_t  decided_by;           /**< method that set the outcome, or -1     */
    uint32_t baseline_id;          /**< baseline used by the primary method    */
    uint32_t feature_mask;         /**< features present in the window         */
    double   features[ALGA_MAX_FEATURES];
    AlgaMethodResult methods[ALGA_HM_COUNT];
    int32_t  alert_event;          /**< ALGA_ALERT_*                           */
    int32_t  alert_kind;           /**< outcome of the open/cleared alert, -1  */
    int32_t  alert_prev_kind;      /**< for ALGA_ALERT_CHANGED, else -1        */
    uint32_t consecutive;          /**< consecutive windows with this outcome  */
    uint32_t missing_count;        /**< packets missing before this window     */
    double   completeness;         /**< recent stream completeness, -1 if n/a  */
    char     explanation[ALGAOPS_EXPLANATION_LEN]; /**< TrustGuard reasons    */
    char     reason[ALGAOPS_EXPLANATION_LEN];      /**< decision reason       */
} AlgaDecision;

typedef struct {
    uint32_t abi_version;
    uint32_t size_window;
    uint32_t size_decision;
    uint32_t size_config;
    uint32_t size_baseline;
    uint32_t size_method_result;
    uint32_t off_window_temperature;
    uint32_t off_window_band;
    uint32_t off_decision_outcome;
    uint32_t off_decision_methods;
    uint32_t off_decision_alert_event;
    uint32_t off_decision_completeness;
    uint32_t off_decision_explanation;
    uint32_t off_decision_reason;
    uint32_t off_baseline_cov;
} AlgaAbiInfo;

typedef struct AlgaEngine AlgaEngine;

/* ---- library info -------------------------------------------------------- */
ALGAOPS_API const char *algaops_version(void);
ALGAOPS_API void algaops_abi_info(AlgaAbiInfo *out);

/* ---- configuration ------------------------------------------------------- */

/** Defaults: every v2 feature disabled => exact v1 behaviour (legacy profile). */
ALGAOPS_API void algaops_engine_config_default(AlgaEngineConfig *cfg);
ALGAOPS_API size_t algaops_engine_config_size(void);

/**
 * @brief Set a numeric/boolean policy key, e.g. "trust.vib_max_g".
 * @return ALGAOPS_OK, ALGAOPS_ERR_CONFIG (unknown key / wrong type) or
 *         ALGAOPS_ERR_RANGE (value not representable for the key).
 */
ALGAOPS_API AlgaopsStatus algaops_config_set_number(AlgaEngineConfig *cfg,
                                                    const char *key, double value);
/** Set a string key (method.off / method.startup / method.steady). */
ALGAOPS_API AlgaopsStatus algaops_config_set_string(AlgaEngineConfig *cfg,
                                                    const char *key, const char *value);
ALGAOPS_API AlgaopsStatus algaops_config_get_number(const AlgaEngineConfig *cfg,
                                                    const char *key, double *value);
/** Returns a constant string value for string keys, NULL otherwise. */
ALGAOPS_API const char *algaops_config_get_string(const AlgaEngineConfig *cfg,
                                                  const char *key);
ALGAOPS_API int algaops_config_key_count(void);
ALGAOPS_API const char *algaops_config_key_name(int index);
/** 1 for string keys, 0 for numeric/boolean keys, -1 for unknown. */
ALGAOPS_API int algaops_config_key_is_string(const char *key);
ALGAOPS_API AlgaopsStatus algaops_config_validate(const AlgaEngineConfig *cfg,
                                                  char *err, size_t err_len);

/**
 * @brief Load a policy file (flat TOML subset: [section] headers, key = value
 *        with numbers, true/false and "strings", # comments) into @p cfg.
 */
ALGAOPS_API AlgaopsStatus algaops_config_load_file(AlgaEngineConfig *cfg, const char *path,
                                       char *err, size_t err_len);

/* ---- engine lifecycle ---------------------------------------------------- */
ALGAOPS_API AlgaEngine *algaops_engine_create(const AlgaEngineConfig *cfg);
ALGAOPS_API void algaops_engine_destroy(AlgaEngine *engine);
/** Start a new stream (forget sequence/history/alerts); baselines are kept. */
ALGAOPS_API AlgaopsStatus algaops_engine_reset(AlgaEngine *engine);

ALGAOPS_API AlgaopsStatus algaops_engine_set_baseline(AlgaEngine *engine,
                                                      const AlgaBaseline *baseline);
ALGAOPS_API AlgaopsStatus algaops_engine_clear_baseline(AlgaEngine *engine, int mode);
ALGAOPS_API AlgaopsStatus algaops_engine_get_baseline(const AlgaEngine *engine, int mode,
                                                      AlgaBaseline *out);

/* ---- decisions ----------------------------------------------------------- */
ALGAOPS_API AlgaopsStatus algaops_engine_process(AlgaEngine *engine,
                                                 const AlgaWindow *window,
                                                 AlgaDecision *decision);

/** Stream liveness; *emitted = 1 when a STREAM_TIMEOUT decision was produced. */
ALGAOPS_API AlgaopsStatus algaops_engine_tick(AlgaEngine *engine, uint64_t now_ms,
                                              AlgaDecision *decision, int *emitted);

/** A message for this node could not be parsed: CHECK_SENSOR / MALFORMED. */
ALGAOPS_API AlgaopsStatus algaops_engine_report_malformed(AlgaEngine *engine,
                                                          uint64_t received_ms,
                                                          const char *detail,
                                                          AlgaDecision *decision);

/** Warm start: restore the persisted alert state (the open alert's outcome,
 *  DIAG_CHECK_SENSOR or DIAG_CHECK_EQUIPMENT, or -1 when none is open).  Window
 *  replay cannot reproduce alerts opened by stream timeouts or malformed records. */
ALGAOPS_API AlgaopsStatus algaops_engine_restore_alert(AlgaEngine *engine, int32_t open_kind);

/** Label helpers. */
ALGAOPS_API const char *algaops_mode_str(int mode);
ALGAOPS_API const char *algaops_method_str(int method);
ALGAOPS_API const char *algaops_alert_event_str(int event);

/* ---- CSV helpers (CLI, golden tests) ------------------------------------- */

typedef void (*AlgaDecisionCallback)(void *user, int row, const AlgaWindow *window,
                                     const AlgaDecision *decision, int parse_error);

/** Run every row of a v1/v2 CSV file through the engine. */
ALGAOPS_API AlgaopsStatus algaops_replay_csv(AlgaEngine *engine, const char *path,
                                 AlgaDecisionCallback cb, void *user, int *rows);

/**
 * @brief Build per-mode baselines from a CSV recording with a fresh engine
 *        (same TrustGuard screening and mode classification as live data).
 * @param out    array of ALGA_MODE_COUNT baselines (index = mode).
 * @param valid  number of valid baselines produced.
 */
ALGAOPS_API AlgaopsStatus algaops_baselines_from_csv(const AlgaEngineConfig *cfg, const char *path,
                                         AlgaBaseline out[ALGA_MODE_COUNT], int *valid);

#endif /* ALGAOPS_ENGINE_H */
