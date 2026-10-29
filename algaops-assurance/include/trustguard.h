/**
 * @file trustguard.h
 * @brief Public interface for the TrustGuard sensor-data integrity validator.
 *
 * TrustGuard validates incoming sensor measurement records and produces a
 * trust score plus integrity flags for each record.  Its output gates whether
 * the Equipment Health evaluator may emit equipment-anomaly conclusions.
 *
 * Usage pattern:
 *   1. Call trustguard_init() once, providing a configuration and the state
 *      object.  Call trustguard_reset() to start a new data stream.
 *   2. For each incoming AlgaRecord, call trustguard_validate().
 *      (v2: for each AlgaWindow, call trustguard_validate_window().)
 *   3. Check TrustResult.is_trusted before running equipment-health checks.
 *
 * Both entry points share one implementation: a v1 AlgaRecord is converted to
 * an AlgaWindow with no optional fields present, so v2-only checks never run
 * for v1 input.
 *
 * Threading: TrustGuardState is not thread-safe.  Use one state per thread or
 * protect external access with a mutex.
 */

#ifndef TRUSTGUARD_H
#define TRUSTGUARD_H

#include "algaops_types.h"
#include "algaops_export.h"

#include <stddef.h>

/* =========================================================================
 * Internal rolling window (private implementation detail exposed here so
 * that TrustGuardState can be stack-allocated without a heap allocation).
 * ========================================================================= */

/** Maximum supported stuck-sensor window size (per channel). */
#define TRUSTGUARD_MAX_WINDOW 1024

/** Number of recent boot identities remembered for session ordering. */
#define TRUSTGUARD_BOOT_HISTORY 8

/**
 * @brief Opaque state for the TrustGuard validator.
 *
 * Allocate on the stack or embed in a larger struct.  Initialise with
 * trustguard_init() before first use.  Never copy a live state object.
 */
typedef struct {
    /* Configuration snapshot (copied at init time). */
    TrustGuardConfig config;

    /* State of the previous accepted record. */
    bool             has_previous;
    uint64_t         prev_timestamp_ms;
    uint32_t         prev_sequence_id;

    /* Rolling window for stuck-sensor detection (vibration channel). */
    int              window_count;      /* how many samples are in window[]  */
    int              window_head;       /* circular-buffer write index        */
    double           vib_window[TRUSTGUARD_MAX_WINDOW];

    /* Rolling window for the temperature channel (own size since v2). */
    int              temp_count;
    int              temp_head;
    double           temp_window[TRUSTGUARD_MAX_WINDOW];

    /* v2: boot/session tracking. */
    uint32_t         prev_boot_id;
    uint32_t         boot_history[TRUSTGUARD_BOOT_HISTORY];
    int              boot_history_len;

    /* v2: previous finite temperature for the rate-of-change check. */
    bool             has_prev_temp;
    double           prev_temp_c;
    uint64_t         prev_temp_ts_ms;

    /* v2: diagnostics of the last validate call (read by the engine). */
    uint32_t         last_missing_count;
    bool             last_reboot;
    bool             last_forward;      /* record advanced the sequence */
} TrustGuardState;

/* =========================================================================
 * API
 * ========================================================================= */

/**
 * @brief Initialise a TrustGuardState with the given configuration.
 *
 * @param state   Non-NULL pointer to caller-owned state storage.
 * @param config  Non-NULL pointer to configuration.  Copied into state.
 * @return ALGAOPS_OK on success, ALGAOPS_ERR_NULL_PTR if either pointer is NULL,
 *         or ALGAOPS_ERR_GENERIC if the window size is invalid.
 */
AlgaopsStatus trustguard_init(TrustGuardState        *state,
                               const TrustGuardConfig *config);

/**
 * @brief Reset validator state for a new data stream.
 *
 * Clears rolling-window history and forgets previous sequence/timestamp.
 * Configuration is preserved.  Call this when starting to read a new CSV file.
 *
 * @param state  Non-NULL pointer to an initialised TrustGuardState.
 * @return ALGAOPS_OK, or ALGAOPS_ERR_NULL_PTR if state is NULL.
 */
AlgaopsStatus trustguard_reset(TrustGuardState *state);

/**
 * @brief Clear only the value history (stuck windows, previous temperature).
 *
 * Sequence, timestamp and boot tracking are preserved.  Used by the engine
 * after a large sequence gap so that non-contiguous data is never combined.
 */
AlgaopsStatus trustguard_reset_history(TrustGuardState *state);

/**
 * @brief Validate a single measurement record and produce a TrustResult.
 *
 * @param state   Non-NULL pointer to an initialised TrustGuardState.
 * @param record  Non-NULL pointer to the record to evaluate.
 * @param result  Non-NULL pointer to a TrustResult that receives the output.
 * @return ALGAOPS_OK on success, or an error code on API misuse.
 *
 * Notes:
 *  - Records with NON_FINITE, DUPLICATE_SEQ or OUT_OF_ORDER_SEQ flags are not
 *    stored in the rolling stuck-detection window; all other records are.
 *  - Missing packets are detected when the NEXT valid packet arrives; the
 *    gap count is reflected in the result for that next packet.
 *  - The trust score is informational.  A score at or above
 *    config.trust_score_threshold AND no blocking flags → is_trusted = true.
 */
AlgaopsStatus trustguard_validate(TrustGuardState  *state,
                                   const AlgaRecord *record,
                                   TrustResult      *result);

/**
 * @brief v2: validate one telemetry window (superset of trustguard_validate).
 *
 * Additional checks run only when the corresponding presence bit is set or
 * the corresponding config parameter is non-zero: boot/session identity,
 * freshness and clock skew, clock synchronisation, window completeness,
 * internal consistency, self-test result, node-reported faults, current range,
 * temperature rate of change and zero peak-to-peak (frozen accelerometer).
 */
AlgaopsStatus trustguard_validate_window(TrustGuardState  *state,
                                          const AlgaWindow *window,
                                          TrustResult      *result);

/**
 * @brief Add a flag to a result: sets the bit, deducts its score penalty and
 *        appends a formatted explanation.  Used by the engine for flags that
 *        are computed outside TrustGuard (window policy, cross-sensor).
 */
void trustguard_add_flag(TrustResult *result, IntegrityFlags flag,
                         const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

/**
 * @brief Recompute is_trusted from flags and score (clamps the score and
 *        writes "OK" when no flag is set).
 */
void trustguard_finalize(const TrustGuardConfig *config, TrustResult *result);

/** Score penalty applied for a single flag bit. */
int trustguard_flag_penalty(IntegrityFlags flag);

/** Mask of flags that always force is_trusted = false. */
ALGAOPS_API IntegrityFlags trustguard_blocking_flags(void);

/** Short constant name for one flag bit (e.g. "VIB_STUCK"), or "UNKNOWN". */
ALGAOPS_API const char *trustguard_flag_name(IntegrityFlags flag);

/**
 * @brief Convert an IntegrityFlags bitmask to a human-readable string.
 *
 * @param flags  Bitmask to describe.
 * @param buf    Output buffer.
 * @param buflen Size of output buffer in bytes.
 */
ALGAOPS_API void trustguard_flags_to_str(IntegrityFlags flags, char *buf, int buflen);

/** Describe node-reported fault bits (ALGA_NODE_FAULT_*), '|' separated. */
ALGAOPS_API void trustguard_node_faults_to_str(uint32_t faults, char *buf, size_t buflen);

#endif /* TRUSTGUARD_H */
