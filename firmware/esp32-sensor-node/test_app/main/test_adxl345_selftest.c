/*
 * ADXL345 on real hardware: identity, data-sheet self-test, data rate and the
 * gravity reading.  Run with the node mounted but the pump stopped (the
 * gravity test needs the board at rest).  Pins: GPIO 21 SDA, GPIO 22 SCL,
 * SDO low (address 0x53) unless overridden at build time.
 */

#include <math.h>

#include "adxl345.h"
#include "adxl345_convert.h"
#include "adxl345_regs.h"
#include "driver/i2c_master.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "unity.h"

#ifndef TEST_I2C_SDA_GPIO
#define TEST_I2C_SDA_GPIO 21
#endif
#ifndef TEST_I2C_SCL_GPIO
#define TEST_I2C_SCL_GPIO 22
#endif
#ifndef TEST_ADXL345_ADDR
#define TEST_ADXL345_ADDR ADXL345_I2C_ADDR_SDO_LOW
#endif
#ifndef TEST_SUPPLY_V
#define TEST_SUPPLY_V 3.3
#endif

static i2c_master_bus_handle_t s_bus;

static adxl345_handle_t open_accel(uint32_t rate_hz)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,
        .sda_io_num = TEST_I2C_SDA_GPIO,
        .scl_io_num = TEST_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    TEST_ASSERT_EQUAL(ESP_OK, i2c_new_master_bus(&bus_cfg, &s_bus));
    adxl345_config_t cfg = { .bus = s_bus, .address = TEST_ADXL345_ADDR, .scl_hz = 400000,
                             .rate_hz = rate_hz, .range_g = 8 };
    adxl345_handle_t dev = NULL;
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, adxl345_open(&cfg, &dev), "ADXL345 not found: check wiring");
    return dev;
}

static void close_accel(adxl345_handle_t dev)
{
    adxl345_close(dev);
    TEST_ASSERT_EQUAL(ESP_OK, i2c_del_master_bus(s_bus));
}

TEST_CASE("ADXL345 answers with DEVID 0xE5", "[adxl345][hw]")
{
    adxl345_handle_t dev = open_accel(800);   /* adxl345_open checks DEVID */
    close_accel(dev);
}

TEST_CASE("ADXL345 data-sheet self-test passes", "[adxl345][hw]")
{
    adxl345_handle_t dev = open_accel(800);
    adxl345_self_test_result_t r;
    TEST_ASSERT_EQUAL(ESP_OK, adxl345_self_test(dev, TEST_SUPPLY_V, &r));
    printf("self-test change: X %.0f, Y %.0f, Z %.0f LSB\n", r.delta_lsb[0], r.delta_lsb[1],
           r.delta_lsb[2]);
    TEST_ASSERT_TRUE_MESSAGE(r.passed, "self-test response outside the data-sheet limits");
    close_accel(dev);
}

TEST_CASE("ADXL345 delivers 800 Hz through the FIFO without overrun", "[adxl345][hw]")
{
    adxl345_handle_t dev = open_accel(800);
    int16_t buf[3 * ADXL345_FIFO_DEPTH];
    uint32_t total = 0;
    bool any_overrun = false;
    vTaskDelay(pdMS_TO_TICKS(20));
    uint32_t n = 0;
    bool ovr = false;
    adxl345_read_fifo(dev, buf, ADXL345_FIFO_DEPTH, &n, &ovr);     /* start from empty */
    int64_t t0 = esp_timer_get_time();
    TickType_t last = xTaskGetTickCount();
    while (esp_timer_get_time() - t0 < 2000000) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(10));
        TEST_ASSERT_EQUAL(ESP_OK, adxl345_read_fifo(dev, buf, ADXL345_FIFO_DEPTH, &n, &ovr));
        total += n;
        any_overrun = any_overrun || ovr;
    }
    printf("samples in 2 s: %lu\n", (unsigned long)total);
    TEST_ASSERT_FALSE(any_overrun);
    TEST_ASSERT_UINT32_WITHIN(160, 1600, total);    /* output data rate within ±10 % */
    close_accel(dev);
}

TEST_CASE("ADXL345 at rest measures about 1 g", "[adxl345][hw]")
{
    adxl345_handle_t dev = open_accel(100);
    int16_t buf[3 * ADXL345_FIFO_DEPTH];
    double sum[3] = {0, 0, 0};
    uint32_t got = 0;
    while (got < 200) {
        vTaskDelay(pdMS_TO_TICKS(50));
        uint32_t n = 0;
        bool ovr = false;
        TEST_ASSERT_EQUAL(ESP_OK, adxl345_read_fifo(dev, buf, ADXL345_FIFO_DEPTH, &n, &ovr));
        for (uint32_t i = 0; i < n; i++) {
            for (int a = 0; a < 3; a++) sum[a] += buf[3 * i + a];
        }
        got += n;
    }
    double g = ADXL345_G_PER_LSB_FULL_RES / got;
    double mag = sqrt(pow(sum[0] * g, 2) + pow(sum[1] * g, 2) + pow(sum[2] * g, 2));
    printf("|g| = %.3f g\n", mag);
    TEST_ASSERT_DOUBLE_WITHIN(0.15, 1.0, mag);
    close_accel(dev);
}
