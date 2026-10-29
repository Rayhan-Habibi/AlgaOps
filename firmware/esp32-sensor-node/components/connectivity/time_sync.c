/* time_sync.c — see time_sync.h */

#include "time_sync.h"

#include <sys/time.h>

#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "time";

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool         s_synced;
static int64_t      s_offset_us;

static void on_sync(struct timeval *tv)
{
    int64_t mono = esp_timer_get_time();
    int64_t unix_us = (int64_t)tv->tv_sec * 1000000LL + (int64_t)tv->tv_usec;
    taskENTER_CRITICAL(&s_lock);
    s_offset_us = unix_us - mono;
    s_synced = true;
    taskEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "clock set by SNTP (Unix %lld s)", (long long)tv->tv_sec);
}

void time_sync_start(const char *server)
{
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, server);
    sntp_set_time_sync_notification_cb(on_sync);
    esp_sntp_init();
    ESP_LOGI(TAG, "SNTP server %s", server);
}

void time_sync_get_base(time_base_t *out)
{
    if (!out) return;
    taskENTER_CRITICAL(&s_lock);
    out->synced = s_synced;
    out->unix_minus_mono_us = s_offset_us;
    taskEXIT_CRITICAL(&s_lock);
}

bool time_sync_is_synced(void)
{
    taskENTER_CRITICAL(&s_lock);
    bool s = s_synced;
    taskEXIT_CRITICAL(&s_lock);
    return s;
}
