/* ds18b20.c — DS18B20 over espressif/onewire_bus; see ds18b20.h */

#include "ds18b20.h"

#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "onewire_bus.h"
#include "onewire_device.h"

static const char *TAG = "ds18b20";

struct ds18b20_dev {
    onewire_bus_handle_t bus;
    uint8_t              rom[8];        /* family code first (LSB of the address) */
    int                  bits;
    bool                 has_previous;
    float                previous_c;
};

/* Reset + MATCH ROM: only our sensor listens to the following command. */
static esp_err_t select_device(struct ds18b20_dev *d)
{
    esp_err_t err = onewire_bus_reset(d->bus);   /* ESP_ERR_NOT_FOUND: no presence pulse */
    if (err != ESP_OK) return err;
    uint8_t cmd[9];
    cmd[0] = DS18B20_CMD_MATCH_ROM;
    memcpy(&cmd[1], d->rom, sizeof(d->rom));
    return onewire_bus_write_bytes(d->bus, cmd, sizeof(cmd));
}

static esp_err_t find_sensor(struct ds18b20_dev *d)
{
    onewire_device_iter_handle_t iter = NULL;
    ESP_RETURN_ON_ERROR(onewire_new_device_iter(d->bus, &iter), TAG, "device iterator");
    onewire_device_t dev;
    esp_err_t found = ESP_ERR_NOT_FOUND;
    while (onewire_device_iter_get_next(iter, &dev) == ESP_OK) {
        if ((dev.address & 0xFF) == DS18B20_FAMILY_CODE) {
            for (int i = 0; i < 8; i++) d->rom[i] = (uint8_t)(dev.address >> (8 * i));
            found = ESP_OK;
            break;
        }
    }
    onewire_del_device_iter(iter);
    return found;
}

esp_err_t ds18b20_open(const ds18b20_config_t *cfg, ds18b20_handle_t *out)
{
    ESP_RETURN_ON_FALSE(cfg && out, ESP_ERR_INVALID_ARG, TAG, "invalid arguments");
    uint8_t config = 0;
    ESP_RETURN_ON_FALSE(ds18b20_config_byte(cfg->resolution_bits, &config), ESP_ERR_INVALID_ARG,
                        TAG, "resolution must be 9..12 bit");
    struct ds18b20_dev *d = calloc(1, sizeof(*d));
    ESP_RETURN_ON_FALSE(d, ESP_ERR_NO_MEM, TAG, "no memory");
    d->bits = cfg->resolution_bits;

    onewire_bus_config_t bus_cfg = { .bus_gpio_num = cfg->gpio };
    onewire_bus_rmt_config_t rmt_cfg = { .max_rx_bytes = 10 };  /* ROM cmd + ROM + 1 */
    esp_err_t err = onewire_new_bus_rmt(&bus_cfg, &rmt_cfg, &d->bus);
    if (err != ESP_OK) {
        free(d);
        return err;
    }
    err = find_sensor(d);
    if (err == ESP_OK) err = select_device(d);
    if (err == ESP_OK) {
        uint8_t w[4] = { DS18B20_CMD_WRITE_SCRATCHPAD, DS18B20_ALARM_TH_DEFAULT,
                         DS18B20_ALARM_TL_DEFAULT, config };
        err = onewire_bus_write_bytes(d->bus, w, sizeof(w));
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no DS18B20 on GPIO %d (%s)", cfg->gpio, esp_err_to_name(err));
        onewire_bus_del(d->bus);
        free(d);
        return err == ESP_ERR_NOT_FOUND ? ESP_ERR_NOT_FOUND : err;
    }
    ESP_LOGI(TAG, "DS18B20 %02x%02x%02x%02x%02x%02x%02x%02x, %d bit", d->rom[7], d->rom[6],
             d->rom[5], d->rom[4], d->rom[3], d->rom[2], d->rom[1], d->rom[0], d->bits);
    *out = d;
    return ESP_OK;
}

void ds18b20_close(ds18b20_handle_t d)
{
    if (!d) return;
    onewire_bus_del(d->bus);
    free(d);
}

esp_err_t ds18b20_start_conversion(ds18b20_handle_t d)
{
    ESP_RETURN_ON_FALSE(d, ESP_ERR_INVALID_ARG, TAG, "no device");
    esp_err_t err = select_device(d);
    if (err != ESP_OK) return err;
    uint8_t cmd = DS18B20_CMD_CONVERT_T;
    return onewire_bus_write_bytes(d->bus, &cmd, 1);
}

uint32_t ds18b20_conversion_time_ms(ds18b20_handle_t d)
{
    return ds18b20_conversion_ms(d ? d->bits : 12);
}

esp_err_t ds18b20_read(ds18b20_handle_t d, float *temp_c, ds18b20_data_status_t *status)
{
    ESP_RETURN_ON_FALSE(d && temp_c && status, ESP_ERR_INVALID_ARG, TAG, "args");
    esp_err_t err = select_device(d);
    if (err != ESP_OK) return err;
    uint8_t cmd = DS18B20_CMD_READ_SCRATCHPAD;
    ESP_RETURN_ON_ERROR(onewire_bus_write_bytes(d->bus, &cmd, 1), TAG, "READ SCRATCHPAD");
    uint8_t sp[DS18B20_SCRATCHPAD_LEN];
    ESP_RETURN_ON_ERROR(onewire_bus_read_bytes(d->bus, sp, sizeof(sp)), TAG, "scratchpad");
    int bits = 0;
    float t = 0.0f;
    *status = ds18b20_decode(sp, d->has_previous ? &d->previous_c : NULL, &t, &bits);
    if (*status == DS18B20_DATA_OK) {
        *temp_c = t;
        d->has_previous = true;
        d->previous_c = t;
        if (bits != d->bits) {
            /* The sensor lost its configuration (brown-out): restore it. */
            ESP_LOGW(TAG, "resolution %d bit, expected %d: rewriting", bits, d->bits);
            uint8_t config = 0;
            ds18b20_config_byte(d->bits, &config);
            if (select_device(d) == ESP_OK) {
                uint8_t w[4] = { DS18B20_CMD_WRITE_SCRATCHPAD, DS18B20_ALARM_TH_DEFAULT,
                                 DS18B20_ALARM_TL_DEFAULT, config };
                (void)onewire_bus_write_bytes(d->bus, w, sizeof(w));
            }
        }
    }
    return ESP_OK;
}
