/*
 * node_app.c — sampling, windowing, bounded buffering and publishing.
 *
 * Tasks (measurement on core 1, Wi-Fi / MQTT on core 0):
 *   accel   prio 10, core 1  drains the ADXL345 FIFO every 10 ms and closes a
 *                            window every samples_expected samples
 *   temp    prio 6,  core 1  one DS18B20 conversion per second
 *   ct      prio 9,  core 1  optional CT sampling (current_sensor.c)
 *   publish prio 4,  core 0  publishes buffered windows with QoS 1 and removes
 *                            each only after its PUBACK; retained status
 *
 * The node measures and reports.  It has no output that could switch a pump,
 * a blower or change a PBR setpoint, and it subscribes to no command topic.
 */

#include "node_app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "adxl345.h"
#include "adxl345_convert.h"
#include "adxl345_regs.h"
#include "boot_identity.h"
#include "current_sensor.h"
#include "ds18b20.h"
#include "mqtt_link.h"
#include "ring_buffer.h"
#include "sample_window.h"
#include "sensor_health.h"
#include "sequence_counter.h"
#include "telemetry_payload.h"
#include "time_sync.h"
#include "wifi_link.h"

static const char *TAG = "node";

#define TEMP_MAX_AGE_US     (3LL * 1000000LL)  /* older temperatures are not reported */
#define ACK_TIMEOUT_MS      5000
#define BACKOFF_MAX_MS      30000
#define TOPIC_LEN           (sizeof(CONFIG_ALGAOPS_MQTT_TOPIC_PREFIX) + NODE_ID_MAX_LEN + 24)

typedef struct {
    char               node_id[NODE_ID_MAX_LEN + 1];
    char               topic_telemetry[TOPIC_LEN];
    char               topic_status[TOPIC_LEN];
    char               lwt[256];
    const char        *fw;
    boot_identity_t    boot;
    uint32_t           rate_hz;
    uint32_t           window_ms;
    adxl345_handle_t   accel;
    ds18b20_handle_t   temp;
    bool               current_present;
    sequence_counter_t seq;
    sample_window_t    window;        /* accel task only */
    ring_buffer_t      buffer;        /* guarded by buffer_lock */
    SemaphoreHandle_t  buffer_lock;
    /* guarded by s_lock: written by the accel and temp tasks */
    sensor_health_t    health;
    bool               temp_valid;
    float              temp_c;
    int64_t            temp_mono_us;
    bool               current_ok;    /* the last window had a current value */
} node_t;

static node_t      s_node;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

/* ---- set-up ----------------------------------------------------------------- */

static void init_identity(node_t *n)
{
    const char *cfg_id = CONFIG_ALGAOPS_NODE_ID;
    if (cfg_id[0] && node_id_valid(cfg_id)) {
        snprintf(n->node_id, sizeof(n->node_id), "%s", cfg_id);
    } else {
        if (cfg_id[0]) ESP_LOGW(TAG, "configured node id \"%s\" is invalid: using the MAC", cfg_id);
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(n->node_id, sizeof(n->node_id), "esp32-%02x%02x%02x%02x%02x%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    snprintf(n->topic_telemetry, sizeof(n->topic_telemetry), "%s/nodes/%s/telemetry",
             CONFIG_ALGAOPS_MQTT_TOPIC_PREFIX, n->node_id);
    snprintf(n->topic_status, sizeof(n->topic_status), "%s/nodes/%s/status",
             CONFIG_ALGAOPS_MQTT_TOPIC_PREFIX, n->node_id);
}

static void init_sensors(node_t *n)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,                              /* any free controller */
        .sda_io_num = CONFIG_ALGAOPS_I2C_SDA_GPIO,
        .scl_io_num = CONFIG_ALGAOPS_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,        /* weak; breakout boards add 4.7 kOhm */
    };
    i2c_master_bus_handle_t bus = NULL;
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &bus);
    if (err == ESP_OK) {
        adxl345_config_t ac = {
            .bus = bus,
            .address = CONFIG_ALGAOPS_ADXL345_ALT_ADDR ? ADXL345_I2C_ADDR_SDO_HIGH
                                                       : ADXL345_I2C_ADDR_SDO_LOW,
            .scl_hz = CONFIG_ALGAOPS_I2C_HZ,
            .rate_hz = n->rate_hz,
            .range_g = CONFIG_ALGAOPS_ACCEL_RANGE_G,
        };
        err = adxl345_open(&ac, &n->accel);
    }
    if (err != ESP_OK) {
        n->accel = NULL;
        sensor_health_accel_absent(&n->health);
        ESP_LOGE(TAG, "accelerometer unavailable (%s): windows report ACCEL_MISSING",
                 esp_err_to_name(err));
    } else {
        adxl345_self_test_result_t st;
        bool ran = adxl345_self_test(n->accel, CONFIG_ALGAOPS_ACCEL_SUPPLY_MV / 1000.0, &st) == ESP_OK;
        sensor_health_self_test(&n->health, ran && st.passed);
    }

    ds18b20_config_t tc = { .gpio = CONFIG_ALGAOPS_ONEWIRE_GPIO,
                            .resolution_bits = CONFIG_ALGAOPS_DS18B20_RESOLUTION };
    if (ds18b20_open(&tc, &n->temp) != ESP_OK) {
        n->temp = NULL;
        for (unsigned i = 0; i < SENSOR_HEALTH_TEMP_MISSING_AFTER; i++) {
            sensor_health_temp_absent(&n->health);
        }
    }

