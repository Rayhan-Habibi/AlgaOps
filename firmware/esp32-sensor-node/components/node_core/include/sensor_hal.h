/*
 * sensor_hal.h — plain-C contract between the ESP-IDF sensor drivers and the
 * host-testable node logic (no ESP-IDF types in this header).
 *
 * The fault bits and their names are the node side of the telemetry contract
 * (schemas/telemetry_window.v1.schema.json, "faults").  The gateway maps them
 * to ALGA_NODE_FAULT_* and TrustGuard raises SENSOR_FAULT, so any of them
 * makes the window CHECK_SENSOR; the node never decides about equipment.
 */

#ifndef SENSOR_HAL_H
#define SENSOR_HAL_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    NODE_FAULT_ACCEL_BUS     = 1u << 0,  /* I2C transfer to the ADXL345 failed        */
    NODE_FAULT_ACCEL_OVERRUN = 1u << 1,  /* FIFO overrun: samples of this window lost */
    NODE_FAULT_ACCEL_MISSING = 1u << 2,  /* ADXL345 absent (DEVID) or failing         */
    NODE_FAULT_TEMP_CRC      = 1u << 3,  /* DS18B20 scratchpad CRC mismatch           */
    NODE_FAULT_TEMP_MISSING  = 1u << 4,  /* no presence pulse / no recent valid value */
    NODE_FAULT_TEMP_POR      = 1u << 5,  /* 85 °C power-on-reset value read           */
    NODE_FAULT_CURRENT_ADC   = 1u << 6,  /* current-sensor ADC read failed            */
    NODE_FAULT_CURRENT_CLIP  = 1u << 7,  /* CT signal at the ADC limits (overrange)   */
} node_fault_t;

#define NODE_FAULT_COUNT 8
#define NODE_FAULT_ALL   0xFFu

/** Contract name of fault bit @p index (0..NODE_FAULT_COUNT-1), NULL otherwise. */
const char *node_fault_name(int index);

typedef enum {
    NODE_SELF_TEST_NOT_RUN = 0,
    NODE_SELF_TEST_PASS,
    NODE_SELF_TEST_FAIL,
} node_self_test_t;

/** "not_run" / "pass" / "fail" as used in the payload. */
const char *node_self_test_str(node_self_test_t s);

/** Status codes of the sensor drivers (0 = success). */
typedef enum {
    SENSOR_OK          = 0,
    SENSOR_ERR_BUS     = -1,   /* transfer error or timeout      */
    SENSOR_ERR_ABSENT  = -2,   /* device not detected            */
    SENSOR_ERR_CRC     = -3,   /* data integrity check failed    */
    SENSOR_ERR_RANGE   = -4,   /* value outside the sensor range */
    SENSOR_ERR_ARG     = -5,   /* invalid argument               */
} sensor_status_t;

#endif /* SENSOR_HAL_H */
