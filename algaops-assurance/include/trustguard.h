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
 *   3. Check TrustResult.is_trusted before running equipment-health checks.
 *
 * Threading: TrustGuardState is not thread-safe.  Use one state per thread or
 * protect external access with a mutex.
 */

#ifndef TRUSTGUARD_H
#define TRUSTGUARD_H

#include "algaops_types.h"

/* =========================================================================
 * Internal rolling window (private implementation detail exposed here so
 * that TrustGuardState can be stack-allocated without a heap allocation).
 * ========================================================================= */

/** Maximum supported stuck-sensor window size. */
#define TRUSTGUARD_MAX_WINDOW 64

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

    /* Rolling window for stuck-sensor detection. */
    int              window_count;      /* how many samples are in window[]  */
    int              window_head;       /* circular-buffer write index        */
    double           vib_window[TRUSTGUARD_MAX_WINDOW];
    double           temp_window[TRUSTGUARD_MAX_WINDOW];
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
 * @brief Validate a single measurement record and produce a TrustResult.
 *
 * @param state   Non-NULL pointer to an initialised TrustGuardState.
 * @param record  Non-NULL pointer to the record to evaluate.
 * @param result  Non-NULL pointer to a TrustResult that receives the output.
 * @return ALGAOPS_OK on success, or an error code on API misuse.
 *
 * Notes:
 *  - A blocking integrity flag (any flag except INTEGRITY_MISSING_PACKET
 *    detected for the *previous* gap) prevents the record from being stored
 *    in the rolling window and sets is_trusted = false regardless of score.
 *  - Missing packets are detected when the NEXT valid packet arrives; the
 *    gap count is reflected in the result for that next packet.
 *  - The trust score is informational.  A score at or above
 *    config.trust_score_threshold AND no blocking flags → is_trusted = true.
 */
AlgaopsStatus trustguard_validate(TrustGuardState  *state,
                                   const AlgaRecord *record,
                                   TrustResult      *result);

/**
 * @brief Convert an IntegrityFlags bitmask to a human-readable string.
 *
 * @param flags  Bitmask to describe.
 * @param buf    Output buffer.
 * @param buflen Size of output buffer in bytes.
 */
void trustguard_flags_to_str(IntegrityFlags flags, char *buf, int buflen);

#endif /* TRUSTGUARD_H */
