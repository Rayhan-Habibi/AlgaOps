/* ds18b20_convert.c — see ds18b20_convert.h */

#include "ds18b20_convert.h"

#include <math.h>

uint8_t ds18b20_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0;
    for (size_t i = 0; data && i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1u) ? (uint8_t)((crc >> 1) ^ 0x8Cu) : (uint8_t)(crc >> 1);
        }
    }
    return crc;
}

bool ds18b20_config_byte(int resolution_bits, uint8_t *value)
{
    if (resolution_bits < 9 || resolution_bits > 12) return false;
    if (value) *value = (uint8_t)(((resolution_bits - 9) << 5) | 0x1F);
    return true;
}

int ds18b20_resolution_from_config(uint8_t config)
{
    return ((config >> 5) & 0x3) + 9;
}

uint32_t ds18b20_conversion_ms(int resolution_bits)
{
    switch (resolution_bits) {
        case 9:  return 94;    /* 93.75 ms */
        case 10: return 188;   /* 187.5 ms */
        case 11: return 375;
        default: return 750;
    }
}

ds18b20_data_status_t ds18b20_decode(const uint8_t sp[DS18B20_SCRATCHPAD_LEN],
                                     const float *previous_c, float *temp_c,
                                     int *resolution_bits)
{
    bool all_ff = true;
    bool all_00 = true;
    for (int i = 0; i < DS18B20_SCRATCHPAD_LEN; i++) {
        all_ff = all_ff && sp[i] == 0xFF;
        all_00 = all_00 && sp[i] == 0x00;
    }
    if (all_ff) return DS18B20_DATA_NO_DEVICE;
    if (all_00) return DS18B20_DATA_BUS_SHORT;     /* would otherwise pass the CRC */
    if (ds18b20_crc8(sp, 8) != sp[8]) return DS18B20_DATA_CRC;

    int bits = ds18b20_resolution_from_config(sp[4]);
    uint16_t raw = (uint16_t)(sp[0] | ((uint16_t)sp[1] << 8));
    /* Low bits are undefined below 12-bit resolution (data sheet, Figure 2). */
    raw &= (uint16_t)~((1u << (12 - bits)) - 1u);
    float t = (float)(int16_t)raw / 16.0f;
    if (resolution_bits) *resolution_bits = bits;
    if (raw == DS18B20_POWER_ON_RAW && (!previous_c || fabsf(*previous_c - 85.0f) > 10.0f)) {
        return DS18B20_DATA_POWER_ON;
    }
    if (t < -55.0f || t > 125.0f) return DS18B20_DATA_RANGE;
    if (temp_c) *temp_c = t;
    return DS18B20_DATA_OK;
}

const char *ds18b20_data_status_str(ds18b20_data_status_t s)
{
    switch (s) {
        case DS18B20_DATA_OK:         return "ok";
        case DS18B20_DATA_NO_DEVICE:  return "no device";
        case DS18B20_DATA_BUS_SHORT:  return "bus held low";
        case DS18B20_DATA_CRC:        return "CRC mismatch";
        case DS18B20_DATA_POWER_ON:   return "85 C power-on value";
        case DS18B20_DATA_RANGE:      return "out of range";
    }
    return "unknown";
}
