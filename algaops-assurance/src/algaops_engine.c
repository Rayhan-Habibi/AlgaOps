/*
 * algaops_engine.c — per-node diagnostic engine (see algaops_engine.h).
 */

#include "../include/algaops_engine.h"
#include "../include/equipment_health.h"
#include "csv_reader.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define F ALGA_MAX_FEATURES

struct AlgaEngine {
    AlgaEngineConfig   cfg;
    TrustGuardState    tg;
    WindowPolicyState  wp;
    OperatingModeState om;
    CrossSensorState   cs;
    AlgaBaseline       baselines[ALGA_MODE_COUNT];
    bool               has_last;
    int32_t            last_outcome;
    uint32_t           consecutive;
    bool               alert_open;
    int32_t            alert_kind;
    bool               has_identity;
    uint32_t           last_boot_id;
    uint32_t           last_seq;
};

/* =========================================================================
 * Library info
 * ========================================================================= */

const char *algaops_version(void) { return ALGAOPS_VERSION_STR; }

void algaops_abi_info(AlgaAbiInfo *o)
{
    if (!o) return;
    memset(o, 0, sizeof(*o));
    o->abi_version               = ALGAOPS_ABI_VERSION;
    o->size_window               = (uint32_t)sizeof(AlgaWindow);
    o->size_decision             = (uint32_t)sizeof(AlgaDecision);
    o->size_config               = (uint32_t)sizeof(AlgaEngineConfig);
    o->size_baseline             = (uint32_t)sizeof(AlgaBaseline);
    o->size_method_result        = (uint32_t)sizeof(AlgaMethodResult);
    o->off_window_temperature    = (uint32_t)offsetof(AlgaWindow, temperature_c);
    o->off_window_band           = (uint32_t)offsetof(AlgaWindow, band_rms_g);
    o->off_decision_outcome      = (uint32_t)offsetof(AlgaDecision, outcome);
    o->off_decision_methods      = (uint32_t)offsetof(AlgaDecision, methods);
    o->off_decision_alert_event  = (uint32_t)offsetof(AlgaDecision, alert_event);
    o->off_decision_completeness = (uint32_t)offsetof(AlgaDecision, completeness);
    o->off_decision_explanation  = (uint32_t)offsetof(AlgaDecision, explanation);
    o->off_decision_reason       = (uint32_t)offsetof(AlgaDecision, reason);
    o->off_baseline_cov          = (uint32_t)offsetof(AlgaBaseline, cov);
}

/* =========================================================================
 * Configuration
 * ========================================================================= */

void algaops_engine_config_default(AlgaEngineConfig *c)
{
    if (!c) return;
    memset(c, 0, sizeof(*c));
    c->trust  = trustguard_default_config();
    c->window = window_policy_default_config();
    c->mode   = operating_mode_default_config();
    c->cross  = cross_sensor_default_config();
    c->health = health_methods_default_config();
    c->primary_method[ALGA_MODE_UNKNOWN] = -1;
    c->primary_method[ALGA_MODE_OFF]     = ALGA_HM_FIXED;
    c->primary_method[ALGA_MODE_STARTUP] = ALGA_HM_FIXED;
    c->primary_method[ALGA_MODE_STEADY]  = ALGA_HM_ZSCORE;
    c->min_baseline_samples  = (uint32_t)equipment_health_default_config().min_baseline_samples;
    c->alert_confirm_windows = 1;
    c->alert_clear_windows   = 1;
}

size_t algaops_engine_config_size(void) { return sizeof(AlgaEngineConfig); }

typedef enum { K_DOUBLE, K_INT, K_U32, K_BOOL, K_METHOD, K_FMASK } KeyType;
typedef struct { const char *name; KeyType type; size_t offset; uint32_t bit; } KeyDef;

#define KD(n, t, f)     { n, t, offsetof(AlgaEngineConfig, f), 0u }
#define KF(n, b)        { n, K_FMASK, offsetof(AlgaEngineConfig, health.feature_mask), ALGA_FMASK(b) }

