/**
 * @file cross_sensor.h
 * @brief Cross-sensor consistency: current vs vibration running state.
 *
 * When the optional current channel is present, the running state implied by
 * current and the one implied by vibration must agree.  Motor current is the
 * physical driver: after a current transition (switch-on, switch-off) the
 * vibration may lag for up to grace_ms (spin-up, coast-down).  Therefore:
 *
 *  - disagreement within grace_ms of the last current transition => PENDING:
 *    the window is a running-state transient, equipment health is withheld
 *    (the engine reports NOT_EVALUATED, never NORMAL or CHECK_EQUIPMENT);
 *  - disagreement outside that grace (e.g. vibration collapses while the
 *    current stays steady: detached accelerometer; or current drops while
 *    vibration continues: CT clamp off) => INTEGRITY_CROSS_SENSOR, the window
 *    is untrusted and the outcome is CHECK_SENSOR.
 *
 * Known limitation (stated in the reason text): a locked rotor or broken
 * coupling can also produce disagreement; the technician is asked to verify
 * both sensors before inspecting the pump.  Without a current channel this
 * check cannot run.
 */

#ifndef CROSS_SENSOR_H
#define CROSS_SENSOR_H

#include "algaops_types.h"
#include "operating_mode.h"

#include <stddef.h>

typedef struct {
    bool     enabled;
    uint32_t grace_ms;
} CrossSensorConfig;

static inline CrossSensorConfig cross_sensor_default_config(void)
{
    CrossSensorConfig c;
    c.enabled  = false;
    c.grace_ms = 10000;
    return c;
}

typedef enum {
    CROSS_OK      = 0,   /**< agreement, or check not applicable      */
    CROSS_PENDING = 1,   /**< transient disagreement within grace      */
    CROSS_FLAG    = 2    /**< persistent / unexplained disagreement    */
} CrossSensorVerdict;

typedef struct {
    bool     initialized;
    bool     vib_running;
    bool     cur_running;
    uint64_t cur_changed_ms;      /**< time of the last current-state change */
    bool     disagreeing;
    uint64_t disagree_since_ms;
} CrossSensorState;

void cross_sensor_init(CrossSensorState *s);

/**
 * @brief Update running states and test agreement for one finite window.
 * @return CROSS_OK, CROSS_PENDING or CROSS_FLAG; for PENDING and FLAG a
 *         human-readable explanation is written to @p expl.
 */
CrossSensorVerdict cross_sensor_check(CrossSensorState *s,
                                      const CrossSensorConfig *cfg,
                                      const OperatingModeConfig *mode_cfg,
                                      const AlgaWindow *w,
                                      char *expl, size_t expl_len);

#endif /* CROSS_SENSOR_H */
