/* wifi_link.c — see wifi_link.h */

#include "wifi_link.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"

static const char *TAG = "wifi";

#define BACKOFF_MAX_MS 60000ULL

static volatile bool       s_connected;
static uint32_t            s_attempt;
static uint32_t            s_reconnects;
static esp_timer_handle_t  s_retry;

static void retry_cb(void *arg)
{
    (void)arg;
    esp_wifi_connect();
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_connected) s_reconnects++;
        s_connected = false;
        uint32_t shift = s_attempt < 6 ? s_attempt : 6;
        uint64_t delay_ms = 1000ULL << shift;
        if (delay_ms > BACKOFF_MAX_MS) delay_ms = BACKOFF_MAX_MS;
        s_attempt++;
        const wifi_event_sta_disconnected_t *ev = data;
        ESP_LOGW(TAG, "disconnected (reason %d), retry in %llu ms", ev ? (int)ev->reason : -1,
                 (unsigned long long)delay_ms);
        esp_timer_stop(s_retry);                 /* not running: harmless error */
        esp_timer_start_once(s_retry, delay_ms * 1000ULL);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = data;
        ESP_LOGI(TAG, "connected, address " IPSTR, IP2STR(&ev->ip_info.ip));
        s_connected = true;
        s_attempt = 0;
    }
}

esp_err_t wifi_link_start(const char *ssid, const char *password)
{
    ESP_RETURN_ON_FALSE(ssid && ssid[0], ESP_ERR_INVALID_ARG, TAG,
                        "Wi-Fi SSID not configured (idf.py menuconfig -> AlgaOps sensor node)");
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "esp_wifi_init");
    const esp_timer_create_args_t targs = { .callback = retry_cb, .name = "wifi_retry" };
    ESP_RETURN_ON_ERROR(esp_timer_create(&targs, &s_retry), TAG, "retry timer");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                            on_event, NULL, NULL), TAG, "events");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                            on_event, NULL, NULL), TAG, "events");
    wifi_config_t wc;
    memset(&wc, 0, sizeof(wc));
    snprintf((char *)wc.sta.ssid, sizeof(wc.sta.ssid), "%s", ssid);
    snprintf((char *)wc.sta.password, sizeof(wc.sta.password), "%s", password ? password : "");
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;   /* refuse open / WEP networks */
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wc), TAG, "config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");
    return ESP_OK;
}

bool wifi_link_connected(void) { return s_connected; }

uint32_t wifi_link_reconnects(void) { return s_reconnects; }

bool wifi_link_rssi(int *rssi_dbm)
{
    if (!s_connected || !rssi_dbm) return false;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return false;
    *rssi_dbm = ap.rssi;
    return true;
}
