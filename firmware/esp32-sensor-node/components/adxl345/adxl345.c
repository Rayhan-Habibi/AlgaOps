/* adxl345.c — ADXL345 on the ESP-IDF I2C master driver; see adxl345.h */

#include "adxl345.h"

#include <stdlib.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "adxl345_convert.h"
#include "adxl345_regs.h"

static const char *TAG = "adxl345";

#define XFER_TIMEOUT_MS     20
#define FIFO_WATERMARK      16
#define SELF_TEST_SAMPLES   100

struct adxl345_dev {
    i2c_master_dev_handle_t i2c;
    adxl345_config_t        cfg;
    uint8_t                 rate_code;
    uint8_t                 format;      /* measurement DATA_FORMAT */
};

static esp_err_t wr(struct adxl345_dev *d, uint8_t reg, uint8_t val)
{
    uint8_t b[2] = { reg, val };
    return i2c_master_transmit(d->i2c, b, sizeof(b), XFER_TIMEOUT_MS);
}

static esp_err_t rd(struct adxl345_dev *d, uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_master_transmit_receive(d->i2c, &reg, 1, buf, n, XFER_TIMEOUT_MS);
}

/* Standby, rate, format, polled operation, FIFO reset to stream mode, measure. */
static esp_err_t configure(struct adxl345_dev *d, uint8_t format)
{
    ESP_RETURN_ON_ERROR(wr(d, ADXL345_REG_POWER_CTL, 0x00), TAG, "standby");
    ESP_RETURN_ON_ERROR(wr(d, ADXL345_REG_BW_RATE, d->rate_code), TAG, "BW_RATE");
    ESP_RETURN_ON_ERROR(wr(d, ADXL345_REG_DATA_FORMAT, format), TAG, "DATA_FORMAT");
    ESP_RETURN_ON_ERROR(wr(d, ADXL345_REG_INT_ENABLE, 0x00), TAG, "INT_ENABLE");
    ESP_RETURN_ON_ERROR(wr(d, ADXL345_REG_FIFO_CTL, ADXL345_FIFO_MODE_BYPASS), TAG, "FIFO reset");
    ESP_RETURN_ON_ERROR(wr(d, ADXL345_REG_FIFO_CTL, adxl345_fifo_ctl_stream(FIFO_WATERMARK)),
                        TAG, "FIFO_CTL");
    return wr(d, ADXL345_REG_POWER_CTL, ADXL345_POWER_CTL_MEASURE);
}

esp_err_t adxl345_open(const adxl345_config_t *cfg, adxl345_handle_t *out)
{
    ESP_RETURN_ON_FALSE(cfg && out && cfg->bus, ESP_ERR_INVALID_ARG, TAG, "invalid arguments");
    struct adxl345_dev *d = calloc(1, sizeof(*d));
    ESP_RETURN_ON_FALSE(d, ESP_ERR_NO_MEM, TAG, "no memory");
    d->cfg = *cfg;
    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = cfg->address,
        .scl_speed_hz    = cfg->scl_hz,
    };
    uint8_t id = 0;
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (!adxl345_rate_code(cfg->rate_hz, &d->rate_code) ||
        !adxl345_data_format(cfg->range_g, true, false, &d->format)) {
        ESP_LOGE(TAG, "unsupported rate %lu Hz or range %u g", (unsigned long)cfg->rate_hz,
                 cfg->range_g);
        goto fail;
    }
    if (i2c_master_probe(cfg->bus, cfg->address, XFER_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGE(TAG, "no device answers at 0x%02x", cfg->address);
        err = ESP_ERR_NOT_FOUND;
        goto fail;
    }
    err = i2c_master_bus_add_device(cfg->bus, &dc, &d->i2c);
    if (err != ESP_OK) goto fail;
    err = rd(d, ADXL345_REG_DEVID, &id, 1);
    if (err == ESP_OK && id != ADXL345_DEVID_VALUE) {
        ESP_LOGE(TAG, "DEVID 0x%02x (expected 0x%02x): not an ADXL345", id, ADXL345_DEVID_VALUE);
        err = ESP_ERR_NOT_FOUND;
    }
    if (err == ESP_OK) err = configure(d, d->format);
    if (err != ESP_OK) {
        i2c_master_bus_rm_device(d->i2c);
        goto fail;
    }
    ESP_LOGI(TAG, "ADXL345 at 0x%02x: %lu Hz, ±%u g, full resolution", cfg->address,
             (unsigned long)cfg->rate_hz, cfg->range_g);
    *out = d;
    return ESP_OK;
fail:
    free(d);
    return err;
}

