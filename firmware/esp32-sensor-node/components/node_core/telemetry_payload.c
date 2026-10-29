/* telemetry_payload.c — see telemetry_payload.h */

#include "telemetry_payload.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "sensor_health.h"

/* ---- bounded JSON writer ---------------------------------------------------- */

typedef struct {
    char  *buf;
    size_t len;
    size_t pos;
    bool   overflow;
} jbuf_t;

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 2, 3)))
#endif
static void jb_printf(jbuf_t *j, const char *fmt, ...)
{
    if (j->overflow) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(j->buf + j->pos, j->len - j->pos, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= j->len - j->pos) {
        j->overflow = true;
        return;
    }
    j->pos += (size_t)n;
}

static void jb_num(jbuf_t *j, double v, int decimals)
{
    if (!isfinite(v)) jb_printf(j, "null");
    else              jb_printf(j, "%.*f", decimals, v);
}

static void jb_triple(jbuf_t *j, const float v[3], int decimals)
{
    jb_printf(j, "[");
    for (int i = 0; i < 3; i++) {
        if (i) jb_printf(j, ",");
        jb_num(j, v[i], decimals);
    }
    jb_printf(j, "]");
}

/* Only short ASCII identifiers are written as strings; anything that would need
 * escaping is rejected instead. */
static bool plain_ascii(const char *s, size_t max_len)
{
    if (!s) return false;
    size_t n = 0;
    for (; s[n]; n++) {
        unsigned char c = (unsigned char)s[n];
        if (c < 0x20 || c > 0x7E || c == '"' || c == '\\' || n >= max_len) return false;
    }
    return n > 0;
}

/* ---- helpers ------------------------------------------------------------------ */

int64_t time_base_to_ms(const time_base_t *tb, int64_t mono_us)
{
    int64_t us = mono_us;
    if (tb && tb->synced) us += tb->unix_minus_mono_us;
    return us < 0 ? 0 : us / 1000;
}

bool node_id_valid(const char *id)
{
    if (!id || !id[0]) return false;
    size_t n = strlen(id);
    if (n > NODE_ID_MAX_LEN) return false;
    for (size_t i = 0; i < n; i++) {
        char c = id[i];
        bool alnum = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (!alnum && (i == 0 || (c != '_' && c != '.' && c != '-'))) return false;
    }
    return true;
}

/* ---- telemetry ---------------------------------------------------------------- */