#if CONFIG_ALGAOPS_CURRENT_ENABLE
    current_sensor_config_t cc = { .gpio = CONFIG_ALGAOPS_CURRENT_ADC_GPIO,
                                   .ma_per_volt = CONFIG_ALGAOPS_CURRENT_MA_PER_VOLT,
                                   .sample_hz = CONFIG_ALGAOPS_CURRENT_SAMPLE_HZ };
    n->current_present = true;   /* configured: a failing CT is reported, never hidden */
    if (current_sensor_start(&cc) != ESP_OK) ESP_LOGE(TAG, "current sensor failed to start");
#endif
}

/* ---- windows ---------------------------------------------------------------- */

static void close_window(node_t *n)
{
    telemetry_record_t r;
    memset(&r, 0, sizeof(r));
    r.boot_id = n->boot.boot_id;
    r.seq = sequence_counter_next(&n->seq);
    r.t_end_mono_us = esp_timer_get_time();
    r.window_ms = n->window_ms;
    r.sample_rate_hz = (float)n->rate_hz;
    r.samples_expected = n->window.samples_expected;
    r.samples_valid = n->window.samples_valid;

    VibFeatures vf;
    if (n->window.samples_valid >= 2 &&
        sample_window_features(&n->window, ADXL345_G_PER_LSB_FULL_RES, &vf) == ALGAOPS_OK) {
        r.vib_valid = true;
        r.vib_rms_g = (float)vf.rms_mag_g;
        for (int a = 0; a < 3; a++) {
            r.vib_rms_axis_g[a] = (float)vf.rms_g[a];
            r.vib_p2p_g[a] = (float)vf.p2p_g[a];
        }
    }

    bool cur_errors = false;
    bool cur_clipped = false;
    if (n->current_present) {
        r.current_present = true;
#if CONFIG_ALGAOPS_CURRENT_ENABLE
        current_window_t cw;
        current_sensor_take_window(&cw);
        r.current_valid = cw.valid;
        r.current_a = cw.amps_rms;
        cur_errors = cw.errors > 0 || cw.samples == 0;
        cur_clipped = cw.clipped > 0;
#endif
    }

    int rssi = 0;
    r.health.rssi_valid = wifi_link_rssi(&rssi);
    r.health.rssi_dbm = (int16_t)rssi;
    r.health.uptime_s = (uint32_t)(r.t_end_mono_us / 1000000LL);
    r.health.reconnects = wifi_link_reconnects() + mqtt_link_disconnects();

    taskENTER_CRITICAL(&s_lock);
    if (cur_errors) sensor_health_current_adc_error(&n->health);
    if (cur_clipped) sensor_health_current_clipped(&n->health);
    n->current_ok = r.current_valid;
    if (n->temp_valid && r.t_end_mono_us - n->temp_mono_us <= TEMP_MAX_AGE_US) {
        r.temp_valid = true;
        r.temp_c = n->temp_c;
    }
    r.self_test = (uint8_t)n->health.self_test;
    r.faults = sensor_health_take_window_faults(&n->health);
    r.health.i2c_errors = n->health.i2c_errors;
    r.health.ow_crc_errors = n->health.ow_crc_errors;
    r.health.fifo_overruns = n->health.fifo_overruns;
    taskEXIT_CRITICAL(&s_lock);

    xSemaphoreTake(n->buffer_lock, portMAX_DELAY);
    r.health.queue_depth = ring_buffer_count(&n->buffer);
    r.health.dropped_windows = n->buffer.dropped;
    bool kept = ring_buffer_push(&n->buffer, &r);
    xSemaphoreGive(n->buffer_lock);
    if (!kept) ESP_LOGW(TAG, "buffer full: oldest window dropped (gateway reports the gap)");
    sample_window_reset(&n->window);
}