void adxl345_close(adxl345_handle_t d)
{
    if (!d) return;
    (void)wr(d, ADXL345_REG_POWER_CTL, 0x00);
    i2c_master_bus_rm_device(d->i2c);
    free(d);
}

esp_err_t adxl345_read_fifo(adxl345_handle_t d, int16_t *xyz, uint32_t max,
                            uint32_t *n_read, bool *overrun)
{
    ESP_RETURN_ON_FALSE(d && xyz && n_read && overrun, ESP_ERR_INVALID_ARG, TAG, "args");
    *n_read = 0;
    *overrun = false;
    uint8_t src = 0;
    uint8_t status = 0;
    ESP_RETURN_ON_ERROR(rd(d, ADXL345_REG_INT_SOURCE, &src, 1), TAG, "INT_SOURCE");
    ESP_RETURN_ON_ERROR(rd(d, ADXL345_REG_FIFO_STATUS, &status, 1), TAG, "FIFO_STATUS");
    /* In stream mode OVERRUN means the FIFO filled up: older samples may have
     * been replaced, so the window is reported as incomplete. */
    *overrun = (src & ADXL345_INT_OVERRUN) != 0;
    uint32_t n = adxl345_fifo_entries(status);
    if (n > max) n = max;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t raw[6];
        esp_err_t err = rd(d, ADXL345_REG_DATAX0, raw, sizeof(raw));  /* pops one entry */
        if (err != ESP_OK) return err;    /* samples read so far stay valid */
        adxl345_decode_xyz(raw, &xyz[3 * i]);
        *n_read = i + 1;
    }
    return ESP_OK;
}

static esp_err_t drain(struct adxl345_dev *d)
{
    int16_t buf[3 * ADXL345_FIFO_DEPTH];
    uint32_t n = 0;
    bool ovr = false;
    return adxl345_read_fifo(d, buf, ADXL345_FIFO_DEPTH, &n, &ovr);
}

static esp_err_t mean_of(struct adxl345_dev *d, uint32_t count, double mean[3])
{
    int16_t buf[3 * ADXL345_FIFO_DEPTH];
    int64_t sum[3] = {0, 0, 0};
    uint32_t got = 0;
    for (int guard = 0; got < count && guard < 500; guard++) {
        vTaskDelay(pdMS_TO_TICKS(10));
        uint32_t n = 0;
        bool ovr = false;
        ESP_RETURN_ON_ERROR(adxl345_read_fifo(d, buf, ADXL345_FIFO_DEPTH, &n, &ovr), TAG, "FIFO");
        for (uint32_t i = 0; i < n && got < count; i++, got++) {
            for (int a = 0; a < 3; a++) sum[a] += buf[3 * i + a];
        }
    }
    if (got < count) return ESP_ERR_TIMEOUT;
    for (int a = 0; a < 3; a++) mean[a] = (double)sum[a] / (double)count;
    return ESP_OK;
}

esp_err_t adxl345_self_test(adxl345_handle_t d, double supply_v,
                            adxl345_self_test_result_t *result)
{
    ESP_RETURN_ON_FALSE(d && result, ESP_ERR_INVALID_ARG, TAG, "args");
    uint8_t fmt_off = 0;
    uint8_t fmt_on = 0;
    adxl345_data_format(16, true, false, &fmt_off);
    adxl345_data_format(16, true, true, &fmt_on);
    double off[3] = {0, 0, 0};
    double on[3] = {0, 0, 0};

    esp_err_t err = configure(d, fmt_off);
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(50));            /* settle, then discard */
        err = drain(d);
    }
    if (err == ESP_OK) err = mean_of(d, SELF_TEST_SAMPLES, off);
    if (err == ESP_OK) err = wr(d, ADXL345_REG_DATA_FORMAT, fmt_on);
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(50));            /* self-test force settles */
        err = drain(d);
    }
    if (err == ESP_OK) err = mean_of(d, SELF_TEST_SAMPLES, on);
    esp_err_t restore = configure(d, d->format);  /* self-test bit cleared again */
    if (err != ESP_OK) return err;
    if (restore != ESP_OK) return restore;

    result->supply_v = supply_v;
    result->passed = adxl345_self_test_evaluate(off, on, supply_v, result->delta_lsb);
    ESP_LOGI(TAG, "self-test %s: change %.0f / %.0f / %.0f LSB (VS %.2f V)",
             result->passed ? "PASS" : "FAIL", result->delta_lsb[0], result->delta_lsb[1],
             result->delta_lsb[2], supply_v);
    return ESP_OK;
}