static const KeyDef KEYS[] = {
    KD("trust.vib_min_g",                  K_DOUBLE, trust.vib_min_g),
    KD("trust.vib_max_g",                  K_DOUBLE, trust.vib_max_g),
    KD("trust.temp_min_c",                 K_DOUBLE, trust.temp_min_c),
    KD("trust.temp_max_c",                 K_DOUBLE, trust.temp_max_c),
    KD("trust.stuck_window_size",          K_INT,    trust.stuck_window_size),
    KD("trust.stuck_vib_threshold_g",      K_DOUBLE, trust.stuck_vib_threshold_g),
    KD("trust.stuck_temp_threshold_c",     K_DOUBLE, trust.stuck_temp_threshold_c),
    KD("trust.trust_score_threshold",      K_INT,    trust.trust_score_threshold),
    KD("trust.stuck_temp_window_size",     K_INT,    trust.stuck_temp_window_size),
    KD("trust.min_window_completeness",    K_DOUBLE, trust.min_window_completeness),
    KD("trust.max_staleness_ms",           K_U32,    trust.max_staleness_ms),
    KD("trust.max_future_skew_ms",         K_U32,    trust.max_future_skew_ms),
    KD("trust.require_time_sync",          K_BOOL,   trust.require_time_sync),
    KD("trust.max_temp_rate_c_per_s",      K_DOUBLE, trust.max_temp_rate_c_per_s),
    KD("trust.consistency_rel_tol",        K_DOUBLE, trust.consistency_rel_tol),
    KD("trust.flag_zero_p2p_as_stuck",     K_BOOL,   trust.flag_zero_p2p_as_stuck),
    KD("trust.current_min_a",              K_DOUBLE, trust.current_min_a),
    KD("trust.current_max_a",              K_DOUBLE, trust.current_max_a),
    KD("trust.window_timing_tolerance_ms", K_U32,    trust.window_timing_tolerance_ms),
    KD("trust.history_reset_gap",          K_U32,    trust.history_reset_gap),
    KD("window.completeness_window",       K_U32,    window.completeness_window),
    KD("window.min_completeness",          K_DOUBLE, window.min_completeness),
    KD("window.min_span",                  K_U32,    window.min_span),
    KD("window.stream_timeout_ms",         K_U32,    window.stream_timeout_ms),
    KD("mode.enabled",                     K_BOOL,   mode.enabled),
    KD("mode.vib_on_g",                    K_DOUBLE, mode.vib_on_g),
    KD("mode.vib_off_g",                   K_DOUBLE, mode.vib_off_g),
    KD("mode.current_on_a",                K_DOUBLE, mode.current_on_a),
    KD("mode.current_off_a",               K_DOUBLE, mode.current_off_a),
    KD("mode.startup_ms",                  K_U32,    mode.startup_ms),
    KD("cross.enabled",                    K_BOOL,   cross.enabled),
    KD("cross.grace_ms",                   K_U32,    cross.grace_ms),
    KD("health.z_threshold",               K_DOUBLE, health.z_threshold),
    KD("health.std_floor.vib_rms_g",       K_DOUBLE, health.std_floor[ALGA_F_VIB_RMS]),
    KD("health.std_floor.vib_p2p_g",       K_DOUBLE, health.std_floor[ALGA_F_VIB_P2P]),
    KD("health.std_floor.temp_c",          K_DOUBLE, health.std_floor[ALGA_F_TEMP]),
    KD("health.std_floor.current_a",       K_DOUBLE, health.std_floor[ALGA_F_CURRENT]),
    KD("health.maha_threshold",            K_DOUBLE, health.maha_threshold),
    KD("health.maha_shrinkage",            K_DOUBLE, health.maha_shrinkage),
    KD("health.maha_min_samples_per_dim",  K_U32,    health.maha_min_samples_per_dim),
    KF("health.features.vib_rms_g",        ALGA_F_VIB_RMS),
    KF("health.features.vib_p2p_g",        ALGA_F_VIB_P2P),
    KF("health.features.temp_c",           ALGA_F_TEMP),
    KF("health.features.current_a",        ALGA_F_CURRENT),
    KD("health.fixed_always_on",           K_BOOL,   health.fixed_always_on),
    KD("health.fixed.off.vib_max_g",       K_DOUBLE, health.fixed_vib_max_g[ALGA_MODE_OFF]),
    KD("health.fixed.off.temp_max_c",      K_DOUBLE, health.fixed_temp_max_c[ALGA_MODE_OFF]),
    KD("health.fixed.off.current_max_a",   K_DOUBLE, health.fixed_current_max_a[ALGA_MODE_OFF]),
    KD("health.fixed.startup.vib_max_g",   K_DOUBLE, health.fixed_vib_max_g[ALGA_MODE_STARTUP]),
    KD("health.fixed.startup.temp_max_c",  K_DOUBLE, health.fixed_temp_max_c[ALGA_MODE_STARTUP]),
    KD("health.fixed.startup.current_max_a", K_DOUBLE, health.fixed_current_max_a[ALGA_MODE_STARTUP]),
    KD("health.fixed.steady.vib_max_g",    K_DOUBLE, health.fixed_vib_max_g[ALGA_MODE_STEADY]),
    KD("health.fixed.steady.temp_max_c",   K_DOUBLE, health.fixed_temp_max_c[ALGA_MODE_STEADY]),
    KD("health.fixed.steady.current_max_a", K_DOUBLE, health.fixed_current_max_a[ALGA_MODE_STEADY]),
    KD("method.off",                       K_METHOD, primary_method[ALGA_MODE_OFF]),
    KD("method.startup",                   K_METHOD, primary_method[ALGA_MODE_STARTUP]),
    KD("method.steady",                    K_METHOD, primary_method[ALGA_MODE_STEADY]),
    KD("baseline.min_samples",             K_U32,    min_baseline_samples),
    KD("alert.confirm_windows",            K_U32,    alert_confirm_windows),
    KD("alert.clear_windows",              K_U32,    alert_clear_windows),
};
#define KEY_COUNT ((int)(sizeof(KEYS) / sizeof(KEYS[0])))

static const KeyDef *find_key(const char *key)
{
    if (!key) return NULL;
    for (int i = 0; i < KEY_COUNT; i++) {
        if (strcmp(KEYS[i].name, key) == 0) return &KEYS[i];
    }
    return NULL;
}

int algaops_config_key_count(void) { return KEY_COUNT; }

const char *algaops_config_key_name(int index)
{
    return (index >= 0 && index < KEY_COUNT) ? KEYS[index].name : NULL;
}

int algaops_config_key_is_string(const char *key)
{
    const KeyDef *k = find_key(key);
    if (!k) return -1;
    return k->type == K_METHOD ? 1 : 0;
}

static bool integral(double v) { return isfinite(v) && floor(v) == v; }

