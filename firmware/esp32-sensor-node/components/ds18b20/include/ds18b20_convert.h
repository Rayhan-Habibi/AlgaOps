/*
 * ds18b20_convert.h — hardware-independent DS18B20 logic (host-tested):
 * commands, the Dallas/Maxim CRC-8 and scratchpad validation.
 *
 * References: Maxim DS18B20 data sheet (scratchpad layout, configuration
 * register, conversion times, 85 °C power-on value) and Maxim Application
 * Note 27 (1-Wire CRC-8, polynomial x^8 + x^5 + x^4 + 1).
 */

#ifndef DS18B20_CONVERT_H
#define DS18B20_CONVERT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DS18B20_FAMILY_CODE           0x28
#define DS18B20_CMD_MATCH_ROM         0x55
#define DS18B20_CMD_SKIP_ROM          0xCC
#define DS18B20_CMD_CONVERT_T         0x44
#define DS18B20_CMD_WRITE_SCRATCHPAD  0x4E
#define DS18B20_CMD_READ_SCRATCHPAD   0xBE
#define DS18B20_SCRATCHPAD_LEN        9
#define DS18B20_POWER_ON_RAW          0x0550   /* +85.0 °C */
#define DS18B20_ALARM_TH_DEFAULT      0x4B     /* +75 °C (unused alarm registers) */
#define DS18B20_ALARM_TL_DEFAULT      0x46     /* +70 °C */

typedef enum {
    DS18B20_DATA_OK = 0,
    DS18B20_DATA_NO_DEVICE,    /* all bytes 0xFF: nothing answered          */
    DS18B20_DATA_BUS_SHORT,    /* all bytes 0x00: data line held low        */
    DS18B20_DATA_CRC,          /* CRC-8 mismatch                            */
    DS18B20_DATA_POWER_ON,     /* 85 °C power-on value: no conversion done  */
    DS18B20_DATA_RANGE,        /* outside the -55..+125 °C sensor range     */
} ds18b20_data_status_t;

/** 1-Wire CRC-8 (reflected polynomial 0x8C, initial value 0). */
uint8_t ds18b20_crc8(const uint8_t *data, size_t len);

/** Configuration register value for 9..12 bit resolution. */
bool ds18b20_config_byte(int resolution_bits, uint8_t *value);

/** Resolution encoded in a configuration register value (9..12). */
int ds18b20_resolution_from_config(uint8_t config);

/** Maximum conversion time in ms (data sheet tCONV), rounded up. */
uint32_t ds18b20_conversion_ms(int resolution_bits);

/**
 * Validate and decode a scratchpad.  @p previous_c is the last accepted value
 * (NULL if none): an 85.0 °C reading is reported as POWER_ON unless the
 * previous value was within 10 °C of it, because a real jump to exactly 85 °C
 * is far less likely than a missed conversion after a brown-out.
 */
ds18b20_data_status_t ds18b20_decode(const uint8_t sp[DS18B20_SCRATCHPAD_LEN],
                                     const float *previous_c, float *temp_c,
                                     int *resolution_bits);

const char *ds18b20_data_status_str(ds18b20_data_status_t s);

#endif /* DS18B20_CONVERT_H */