int telemetry_payload_build(char *buf, size_t len, const char *node_id, const char *fw,
                            const telemetry_record_t *r, const time_base_t *tb)
{
    if (!buf || len == 0 || !r || !node_id_valid(node_id) || !plain_ascii(fw, 32) ||
        r->window_ms == 0 || !(r->sample_rate_hz > 0.0f)) {
        return -1;
    }
    jbuf_t j = { buf, len, 0, false };
    int64_t t_end = time_base_to_ms(tb, r->t_end_mono_us);
    int64_t t_start = t_end >= (int64_t)r->window_ms ? t_end - (int64_t)r->window_ms : 0;

    jb_printf(&j, "{\"schema\":\"" TELEMETRY_SCHEMA "\",\"node_id\":\"%s\",\"boot_id\":%lu,"
              "\"seq\":%lu,\"t_start_ms\":%lld,\"t_end_ms\":%lld,\"window_ms\":%lu,"
              "\"time_synced\":%s,\"sample_rate_hz\":",
              node_id, (unsigned long)r->boot_id, (unsigned long)r->seq, (long long)t_start,
              (long long)t_end, (unsigned long)r->window_ms,
              (tb && tb->synced) ? "true" : "false");
    jb_num(&j, r->sample_rate_hz, 1);
    jb_printf(&j, ",\"samples_expected\":%lu,\"samples_valid\":%lu,\"vib\":{\"rms_g\":",
              (unsigned long)r->samples_expected, (unsigned long)r->samples_valid);
    if (r->vib_valid) {
        jb_num(&j, r->vib_rms_g, 5);
        jb_printf(&j, ",\"rms_axis_g\":");
        jb_triple(&j, r->vib_rms_axis_g, 5);
        jb_printf(&j, ",\"p2p_g\":");
        jb_triple(&j, r->vib_p2p_g, 5);
    } else {
        jb_printf(&j, "null");
    }
    jb_printf(&j, "},\"temp_c\":");
    if (r->temp_valid) jb_num(&j, r->temp_c, 4);
    else               jb_printf(&j, "null");
    if (r->current_present) {
        jb_printf(&j, ",\"current_a\":");
        if (r->current_valid) jb_num(&j, r->current_a, 3);
        else                  jb_printf(&j, "null");
    }
    jb_printf(&j, ",\"self_test\":\"%s\",\"faults\":[",
              node_self_test_str((node_self_test_t)r->self_test));
    const char *names[NODE_FAULT_COUNT];
    int nf = sensor_health_fault_names(r->faults, names, NODE_FAULT_COUNT);
    for (int i = 0; i < nf; i++) jb_printf(&j, "%s\"%s\"", i ? "," : "", names[i]);
    const telemetry_health_t *h = &r->health;
    jb_printf(&j, "],\"fw\":\"%s\",\"source\":\"hardware\",\"health\":{\"i2c_errors\":%lu,"
              "\"ow_crc_errors\":%lu,\"fifo_overruns\":%lu,\"queue_depth\":%lu,"
              "\"dropped_windows\":%lu,\"uptime_s\":%lu,\"reconnects\":%lu",
              fw, (unsigned long)h->i2c_errors, (unsigned long)h->ow_crc_errors,
              (unsigned long)h->fifo_overruns, (unsigned long)h->queue_depth,
              (unsigned long)h->dropped_windows, (unsigned long)h->uptime_s,
              (unsigned long)h->reconnects);
    if (h->rssi_valid && h->rssi_dbm <= 0 && h->rssi_dbm >= -200) {
        jb_printf(&j, ",\"rssi_dbm\":%d", (int)h->rssi_dbm);
    }
    jb_printf(&j, "}}");
    return j.overflow ? -1 : (int)j.pos;
}

/* ---- status --------------------------------------------------------------------- */

int status_payload_build(char *buf, size_t len, const char *node_id, const node_status_t *s)
{
    if (!buf || len == 0 || !s || !node_id_valid(node_id) || !s->state ||
        (strcmp(s->state, "online") != 0 && strcmp(s->state, "offline") != 0)) {
        return -1;
    }
    if ((s->fw && !plain_ascii(s->fw, 64)) || (s->reset_reason && !plain_ascii(s->reset_reason, 64))) {
        return -1;
    }
    jbuf_t j = { buf, len, 0, false };
    jb_printf(&j, "{\"schema\":\"" STATUS_SCHEMA "\",\"node_id\":\"%s\",\"state\":\"%s\","
              "\"source\":\"hardware\"", node_id, s->state);
    if (s->has_boot) jb_printf(&j, ",\"boot_id\":%lu", (unsigned long)s->boot_id);
    if (s->fw) jb_printf(&j, ",\"fw\":\"%s\"", s->fw);
    if (s->has_runtime) {
        jb_printf(&j, ",\"uptime_s\":%lu,\"queue_depth\":%lu,\"dropped_windows\":%lu,"
                  "\"self_test\":\"%s\",\"sensors\":{\"accelerometer\":%s,\"temperature\":%s",
                  (unsigned long)s->uptime_s, (unsigned long)s->queue_depth,
                  (unsigned long)s->dropped_windows, node_self_test_str(s->self_test),
                  s->accel_ok ? "true" : "false", s->temp_ok ? "true" : "false");
        if (s->current_present) jb_printf(&j, ",\"current\":%s", s->current_ok ? "true" : "false");
        jb_printf(&j, "}");
        if (s->rssi_valid && s->rssi_dbm <= 0 && s->rssi_dbm >= -200) {
            jb_printf(&j, ",\"rssi_dbm\":%d", (int)s->rssi_dbm);
        }
    }
    if (s->reset_reason) jb_printf(&j, ",\"reset_reason\":\"%s\"", s->reset_reason);
    if (s->ts_ms >= 0) jb_printf(&j, ",\"ts_ms\":%lld", (long long)s->ts_ms);
    jb_printf(&j, "}");
    return j.overflow ? -1 : (int)j.pos;
}