AlgaopsStatus algaops_config_set_number(AlgaEngineConfig *cfg, const char *key, double v)
{
    if (!cfg || !key) return ALGAOPS_ERR_NULL_PTR;
    const KeyDef *k = find_key(key);
    if (!k || k->type == K_METHOD) return ALGAOPS_ERR_CONFIG;
    char *p = (char *)cfg + k->offset;
    switch (k->type) {
        case K_DOUBLE:
            if (!isfinite(v)) return ALGAOPS_ERR_RANGE;
            *(double *)(void *)p = v;
            break;
        case K_INT:
            if (!integral(v) || v < -2147483648.0 || v > 2147483647.0) return ALGAOPS_ERR_RANGE;
            *(int *)(void *)p = (int)v;
            break;
        case K_U32:
            if (!integral(v) || v < 0.0 || v > 4294967295.0) return ALGAOPS_ERR_RANGE;
            *(uint32_t *)(void *)p = (uint32_t)v;
            break;
        case K_BOOL:
            if (v != 0.0 && v != 1.0) return ALGAOPS_ERR_RANGE;
            *(bool *)(void *)p = (v != 0.0);
            break;
        case K_FMASK: {
            if (v != 0.0 && v != 1.0) return ALGAOPS_ERR_RANGE;
            uint32_t *m = (uint32_t *)(void *)p;
            if (v != 0.0) *m |= k->bit; else *m &= ~k->bit;
            break;
        }
        default:
            return ALGAOPS_ERR_CONFIG;
    }
    return ALGAOPS_OK;
}

AlgaopsStatus algaops_config_set_string(AlgaEngineConfig *cfg, const char *key, const char *value)
{
    if (!cfg || !key || !value) return ALGAOPS_ERR_NULL_PTR;
    const KeyDef *k = find_key(key);
    if (!k || k->type != K_METHOD) return ALGAOPS_ERR_CONFIG;
    int m = strcmp(value, "none") == 0 ? -1 : health_method_parse(value);
    if (m < -1 || (m == -1 && strcmp(value, "none") != 0)) return ALGAOPS_ERR_RANGE;
    *(int32_t *)(void *)((char *)cfg + k->offset) = m;
    return ALGAOPS_OK;
}

AlgaopsStatus algaops_config_get_number(const AlgaEngineConfig *cfg, const char *key, double *v)
{
    if (!cfg || !key || !v) return ALGAOPS_ERR_NULL_PTR;
    const KeyDef *k = find_key(key);
    if (!k || k->type == K_METHOD) return ALGAOPS_ERR_CONFIG;
    const char *p = (const char *)cfg + k->offset;
    switch (k->type) {
        case K_DOUBLE: *v = *(const double *)(const void *)p; break;
        case K_INT:    *v = (double)*(const int *)(const void *)p; break;
        case K_U32:    *v = (double)*(const uint32_t *)(const void *)p; break;
        case K_BOOL:   *v = *(const bool *)(const void *)p ? 1.0 : 0.0; break;
        case K_FMASK:  *v = (*(const uint32_t *)(const void *)p & k->bit) ? 1.0 : 0.0; break;
        default:       return ALGAOPS_ERR_CONFIG;
    }
    return ALGAOPS_OK;
}

const char *algaops_config_get_string(const AlgaEngineConfig *cfg, const char *key)
{
    if (!cfg || !key) return NULL;
    const KeyDef *k = find_key(key);
    if (!k || k->type != K_METHOD) return NULL;
    int32_t m = *(const int32_t *)(const void *)((const char *)cfg + k->offset);
    return m < 0 ? "none" : health_method_str(m);
}

AlgaopsStatus algaops_config_validate(const AlgaEngineConfig *c, char *err, size_t len)
{
#define BAD(...) do { if (err && len) snprintf(err, len, __VA_ARGS__); \
                      return ALGAOPS_ERR_CONFIG; } while (0)
    if (err && len) err[0] = '\0';
    if (!c) return ALGAOPS_ERR_NULL_PTR;
    const TrustGuardConfig *t = &c->trust;
    if (t->stuck_window_size < 1 || t->stuck_window_size > TRUSTGUARD_MAX_WINDOW)
        BAD("trust.stuck_window_size must be in [1, %d]", TRUSTGUARD_MAX_WINDOW);
    if (t->stuck_temp_window_size < 0 || t->stuck_temp_window_size > TRUSTGUARD_MAX_WINDOW)
        BAD("trust.stuck_temp_window_size must be in [0, %d]", TRUSTGUARD_MAX_WINDOW);
    if (t->trust_score_threshold < 0 || t->trust_score_threshold > 100)
        BAD("trust.trust_score_threshold must be in [0, 100]");
    if (!(t->vib_min_g <= t->vib_max_g)) BAD("trust.vib_min_g must be <= trust.vib_max_g");
    if (!(t->temp_min_c <= t->temp_max_c)) BAD("trust.temp_min_c must be <= trust.temp_max_c");
    if (!(t->current_min_a <= t->current_max_a))
        BAD("trust.current_min_a must be <= trust.current_max_a");
    if (t->min_window_completeness < 0.0 || t->min_window_completeness > 1.0)
        BAD("trust.min_window_completeness must be in [0, 1]");
    if (t->max_temp_rate_c_per_s < 0.0) BAD("trust.max_temp_rate_c_per_s must be >= 0");
    if (t->consistency_rel_tol < 0.0 || t->consistency_rel_tol >= 1.0)
        BAD("trust.consistency_rel_tol must be in [0, 1)");
    if (c->window.completeness_window > WINDOW_POLICY_MAX_SLOTS)
        BAD("window.completeness_window must be <= %d", WINDOW_POLICY_MAX_SLOTS);
    if (c->window.min_completeness < 0.0 || c->window.min_completeness > 1.0)
        BAD("window.min_completeness must be in [0, 1]");
    if (c->mode.vib_off_g > c->mode.vib_on_g) BAD("mode.vib_off_g must be <= mode.vib_on_g");
    if (c->mode.current_off_a > c->mode.current_on_a)
        BAD("mode.current_off_a must be <= mode.current_on_a");
    if (!(c->health.z_threshold > 0.0)) BAD("health.z_threshold must be > 0");
    for (int i = 0; i < F; i++) {
        if (!(c->health.std_floor[i] > 0.0)) BAD("health.std_floor.%s must be > 0",
                                                 alga_feature_name(i));
    }
    if (c->health.maha_shrinkage < 0.0 || c->health.maha_shrinkage >= 1.0)
        BAD("health.maha_shrinkage must be in [0, 1)");
    if (c->health.maha_threshold < 0.0) BAD("health.maha_threshold must be >= 0");
    if (c->health.feature_mask == 0) BAD("at least one health.features.* must be enabled");
    for (int m = ALGA_MODE_OFF; m < ALGA_MODE_COUNT; m++) {
        if (c->primary_method[m] < -1 || c->primary_method[m] >= ALGA_HM_COUNT)
            BAD("invalid primary method for mode %s", operating_mode_str(m));
    }
    if (c->min_baseline_samples < 2) BAD("baseline.min_samples must be >= 2");
    if (c->alert_confirm_windows < 1) BAD("alert.confirm_windows must be >= 1");
    if (c->alert_clear_windows < 1) BAD("alert.clear_windows must be >= 1");