static void accel_task(void *arg)
{
    node_t *n = arg;
    int16_t fifo[3 * ADXL345_FIFO_DEPTH];
    esp_task_wdt_add(NULL);
    /* Without data a window is still closed on time, so the gateway keeps
     * receiving (flagged) windows instead of a silent stream. */
    const int64_t deadline_us = (int64_t)n->window_ms * (n->accel ? 1500 : 1000);
    int64_t window_start = esp_timer_get_time();
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(10));
        esp_task_wdt_reset();
        if (n->accel) {
            uint32_t got = 0;
            bool overrun = false;
            esp_err_t err = adxl345_read_fifo(n->accel, fifo, ADXL345_FIFO_DEPTH, &got, &overrun);
            taskENTER_CRITICAL(&s_lock);
            if (err == ESP_OK) sensor_health_accel_ok(&n->health);
            else               sensor_health_accel_bus_error(&n->health);
            if (overrun) sensor_health_accel_overrun(&n->health);
            taskEXIT_CRITICAL(&s_lock);
            if (overrun) sample_window_mark_overrun(&n->window, 1);
            uint32_t used = sample_window_add(&n->window, fifo, got);
            if (sample_window_full(&n->window)) {
                close_window(n);
                window_start = esp_timer_get_time();
                if (used < got) sample_window_add(&n->window, &fifo[3 * used], got - used);
            }
        }
        if (esp_timer_get_time() - window_start >= deadline_us) {
            close_window(n);
            window_start = esp_timer_get_time();
        }
    }
}

static void temp_task(void *arg)
{
    node_t *n = arg;
    for (;;) {
        if (!n->temp) {
            vTaskDelay(pdMS_TO_TICKS(30000));        /* sensor may be plugged in later */
            ds18b20_config_t tc = { .gpio = CONFIG_ALGAOPS_ONEWIRE_GPIO,
                                    .resolution_bits = CONFIG_ALGAOPS_DS18B20_RESOLUTION };
            if (ds18b20_open(&tc, &n->temp) != ESP_OK) n->temp = NULL;
            continue;
        }
        float t = 0.0f;
        ds18b20_data_status_t st = DS18B20_DATA_NO_DEVICE;
        esp_err_t err = ds18b20_start_conversion(n->temp);
        if (err == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(ds18b20_conversion_time_ms(n->temp) + 10));
            err = ds18b20_read(n->temp, &t, &st);
        }
        int64_t now = esp_timer_get_time();
        taskENTER_CRITICAL(&s_lock);
        if (err != ESP_OK) {
            sensor_health_temp_absent(&n->health);
        } else if (st == DS18B20_DATA_OK) {
            sensor_health_temp_ok(&n->health);
            n->temp_valid = true;
            n->temp_c = t;
            n->temp_mono_us = now;
        } else if (st == DS18B20_DATA_CRC) {
            sensor_health_temp_crc_error(&n->health);
        } else if (st == DS18B20_DATA_POWER_ON) {
            sensor_health_temp_power_on_value(&n->health);
        } else {
            sensor_health_temp_absent(&n->health);   /* no device, shorted bus, out of range */
        }
        taskEXIT_CRITICAL(&s_lock);
        vTaskDelay(pdMS_TO_TICKS(err == ESP_OK ? 200 : 1000));
    }
}

/* ---- publishing ------------------------------------------------------------- */

