/*
 * adxl345.h — ADXL345 accelerometer on the ESP-IDF I2C master driver
 * (driver/i2c_master.h, ESP-IDF >= 5.2).
 *
 * The part runs in stream FIFO mode at the configured output data rate; the
 * sampling task drains the 32-entry FIFO every few milliseconds.  Readings
 * are raw counts in full resolution (ADXL345_G_PER_LSB_FULL_RES g/LSB).
 */

#ifndef ADXL345_H
#define ADXL345_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "driver/i2c_master.h"

typedef struct {
    i2c_master_bus_handle_t bus;     /* shared I2C master bus            */
    uint8_t  address;                /* ADXL345_I2C_ADDR_SDO_LOW / _HIGH */
    uint32_t scl_hz;                 /* 100000 or 400000                 */
    uint32_t rate_hz;                /* 25 .. 3200 (see adxl345_rate_code) */
    uint8_t  range_g;                /* 2, 4, 8 or 16                    */
} adxl345_config_t;

typedef struct adxl345_dev *adxl345_handle_t;

typedef struct {
    bool   passed;
    double delta_lsb[3];             /* ST on minus ST off, per axis     */
    double supply_v;
} adxl345_self_test_result_t;

/** Probe, check DEVID and start measuring.  ESP_ERR_NOT_FOUND: no ADXL345. */
esp_err_t adxl345_open(const adxl345_config_t *cfg, adxl345_handle_t *out);
void      adxl345_close(adxl345_handle_t dev);

/** Drain up to @p max entries from the FIFO into @p xyz (3 counts per sample).
 *  @p overrun is set when the part reported lost samples since the last call. */
esp_err_t adxl345_read_fifo(adxl345_handle_t dev, int16_t *xyz, uint32_t max,
                            uint32_t *n_read, bool *overrun);

/** Data sheet self-test; reconfigures the part temporarily (~0.3 s at 800 Hz)
 *  and restores the measurement configuration.  Run it while the node is
 *  mounted but before relying on the data. */
esp_err_t adxl345_self_test(adxl345_handle_t dev, double supply_v,
                            adxl345_self_test_result_t *result);

#endif /* ADXL345_H */
