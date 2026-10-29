/*
 * telemetry_payload.h — window records and their JSON form.
 *
 * Output follows schemas/telemetry_window.v1.schema.json and
 * schemas/node_status.v1.schema.json exactly (the gateway rejects unknown
 * fields).  Invalid measurements are written as null, never as a number.
 *
 * Window times are kept on the monotonic clock (esp_timer) and converted at
 * publish time: once SNTP has set the clock, windows buffered before the sync
 * still get correct Unix times.  Without a sync the value is the time since
 * boot and "time_synced": false makes the gateway flag the window.
 */

#ifndef TELEMETRY_PAYLOAD_H
#define TELEMETRY_PAYLOAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sensor_hal.h"

#define TELEMETRY_SCHEMA "algaops.telemetry_window.v1"
#define STATUS_SCHEMA    "algaops.node_status.v1"
#define NODE_ID_MAX_LEN  64
#define TELEMETRY_PAYLOAD_MAX 1024

typedef struct {
    uint32_t i2c_errors;
    uint32_t ow_crc_errors;
    uint32_t fifo_overruns;
    uint32_t queue_depth;
    uint32_t dropped_windows;
    uint32_t uptime_s;
    uint32_t reconnects;
    int16_t  rssi_dbm;
    bool     rssi_valid;
} telemetry_health_t;

typedef struct {
    uint32_t boot_id;
    uint32_t seq;
    int64_t  t_end_mono_us;        /* monotonic time of the window end            */
    uint32_t window_ms;
    float    sample_rate_hz;
    uint32_t samples_expected;
    uint32_t samples_valid;
    bool     vib_valid;
    float    vib_rms_g;
    float    vib_rms_axis_g[3];
    float    vib_p2p_g[3];
    bool     temp_valid;
    float    temp_c;
    bool     current_present;      /* a CT is configured: field is written        */
    bool     current_valid;
    float    current_a;
    uint8_t  self_test;            /* node_self_test_t                            */
    uint32_t faults;               /* NODE_FAULT_*                                */
    telemetry_health_t health;
} telemetry_record_t;

typedef struct {
    bool    synced;                /* SNTP has set the clock at least once        */
    int64_t unix_minus_mono_us;    /* Unix time = monotonic time + this offset    */
} time_base_t;

/** Unix ms of a monotonic instant, or ms since boot when not synced. */
int64_t time_base_to_ms(const time_base_t *tb, int64_t mono_us);

/** Pattern of the contract: ^[A-Za-z0-9][A-Za-z0-9_.-]{0,63}$ */
bool node_id_valid(const char *id);

/** Serialise a window; returns the length or -1 (invalid input / buffer too small). */
int telemetry_payload_build(char *buf, size_t len, const char *node_id, const char *fw,
                            const telemetry_record_t *r, const time_base_t *tb);

typedef struct {
    const char      *state;         /* "online" | "offline"                       */
    bool             has_boot;
    uint32_t         boot_id;
    const char      *fw;            /* NULL: omitted                              */
    bool             has_runtime;   /* uptime, queue and sensor fields present    */
    uint32_t         uptime_s;
    bool             rssi_valid;
    int16_t          rssi_dbm;
    uint32_t         queue_depth;
    uint32_t         dropped_windows;
    node_self_test_t self_test;
    bool             accel_ok;
    bool             temp_ok;
    bool             current_present;
    bool             current_ok;
    const char      *reset_reason;  /* NULL: omitted                              */
    int64_t          ts_ms;         /* < 0: omitted                               */
} node_status_t;

/** Serialise a status message (retained; also used as the MQTT last will). */
int status_payload_build(char *buf, size_t len, const char *node_id, const node_status_t *s);

#endif /* TELEMETRY_PAYLOAD_H */