#undef BAD
    return ALGAOPS_OK;
}

/* ---- flat TOML subset loader ------------------------------------------- */

static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
    return s;
}

AlgaopsStatus algaops_config_load_file(AlgaEngineConfig *cfg, const char *path,
                                       char *err, size_t err_len)
{
    if (err && err_len) err[0] = '\0';
    if (!cfg || !path) return ALGAOPS_ERR_NULL_PTR;
    FILE *f = fopen(path, "r");
    if (!f) {
        if (err && err_len) snprintf(err, err_len, "cannot open policy file '%s'", path);
        return ALGAOPS_ERR_IO;
    }
    char line[512], section[128] = "";
    int lineno = 0;
    AlgaopsStatus result = ALGAOPS_OK;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        /* strip comment outside quotes */
        bool in_q = false;
        for (char *p = line; *p; p++) {
            if (*p == '"') in_q = !in_q;
            if (*p == '#' && !in_q) { *p = '\0'; break; }
        }
        char *s = trim(line);
        if (!*s) continue;
        if (*s == '[') {
            char *e = strchr(s, ']');
            if (!e) { result = ALGAOPS_ERR_PARSE; break; }
            *e = '\0';
            snprintf(section, sizeof(section), "%s", trim(s + 1));
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq) { result = ALGAOPS_ERR_PARSE; break; }
        *eq = '\0';
        char *key = trim(s), *val = trim(eq + 1);
        char full[256];
        if (section[0]) snprintf(full, sizeof(full), "%s.%s", section, key);
        else            snprintf(full, sizeof(full), "%s", key);
        if (strncmp(full, "meta.", 5) == 0) continue;   /* descriptive keys */

        AlgaopsStatus st;
        if (*val == '"') {
            char *q = strrchr(val + 1, '"');
            if (!q) { result = ALGAOPS_ERR_PARSE; break; }
            *q = '\0';
            st = algaops_config_set_string(cfg, full, val + 1);
        } else if (strcmp(val, "true") == 0) {
            st = algaops_config_set_number(cfg, full, 1.0);
        } else if (strcmp(val, "false") == 0) {
            st = algaops_config_set_number(cfg, full, 0.0);
        } else {
            char *end = NULL;
            double d = strtod(val, &end);
            if (end == val || *trim(end) != '\0') { result = ALGAOPS_ERR_PARSE; break; }
            st = algaops_config_set_number(cfg, full, d);
        }
        if (st != ALGAOPS_OK) {
            if (err && err_len) {
                snprintf(err, err_len, "%s:%d: %s key '%s'", path, lineno,
                         st == ALGAOPS_ERR_CONFIG ? "unknown or mistyped" : "invalid value for",
                         full);
            }
            fclose(f);
            return st;
        }
    }
    fclose(f);
    if (result != ALGAOPS_OK) {
        if (err && err_len) snprintf(err, err_len, "%s:%d: syntax error", path, lineno);
        return result;
    }
    return algaops_config_validate(cfg, err, err_len);
}

/* =========================================================================
 * Engine lifecycle
 * ========================================================================= */

static void engine_reset_stream(AlgaEngine *e)
{
    trustguard_reset(&e->tg);
    window_policy_init(&e->wp);
    operating_mode_init(&e->om);
    cross_sensor_init(&e->cs);
    e->has_last = false;
    e->last_outcome = DIAG_NORMAL;
    e->consecutive = 0;
    e->alert_open = false;
    e->alert_kind = -1;
    e->has_identity = false;
    e->last_boot_id = 0;
    e->last_seq = 0;
}

AlgaEngine *algaops_engine_create(const AlgaEngineConfig *cfg)
{
    if (!cfg) return NULL;
    char err[256];
    if (algaops_config_validate(cfg, err, sizeof(err)) != ALGAOPS_OK) {
        fprintf(stderr, "[engine] invalid configuration: %s\n", err);
        return NULL;
    }
    AlgaEngine *e = (AlgaEngine *)calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->cfg = *cfg;
    if (trustguard_init(&e->tg, &cfg->trust) != ALGAOPS_OK) {
        free(e);
        return NULL;
    }
    engine_reset_stream(e);
    return e;
}

void algaops_engine_destroy(AlgaEngine *e) { free(e); }

AlgaopsStatus algaops_engine_reset(AlgaEngine *e)
{
    if (!e) return ALGAOPS_ERR_NULL_PTR;
    engine_reset_stream(e);
    return ALGAOPS_OK;
}

AlgaopsStatus algaops_engine_set_baseline(AlgaEngine *e, const AlgaBaseline *b)
{
    if (!e || !b) return ALGAOPS_ERR_NULL_PTR;
    AlgaopsStatus s = alga_baseline_validate(b);
    if (s != ALGAOPS_OK) return s;
    e->baselines[b->mode] = *b;
    return ALGAOPS_OK;
}

