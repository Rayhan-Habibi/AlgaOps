/* boot_identity.c — see boot_identity.h */

#include "boot_identity.h"

#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "nvs.h"

static const char *TAG = "boot";

static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
        case ESP_RST_POWERON:   return "power_on";
        case ESP_RST_EXT:       return "external_pin";
        case ESP_RST_SW:        return "software";
        case ESP_RST_PANIC:     return "panic";
        case ESP_RST_INT_WDT:   return "interrupt_watchdog";
        case ESP_RST_TASK_WDT:  return "task_watchdog";
        case ESP_RST_WDT:       return "other_watchdog";
        case ESP_RST_DEEPSLEEP: return "deep_sleep";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_SDIO:      return "sdio";
        default:                return "unknown";
    }
}

esp_err_t boot_identity_init(boot_identity_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    uint32_t count = 0;
    nvs_handle_t h;
    esp_err_t err = nvs_open("algaops", NVS_READWRITE, &h);
    if (err == ESP_OK) {
        if (nvs_get_u32(h, "boots", &count) != ESP_OK) count = 0;
        count++;
        err = nvs_set_u32(h, "boots", count);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "boot counter not persisted (%s)", esp_err_to_name(err));
    }
    out->boot_count = count;
    out->boot_id = esp_random() ^ (count * 0x9E3779B1u);
    out->reset_reason = reset_reason_str(esp_reset_reason());
    ESP_LOGI(TAG, "boot #%lu, boot_id %lu, reset reason %s", (unsigned long)count,
             (unsigned long)out->boot_id, out->reset_reason);
    return ESP_OK;
}
