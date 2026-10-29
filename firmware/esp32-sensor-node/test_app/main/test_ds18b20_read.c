/*
 * DS18B20 on real hardware: presence, CRC-checked scratchpad and plausible,
 * stable bench readings.  Pin: GPIO 4 with a 4.7 kOhm pull-up to 3.3 V.
 */

#include <math.h>

#include "ds18b20.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "unity.h"

#ifndef TEST_ONEWIRE_GPIO
#define TEST_ONEWIRE_GPIO 4
#endif

static float read_once(ds18b20_handle_t dev)
{
    TEST_ASSERT_EQUAL(ESP_OK, ds18b20_start_conversion(dev));
    vTaskDelay(pdMS_TO_TICKS(ds18b20_conversion_time_ms(dev) + 10));
    float t = NAN;
    ds18b20_data_status_t st = DS18B20_DATA_NO_DEVICE;
    TEST_ASSERT_EQUAL(ESP_OK, ds18b20_read(dev, &t, &st));
    TEST_ASSERT_EQUAL_MESSAGE(DS18B20_DATA_OK, st, ds18b20_data_status_str(st));
    return t;
}

TEST_CASE("DS18B20 answers and reads a plausible bench temperature", "[ds18b20][hw]")
{
    ds18b20_config_t cfg = { .gpio = TEST_ONEWIRE_GPIO, .resolution_bits = 12 };
    ds18b20_handle_t dev = NULL;
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, ds18b20_open(&cfg, &dev), "no DS18B20: check wiring/pull-up");
    float t = read_once(dev);
    printf("temperature %.4f C\n", t);
    TEST_ASSERT_FLOAT_WITHIN(25.0f, 30.0f, t);       /* 5..55 °C on a bench */
    ds18b20_close(dev);
}

TEST_CASE("DS18B20 consecutive readings agree within 0.5 C", "[ds18b20][hw]")
{
    ds18b20_config_t cfg = { .gpio = TEST_ONEWIRE_GPIO, .resolution_bits = 12 };
    ds18b20_handle_t dev = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, ds18b20_open(&cfg, &dev));
    float first = read_once(dev);
    for (int i = 0; i < 4; i++) {
        float t = read_once(dev);
        TEST_ASSERT_FLOAT_WITHIN(0.5f, first, t);
    }
    ds18b20_close(dev);
}