AlgaopsStatus algaops_engine_clear_baseline(AlgaEngine *e, int mode)
{
    if (!e) return ALGAOPS_ERR_NULL_PTR;
    if (mode < 0 || mode >= ALGA_MODE_COUNT) return ALGAOPS_ERR_RANGE;
    memset(&e->baselines[mode], 0, sizeof(e->baselines[mode]));
    return ALGAOPS_OK;
}

AlgaopsStatus algaops_engine_get_baseline(const AlgaEngine *e, int mode, AlgaBaseline *out)
{
    if (!e || !out) return ALGAOPS_ERR_NULL_PTR;
    if (mode < 0 || mode >= ALGA_MODE_COUNT) return ALGAOPS_ERR_RANGE;
    *out = e->baselines[mode];
    return e->baselines[mode].valid ? ALGAOPS_OK : ALGAOPS_ERR_BASELINE;
}

/* =========================================================================
 * Reason formatting
 * ========================================================================= */

typedef struct { char *buf; size_t len; size_t used; } Sb;

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 2, 3)))
#endif
static void sb_printf(Sb *sb, const char *fmt, ...)
{
    if (sb->used + 1 >= sb->len) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(sb->buf + sb->used, sb->len - sb->used, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    sb->used += (size_t)n;
    if (sb->used >= sb->len) sb->used = sb->len - 1;
}

static const char *feat_label(int i)
{
    static const char *L[F] = { "vibration", "peak-to-peak", "temperature", "current" };
    return (i >= 0 && i < F) ? L[i] : "feature";
}
static const char *feat_short(int i)
{
    static const char *L[F] = { "vib", "p2p", "temp", "current" };
    return (i >= 0 && i < F) ? L[i] : "f";
}
static const char *feat_unit(int i)
{
    static const char *L[F] = { "g", "g", "°C", "A" };
    return (i >= 0 && i < F) ? L[i] : "";
}
static int feat_prec(int i)
{
    static const int P[F] = { 4, 4, 2, 3 };
    return (i >= 0 && i < F) ? P[i] : 4;
}

static void reason_head(Sb *sb, const AlgaEngine *e, const char *outcome, int mode, int method)
{
    if (e->cfg.mode.enabled) {
        sb_printf(sb, "%s (%s, %s): ", outcome, operating_mode_str(mode),
                  health_method_str(method));
    } else {
        sb_printf(sb, "%s: ", outcome);
    }
}

static void reason_zscore(Sb *sb, const AlgaEngine *e, const AlgaDecision *d,
                          const AlgaMethodResult *r, const AlgaBaseline *bl)
{
    bool anomalous = r->anomalous != 0;
    reason_head(sb, e, anomalous ? "CHECK_EQUIPMENT" : "NORMAL", d->mode, ALGA_HM_ZSCORE);
    uint32_t use = bl->feature_mask & e->cfg.health.feature_mask & d->feature_mask;
    bool first = true;
    if (anomalous) {
        for (int i = 0; i < F; i++) {
            if (!(use & ALGA_FMASK(i)) || !(r->feature_z[i] > r->threshold)) continue;
            int p = feat_prec(i);
            sb_printf(sb, "%s%s z=%.2f > threshold %.2f (%.*f %s vs baseline %.*f±%.*f %s)",
                      first ? "" : "; ", feat_label(i), r->feature_z[i], r->threshold,
                      p, d->features[i], feat_unit(i), p, bl->mean[i], p, bl->std[i],
                      feat_unit(i));
            first = false;
        }
    } else {
        for (int i = 0; i < F; i++) {
            if (!(use & ALGA_FMASK(i))) continue;
            sb_printf(sb, "%s%s z=%.2f", first ? "" : ", ", feat_short(i), r->feature_z[i]);
            first = false;
        }
        sb_printf(sb, " (%s <= threshold %.2f)", r->dims == 2 ? "both" : "all", r->threshold);
    }
}

static void reason_fixed(Sb *sb, const AlgaEngine *e, const AlgaDecision *d,
                         const AlgaMethodResult *r)
{
    bool anomalous = r->anomalous != 0;
    reason_head(sb, e, anomalous ? "CHECK_EQUIPMENT" : "NORMAL", d->mode, ALGA_HM_FIXED);
    const HealthMethodsConfig *h = &e->cfg.health;
    double lim[F] = { h->fixed_vib_max_g[d->mode], 0.0, h->fixed_temp_max_c[d->mode],
                      h->fixed_current_max_a[d->mode] };
    bool first = true;
    for (int i = 0; i < F; i++) {
        if (lim[i] <= 0.0 || !(d->feature_mask & ALGA_FMASK(i))) continue;
        int p = feat_prec(i);
        if (anomalous) {
            if (!(d->features[i] > lim[i])) continue;
            sb_printf(sb, "%s%s %.*f %s > fixed limit %.*f %s", first ? "" : "; ",
                      feat_label(i), p, d->features[i], feat_unit(i), p, lim[i], feat_unit(i));
        } else {
            sb_printf(sb, "%s%s %.*f %s", first ? "" : ", ", feat_label(i), p,
                      d->features[i], feat_unit(i));
        }
        first = false;
    }
    if (!anomalous) sb_printf(sb, " within fixed limits");
}

static void reason_maha(Sb *sb, const AlgaEngine *e, const AlgaDecision *d,
                        const AlgaMethodResult *r)
{
    bool anomalous = r->anomalous != 0;
    reason_head(sb, e, anomalous ? "CHECK_EQUIPMENT" : "NORMAL", d->mode, ALGA_HM_MAHALANOBIS);
    sb_printf(sb, "D²=%.2f %s %.2f (%d features", r->score, anomalous ? ">" : "<=",
              r->threshold, (int)r->dims);
    if (r->top_feature >= 0) {
        sb_printf(sb, "; largest |z| %s %.2f", feat_label(r->top_feature),
                  r->feature_z[r->top_feature]);
    }
    sb_printf(sb, ")");
}

/* =========================================================================
 * Alert state
 * ========================================================================= */

/* `confirmed` marks a condition that is persistent by construction (a stream
 * timeout already means stream_timeout_ms of silence, and no further windows
 * will arrive to confirm it), so it opens the alert without confirm windows. */
static void alert_update(AlgaEngine *e, AlgaDecision *d, bool confirmed)
{
    int32_t o = d->outcome;
    if (e->has_last && o == e->last_outcome) e->consecutive++;
    else e->consecutive = 1;
    e->last_outcome = o;
    e->has_last = true;
    d->consecutive = e->consecutive;
    d->alert_event = ALGA_ALERT_NONE;
    d->alert_prev_kind = -1;
    d->alert_kind = e->alert_open ? e->alert_kind : -1;

    if (o == DIAG_CHECK_SENSOR || o == DIAG_CHECK_EQUIPMENT) {
        if ((confirmed || e->consecutive >= e->cfg.alert_confirm_windows) &&
            (!e->alert_open || e->alert_kind != o)) {
            d->alert_event = e->alert_open ? ALGA_ALERT_CHANGED : ALGA_ALERT_OPENED;
            d->alert_prev_kind = e->alert_open ? e->alert_kind : -1;
            e->alert_open = true;
            e->alert_kind = o;
            d->alert_kind = o;
        }
    } else if (o == DIAG_NORMAL) {
        if (e->alert_open && e->consecutive >= e->cfg.alert_clear_windows) {
            d->alert_event = ALGA_ALERT_CLEARED;
            d->alert_kind = e->alert_kind;
            e->alert_open = false;
            e->alert_kind = -1;
        }
    }
    /* NOT_EVALUATED neither opens nor clears an alert. */
}

/* =========================================================================
 * Decisions
 * ========================================================================= */

static void decision_init(AlgaDecision *d)
{
    memset(d, 0, sizeof(*d));
    d->primary_method = -1;
    d->decided_by = -1;
    d->alert_kind = -1;
    d->alert_prev_kind = -1;
    d->completeness = -1.0;
    d->mode = ALGA_MODE_UNKNOWN;
    for (int m = 0; m < ALGA_HM_COUNT; m++) {
        d->methods[m].status = ALGA_METHOD_SKIPPED;
        d->methods[m].top_feature = -1;
    }
}

AlgaopsStatus algaops_engine_process(AlgaEngine *e, const AlgaWindow *w, AlgaDecision *d)
{
    if (!e || !w || !d) return ALGAOPS_ERR_NULL_PTR;
    decision_init(d);
    d->kind         = ALGA_DECISION_WINDOW;
    d->boot_id      = w->boot_id;
    d->sequence_id  = w->sequence_id;
    d->timestamp_ms = w->timestamp_ms;
    d->received_ms  = (w->presence & ALGA_HAS_RECEIVED) ? w->received_ms : 0;

    TrustResult tr;
    trustguard_validate_window(&e->tg, w, &tr);
    d->missing_count = e->tg.last_missing_count;

    /* Stream-level policy. */
    window_policy_touch(&e->wp, (w->presence & ALGA_HAS_RECEIVED) ? w->received_ms
                                                                  : w->timestamp_ms);
    bool big_gap = e->cfg.trust.history_reset_gap > 0 &&
                   e->tg.last_missing_count >= e->cfg.trust.history_reset_gap;
    if (e->tg.last_reboot) {
        uint64_t keep = e->wp.last_received_ms;
        window_policy_init(&e->wp);
        window_policy_touch(&e->wp, keep);
    }
    if (e->tg.last_reboot || big_gap) {
        operating_mode_init(&e->om);
        cross_sensor_init(&e->cs);
    }
    if (e->tg.last_forward) {
        d->completeness = window_policy_record(&e->wp, &e->cfg.window, e->tg.last_missing_count);
    } else {
        d->completeness = window_policy_completeness(&e->wp, &e->cfg.window);
    }
    if (window_policy_low_completeness(&e->wp, &e->cfg.window)) {
        trustguard_add_flag(&tr, INTEGRITY_LOW_COMPLETENESS,
                            "low data completeness: %.0f%% of the last %u expected windows "
                            "arrived (< %.0f%%)",
                            d->completeness * 100.0, (unsigned)e->wp.span,
                            e->cfg.window.min_completeness * 100.0);
    }

    /* Cross-sensor consistency (finite, in-order data only). */
    bool cross_pending = false;
    char cross_expl[512];
    cross_expl[0] = '\0';
    if (!(tr.flags & (INTEGRITY_NON_FINITE | INTEGRITY_DUPLICATE_SEQ |
                      INTEGRITY_OUT_OF_ORDER_SEQ))) {
        CrossSensorVerdict cv = cross_sensor_check(&e->cs, &e->cfg.cross, &e->cfg.mode, w,
                                                   cross_expl, sizeof(cross_expl));
        if (cv == CROSS_FLAG) {
            trustguard_add_flag(&tr, INTEGRITY_CROSS_SENSOR, "%s", cross_expl);
        } else if (cv == CROSS_PENDING) {
            cross_pending = true;
        }
    }
    trustguard_finalize(&e->cfg.trust, &tr);

    d->trust_score = tr.trust_score;
    d->is_trusted  = tr.is_trusted ? 1 : 0;
    d->flags       = tr.flags;
    memcpy(d->explanation, tr.explanation, sizeof(d->explanation));
    d->feature_mask = alga_window_features(w, d->features);

    Sb sb = { d->reason, sizeof(d->reason), 0 };
    d->reason[0] = '\0';

    if (!tr.is_trusted) {
        /* Trust gate: equipment health is never evaluated on untrusted data. */
        d->outcome = DIAG_CHECK_SENSOR;
        d->mode = ALGA_MODE_UNKNOWN;
        sb_printf(&sb, "CHECK_SENSOR: data integrity blocked evaluation — %.900s",
                  tr.explanation);
    } else {
        bool changed = false;
        uint64_t tim = 0;
        d->mode = operating_mode_update(&e->om, &e->cfg.mode, w, &changed, &tim);
        d->mode_changed = changed ? 1 : 0;
        d->time_in_mode_ms = tim;

        const AlgaBaseline *bl = e->baselines[d->mode].valid ? &e->baselines[d->mode] : NULL;
        if (cross_pending) {
            /* Running-state transient: vibration and current disagree right after a
             * current change.  Health is withheld (methods stay SKIPPED). */
            d->primary_method = e->cfg.primary_method[d->mode];
            d->outcome = DIAG_NOT_EVALUATED;
            sb_printf(&sb, "NOT_EVALUATED (%s): %s", operating_mode_str(d->mode), cross_expl);
            goto decided;
        }
        health_fixed(&e->cfg.health, d->mode, d->features, d->feature_mask,
                     &d->methods[ALGA_HM_FIXED]);
        health_zscore(&e->cfg.health, bl, d->features, d->feature_mask,
                      &d->methods[ALGA_HM_ZSCORE]);
        health_mahalanobis(&e->cfg.health, bl, d->features, d->feature_mask,
                           &d->methods[ALGA_HM_MAHALANOBIS]);

        int pm = e->cfg.primary_method[d->mode];
        d->primary_method = pm;
        const AlgaMethodResult *fx = &d->methods[ALGA_HM_FIXED];
        bool primary_ok = pm >= 0 && d->methods[pm].status == ALGA_METHOD_OK;
        bool hard_limit = e->cfg.health.fixed_always_on && pm != ALGA_HM_FIXED &&
                          fx->status == ALGA_METHOD_OK && fx->anomalous;
        if (hard_limit) {
            /* Absolute limits apply on top of the primary method. */
            d->outcome = DIAG_CHECK_EQUIPMENT;
            d->decided_by = ALGA_HM_FIXED;
            reason_fixed(&sb, e, d, fx);
            if (primary_ok) {
                d->baseline_id = bl ? bl->id : 0;
                sb_printf(&sb, "; primary %s %s", health_method_str(pm),
                          d->methods[pm].anomalous ? "also flags a deviation"
                                                   : "finds no baseline deviation");
            } else {
                sb_printf(&sb, "; primary %s not evaluated", health_method_str(pm));
            }
        } else if (primary_ok) {
            const AlgaMethodResult *r = &d->methods[pm];
            d->outcome = r->anomalous ? DIAG_CHECK_EQUIPMENT : DIAG_NORMAL;
            d->decided_by = pm;
            if (pm == ALGA_HM_FIXED) {
                reason_fixed(&sb, e, d, r);
            } else {
                d->baseline_id = bl ? bl->id : 0;
                if (pm == ALGA_HM_ZSCORE) reason_zscore(&sb, e, d, r, bl);
                else                      reason_maha(&sb, e, d, r);
            }
        } else {
            d->outcome = DIAG_NOT_EVALUATED;
            const char *ms = operating_mode_str(d->mode);
            if (pm < 0) {
                sb_printf(&sb, "NOT_EVALUATED (%s): no health method configured for this mode",
                          ms);
            } else if (!bl && pm != ALGA_HM_FIXED) {
                sb_printf(&sb, "NOT_EVALUATED (%s): no valid %s baseline for method %s — "
                          "equipment health not evaluated", ms, ms, health_method_str(pm));
            } else if (pm == ALGA_HM_MAHALANOBIS) {
                sb_printf(&sb, "NOT_EVALUATED (%s): mahalanobis unavailable (needs >= %u "
                          "baseline samples per feature, baseline has %u) — equipment "
                          "health not evaluated", ms,
                          (unsigned)e->cfg.health.maha_min_samples_per_dim,
                          bl ? (unsigned)bl->n : 0u);
            } else {
                sb_printf(&sb, "NOT_EVALUATED (%s): method %s has no applicable features",
                          ms, health_method_str(pm));
            }
        }
    }

decided:
    e->has_identity = true;
    e->last_boot_id = w->boot_id;
    if (e->tg.last_forward) e->last_seq = w->sequence_id;
    alert_update(e, d, false);
    return ALGAOPS_OK;
}

AlgaopsStatus algaops_engine_tick(AlgaEngine *e, uint64_t now_ms, AlgaDecision *d, int *emitted)
{
    if (!e || !d || !emitted) return ALGAOPS_ERR_NULL_PTR;
    *emitted = 0;
    uint64_t silence = 0;
    if (!window_policy_tick(&e->wp, &e->cfg.window, now_ms, &silence)) return ALGAOPS_OK;

    decision_init(d);
    d->kind = ALGA_DECISION_STREAM_TIMEOUT;
    d->boot_id = e->last_boot_id;
    d->sequence_id = e->last_seq;
    d->received_ms = now_ms;
    d->trust_score = 0;
    d->is_trusted = 0;
    d->flags = INTEGRITY_STREAM_TIMEOUT;
    d->outcome = DIAG_CHECK_SENSOR;
    d->completeness = window_policy_completeness(&e->wp, &e->cfg.window);
    snprintf(d->explanation, sizeof(d->explanation),
             "no telemetry received for %.1f s (timeout %.1f s); last sequence_id %u",
             (double)silence / 1000.0, (double)e->cfg.window.stream_timeout_ms / 1000.0,
             (unsigned)e->last_seq);
    snprintf(d->reason, sizeof(d->reason),
             "CHECK_SENSOR: data stream stale — %.900s", d->explanation);
    /* After silence the running state is unknown: restart mode inference. */
    operating_mode_init(&e->om);
    cross_sensor_init(&e->cs);
    alert_update(e, d, true);
    *emitted = 1;
    return ALGAOPS_OK;
}

AlgaopsStatus algaops_engine_report_malformed(AlgaEngine *e, uint64_t received_ms,
                                              const char *detail, AlgaDecision *d)
{
    if (!e || !d) return ALGAOPS_ERR_NULL_PTR;
    decision_init(d);
    d->kind = ALGA_DECISION_MALFORMED;
    d->boot_id = e->last_boot_id;
    d->sequence_id = e->last_seq;
    d->received_ms = received_ms;
    d->trust_score = 0;
    d->is_trusted = 0;
    d->flags = INTEGRITY_MALFORMED;
    d->outcome = DIAG_CHECK_SENSOR;
    snprintf(d->explanation, sizeof(d->explanation), "%.900s",
             (detail && *detail) ? detail : "malformed record");
    snprintf(d->reason, sizeof(d->reason),
             "CHECK_SENSOR: record could not be parsed (%.900s)", d->explanation);
    if (received_ms) window_policy_touch(&e->wp, received_ms);
    alert_update(e, d, false);
    return ALGAOPS_OK;
}

AlgaopsStatus algaops_engine_restore_alert(AlgaEngine *e, int32_t open_kind)
{
    if (!e) return ALGAOPS_ERR_NULL_PTR;
    if (open_kind != -1 && open_kind != DIAG_CHECK_SENSOR && open_kind != DIAG_CHECK_EQUIPMENT) {
        return ALGAOPS_ERR_RANGE;
    }
    e->alert_open = open_kind != -1;
    e->alert_kind = open_kind;
    return ALGAOPS_OK;
}

const char *algaops_mode_str(int mode) { return operating_mode_str(mode); }
const char *algaops_method_str(int method) { return health_method_str(method); }

const char *algaops_alert_event_str(int event)
{
    switch (event) {
        case ALGA_ALERT_OPENED:  return "OPENED";
        case ALGA_ALERT_CLEARED: return "CLEARED";
        case ALGA_ALERT_CHANGED: return "CHANGED";
        default:                 return "NONE";
    }
}

/* =========================================================================
 * CSV helpers
 * ========================================================================= */

AlgaopsStatus algaops_replay_csv(AlgaEngine *e, const char *path,
                                 AlgaDecisionCallback cb, void *user, int *rows)
{
    if (rows) *rows = 0;
    if (!e || !path) return ALGAOPS_ERR_NULL_PTR;
    CsvReader reader;
    AlgaopsStatus s = csv_reader_open(&reader, path);
    if (s != ALGAOPS_OK) return s;
    AlgaDecision *d = (AlgaDecision *)malloc(sizeof(*d));
    if (!d) { csv_reader_close(&reader); return ALGAOPS_ERR_NOMEM; }
    int row = 0;
    for (;;) {
        AlgaWindow w;
        AlgaopsStatus rs = csv_reader_next_window(&reader, &w);
        if (rs == ALGAOPS_ERR_IO) break;
        row++;
        if (rs == ALGAOPS_ERR_PARSE) {
            algaops_engine_report_malformed(e, 0, "malformed CSV record", d);
            if (cb) cb(user, row, NULL, d, 1);
            continue;
        }
        algaops_engine_process(e, &w, d);
        if (cb) cb(user, row, &w, d, 0);
    }
    free(d);
    csv_reader_close(&reader);
    if (rows) *rows = row;
    return ALGAOPS_OK;
}

typedef struct {
    AlgaBaselineBuilder b[ALGA_MODE_COUNT];
} BaselineCollect;

static void collect_cb(void *user, int row, const AlgaWindow *w, const AlgaDecision *d,
                       int parse_error)
{
    (void)row;
    (void)w;
    BaselineCollect *c = (BaselineCollect *)user;
    if (parse_error || !d->is_trusted) return;
    if (d->mode < ALGA_MODE_OFF || d->mode > ALGA_MODE_STEADY) return;
    alga_baseline_builder_add(&c->b[d->mode], d->features, d->feature_mask);
}

AlgaopsStatus algaops_baselines_from_csv(const AlgaEngineConfig *cfg, const char *path,
                                         AlgaBaseline out[ALGA_MODE_COUNT], int *valid)
{
    if (valid) *valid = 0;
    if (!cfg || !path || !out) return ALGAOPS_ERR_NULL_PTR;
    AlgaEngine *e = algaops_engine_create(cfg);
    if (!e) return ALGAOPS_ERR_CONFIG;
    BaselineCollect *c = (BaselineCollect *)malloc(sizeof(*c));
    if (!c) { algaops_engine_destroy(e); return ALGAOPS_ERR_NOMEM; }
    for (int m = 0; m < ALGA_MODE_COUNT; m++) {
        alga_baseline_builder_init(&c->b[m], cfg->health.feature_mask);
    }
    int rows = 0;
    AlgaopsStatus s = algaops_replay_csv(e, path, collect_cb, c, &rows);
    int n_valid = 0;
    if (s == ALGAOPS_OK) {
        for (int m = 0; m < ALGA_MODE_COUNT; m++) {
            if (alga_baseline_builder_finish(&c->b[m], m, cfg->min_baseline_samples,
                                             &out[m]) == ALGAOPS_OK && m != ALGA_MODE_UNKNOWN) {
                out[m].id = (uint32_t)m;
                n_valid++;
            } else {
                out[m].valid = 0;
            }
        }
        if (n_valid == 0) s = ALGAOPS_ERR_BASELINE;
    }
    free(c);
    algaops_engine_destroy(e);
    if (valid) *valid = n_valid;
    return s;
}
