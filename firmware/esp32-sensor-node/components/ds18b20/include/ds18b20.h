/*
 * ds18b20.h — DS18B20 temperature sensor on Espressif's RMT-based 1-Wire bus
 * component (espressif/onewire_bus, see idf_component.yml).
 *
 * Wiring: external 3.3 V supply on VDD (not parasite power) and a 4.7 kΩ
 * pull-up from DQ to 3.3 V.  The first DS18B20 found on the bus is used and
 * addressed by its ROM code; conversions are started and read separately so
 * the sampling task never blocks for the 750 ms conversion.
 */

#ifndef DS18B20_H
#define DS18B20_H

#include <stdint.h>

#include "esp_err.h"
#include "ds18b20_convert.h"

typedef struct {
    int gpio;                 /* 1-Wire data line                 */
    int resolution_bits;      /* 9..12; 12 bit = 0.0625 °C steps  */
} ds18b20_config_t;

typedef struct ds18b20_dev *ds18b20_handle_t;

/** Create the bus, find a DS18B20 and set its resolution.
 *  ESP_ERR_NOT_FOUND: no DS18B20 answered. */
esp_err_t ds18b20_open(const ds18b20_config_t *cfg, ds18b20_handle_t *out);
void      ds18b20_close(ds18b20_handle_t dev);

/** Start a conversion (CONVERT T); read the result after ds18b20_conversion_ms(). */
esp_err_t ds18b20_start_conversion(ds18b20_handle_t dev);
uint32_t  ds18b20_conversion_time_ms(ds18b20_handle_t dev);

/** Read and validate the scratchpad.  ESP_OK with *status telling whether
 *  *temp_c is usable; ESP_ERR_NOT_FOUND when no presence pulse answered. */
esp_err_t ds18b20_read(ds18b20_handle_t dev, float *temp_c, ds18b20_data_status_t *status);

#endif /* DS18B20_H */