static bool publish_status(node_t *n)
{
    char buf[512];
    node_status_t st;
    memset(&st, 0, sizeof(st));
    st.state = "online";
    st.has_boot = true;
    st.boot_id = n->boot.boot_id;
    st.fw = n->fw;
    st.has_runtime = true;
    st.uptime_s = (uint32_t)(esp_timer_get_time() / 1000000LL);
    int rssi = 0;
    st.rssi_valid = wifi_link_rssi(&rssi);
    st.rssi_dbm = (int16_t)rssi;
    xSemaphoreTake(n->buffer_lock, portMAX_DELAY);
    st.queue_depth = ring_buffer_count(&n->buffer);
    st.dropped_windows = n->buffer.dropped;
    xSemaphoreGive(n->buffer_lock);
    taskENTER_CRITICAL(&s_lock);
    st.self_test = n->health.self_test;
    st.accel_ok = n->accel != NULL && sensor_health_accel_usable(&n->health);
    st.temp_ok = n->temp != NULL && sensor_health_temp_usable(&n->health);
    st.current_ok = n->current_ok;
    taskEXIT_CRITICAL(&s_lock);
    st.current_present = n->current_present;
    st.reset_reason = n->boot.reset_reason;
    time_base_t tb;
    time_sync_get_base(&tb);
    st.ts_ms = tb.synced ? time_base_to_ms(&tb, esp_timer_get_time()) : -1;
    int len = status_payload_build(buf, sizeof(buf), n->node_id, &st);
    return len > 0 && mqtt_link_publish(n->topic_status, buf, len, 1, true) >= 0;
}

static void publish_task(void *arg)
{
    node_t *n = arg;
    static char payload[TELEMETRY_PAYLOAD_MAX];
    telemetry_record_t head;
    bool cached = false;              /* payload holds the serialised head window */
    uint32_t cached_seq = 0;
    int cached_len = 0;
    uint32_t epoch_announced = 0;
    int64_t connected_since = 0;
    int64_t last_status = 0;
    uint32_t backoff_ms = 0;
    const int64_t status_every_us = (int64_t)CONFIG_ALGAOPS_STATUS_INTERVAL_S * 1000000LL;
    const int64_t sync_wait_us = (int64_t)CONFIG_ALGAOPS_SYNC_WAIT_S * 1000000LL;

    for (;;) {
        if (!mqtt_link_connected()) {
            connected_since = 0;
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        int64_t now = esp_timer_get_time();
        if (connected_since == 0) connected_since = now;
        uint32_t epoch = mqtt_link_epoch();
        if ((epoch != epoch_announced || now - last_status >= status_every_us) &&
            publish_status(n)) {
            epoch_announced = epoch;
            last_status = now;
        }
        time_base_t tb;
        time_sync_get_base(&tb);
        if (!tb.synced && now - connected_since < sync_wait_us) {
            vTaskDelay(pdMS_TO_TICKS(250));        /* buffered windows wait for the clock */
            continue;
        }

        xSemaphoreTake(n->buffer_lock, portMAX_DELAY);
        bool have = ring_buffer_peek(&n->buffer, &head);
        xSemaphoreGive(n->buffer_lock);
        if (!have) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (!cached || head.seq != cached_seq) {
            /* Serialised once: retries resend identical bytes, which the gateway
             * drops as an exact redelivery instead of a conflicting duplicate. */
            cached_len = telemetry_payload_build(payload, sizeof(payload), n->node_id, n->fw,
                                                 &head, &tb);
            cached_seq = head.seq;
            cached = cached_len > 0;
            if (!cached) {
                ESP_LOGE(TAG, "window %lu could not be serialised: discarded",
                         (unsigned long)head.seq);
                xSemaphoreTake(n->buffer_lock, portMAX_DELAY);
                ring_buffer_pop(&n->buffer, NULL);
                xSemaphoreGive(n->buffer_lock);
                continue;
            }
        }
        int mid = mqtt_link_publish(n->topic_telemetry, payload, cached_len, 1, false);
        if (mid >= 0 && mqtt_link_wait_ack(mid, ACK_TIMEOUT_MS)) {
            telemetry_record_t cur;
            xSemaphoreTake(n->buffer_lock, portMAX_DELAY);
            /* Pop only if the head was not dropped by an overflow meanwhile. */
            if (ring_buffer_peek(&n->buffer, &cur) && cur.seq == cached_seq) {
                ring_buffer_pop(&n->buffer, NULL);
            }
            xSemaphoreGive(n->buffer_lock);
            cached = false;
            backoff_ms = 0;
        } else {
            backoff_ms = backoff_ms ? backoff_ms * 2 : 500;
            if (backoff_ms > BACKOFF_MAX_MS) backoff_ms = BACKOFF_MAX_MS;
            ESP_LOGW(TAG, "window %lu not acknowledged; retry in %lu ms",
                     (unsigned long)cached_seq, (unsigned long)backoff_ms);
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
        }
    }
}

/* ---- start ------------------------------------------------------------------ */

void node_app_start(void)
{
    node_t *n = &s_node;
    n->fw = esp_app_get_description()->version;
    n->rate_hz = CONFIG_ALGAOPS_ACCEL_RATE_HZ;
    n->window_ms = CONFIG_ALGAOPS_WINDOW_MS;
    init_identity(n);
    sensor_health_init(&n->health);
    sequence_counter_init(&n->seq);

    uint32_t samples = n->rate_hz * n->window_ms / 1000u;
    int16_t *xyz = malloc((size_t)samples * 3u * sizeof(int16_t));
    void *records = malloc((size_t)CONFIG_ALGAOPS_BUFFER_WINDOWS * sizeof(telemetry_record_t));
    n->buffer_lock = xSemaphoreCreateMutex();
    if (!xyz || !records || !n->buffer_lock || !sample_window_init(&n->window, xyz, samples) ||
        !ring_buffer_init(&n->buffer, records, sizeof(telemetry_record_t),
                          CONFIG_ALGAOPS_BUFFER_WINDOWS)) {
        ESP_LOGE(TAG, "cannot allocate %lu samples x %d windows: check the configuration",
                 (unsigned long)samples, CONFIG_ALGAOPS_BUFFER_WINDOWS);
        abort();
    }

    /* Radio first: esp_random() is a true RNG once Wi-Fi runs (boot id). */
    esp_err_t wifi = wifi_link_start(CONFIG_ALGAOPS_WIFI_SSID, CONFIG_ALGAOPS_WIFI_PASSWORD);
    boot_identity_init(&n->boot);
    time_sync_start(CONFIG_ALGAOPS_SNTP_SERVER);
    init_sensors(n);

    node_status_t lwt;
    memset(&lwt, 0, sizeof(lwt));
    lwt.state = "offline";
    lwt.has_boot = true;
    lwt.boot_id = n->boot.boot_id;
    lwt.fw = n->fw;
    lwt.ts_ms = -1;
    if (status_payload_build(n->lwt, sizeof(n->lwt), n->node_id, &lwt) < 0) n->lwt[0] = '\0';
    if (wifi == ESP_OK) {
        mqtt_link_config_t mc = {
            .uri = CONFIG_ALGAOPS_MQTT_URI,
            .client_id = n->node_id,
            .username = CONFIG_ALGAOPS_MQTT_USERNAME,
            .password = CONFIG_ALGAOPS_MQTT_PASSWORD,
            .lwt_topic = n->lwt[0] ? n->topic_status : NULL,
            .lwt_payload = n->lwt[0] ? n->lwt : NULL,
        };
        if (mqtt_link_start(&mc) != ESP_OK) ESP_LOGE(TAG, "MQTT client not started");
    } else {
        ESP_LOGE(TAG, "Wi-Fi not started: windows are only buffered");
    }

    ESP_LOGI(TAG, "node %s, fw %s, %lu Hz x %lu ms = %lu samples per window", n->node_id, n->fw,
             (unsigned long)n->rate_hz, (unsigned long)n->window_ms, (unsigned long)samples);
    xTaskCreatePinnedToCore(accel_task, "accel", 6144, n, 10, NULL, 1);
    xTaskCreatePinnedToCore(temp_task, "temp", 3072, n, 6, NULL, 1);
    xTaskCreatePinnedToCore(publish_task, "publish", 6144, n, 4, NULL, 0);
}
