/* DS18B20 CRC, configuration and scratchpad validation. */

#include <string.h>

#include "ds18b20_convert.h"
#include "test_harness.h"

static void make_scratchpad(uint8_t sp[9], uint16_t raw, uint8_t config)
{
    sp[0] = (uint8_t)(raw & 0xFF);
    sp[1] = (uint8_t)(raw >> 8);
    sp[2] = DS18B20_ALARM_TH_DEFAULT;
    sp[3] = DS18B20_ALARM_TL_DEFAULT;
    sp[4] = config;
    sp[5] = 0xFF;
    sp[6] = 0x0C;
    sp[7] = 0x10;
    sp[8] = ds18b20_crc8(sp, 8);
}

static int test_crc8(void)
{
    /* Maxim Application Note 27 worked example: ROM 02 1C B8 01 00 00 00 -> A2. */
    const uint8_t rom[7] = { 0x02, 0x1C, 0xB8, 0x01, 0x00, 0x00, 0x00 };
    ASSERT(ds18b20_crc8(rom, 7) == 0xA2, "AN27 example");
    uint8_t with_crc[8];
    memcpy(with_crc, rom, 7);
    with_crc[7] = 0xA2;
    ASSERT(ds18b20_crc8(with_crc, 8) == 0x00, "data + CRC checks to zero");
    ASSERT(ds18b20_crc8(NULL, 4) == 0x00, "NULL input");
    return 0;
}

static int test_configuration(void)
{
    uint8_t v = 0;
    ASSERT(ds18b20_config_byte(9, &v) && v == 0x1F, "9 bit");
    ASSERT(ds18b20_config_byte(10, &v) && v == 0x3F, "10 bit");
    ASSERT(ds18b20_config_byte(11, &v) && v == 0x5F, "11 bit");
    ASSERT(ds18b20_config_byte(12, &v) && v == 0x7F, "12 bit");
    ASSERT(!ds18b20_config_byte(8, &v) && !ds18b20_config_byte(13, &v), "outside 9..12");
    ASSERT(ds18b20_resolution_from_config(0x5F) == 11, "inverse");
    ASSERT(ds18b20_conversion_ms(12) == 750 && ds18b20_conversion_ms(9) == 94, "tCONV");
    return 0;
}

static int test_decodes_valid_readings(void)
{
    uint8_t sp[9];
    float t = 0.0f;
    int bits = 0;
    make_scratchpad(sp, 0x0191, 0x7F);                    /* +25.0625 °C */
    ASSERT(ds18b20_decode(sp, NULL, &t, &bits) == DS18B20_DATA_OK, "valid");
    ASSERT_NEAR(t, 25.0625, 1e-6, "12-bit value");
    ASSERT(bits == 12, "resolution reported");
    make_scratchpad(sp, 0xFF5E, 0x7F);                    /* -10.125 °C */
    ASSERT(ds18b20_decode(sp, NULL, &t, &bits) == DS18B20_DATA_OK, "negative");
    ASSERT_NEAR(t, -10.125, 1e-6, "two's complement");
    make_scratchpad(sp, 0x0197, 0x1F);                    /* 9 bit: low 3 bits undefined */
    ASSERT(ds18b20_decode(sp, NULL, &t, &bits) == DS18B20_DATA_OK && bits == 9, "9 bit");
    ASSERT_NEAR(t, 25.0, 1e-6, "undefined bits masked");
    return 0;
}

static int test_power_on_value(void)
{
    /* The classic power-on scratchpad: 50 05 4B 46 7F FF 0C 10 1C (85 °C). */
    const uint8_t por[9] = { 0x50, 0x05, 0x4B, 0x46, 0x7F, 0xFF, 0x0C, 0x10, 0x1C };
    float t = 0.0f;
    ASSERT(ds18b20_decode(por, NULL, &t, NULL) == DS18B20_DATA_POWER_ON, "no history: POR");
    float prev_cool = 31.5f;
    ASSERT(ds18b20_decode(por, &prev_cool, &t, NULL) == DS18B20_DATA_POWER_ON,
           "jump from 31.5 °C to exactly 85 °C: POR");
    float prev_hot = 84.25f;
    ASSERT(ds18b20_decode(por, &prev_hot, &t, NULL) == DS18B20_DATA_OK && t == 85.0f,
           "a genuine 85 °C after 84.25 °C is kept");
    return 0;
}

static int test_rejects_bad_scratchpads(void)
{
    uint8_t sp[9];
    float t = 0.0f;
    memset(sp, 0xFF, sizeof(sp));
    ASSERT(ds18b20_decode(sp, NULL, &t, NULL) == DS18B20_DATA_NO_DEVICE, "all 0xFF");
    memset(sp, 0x00, sizeof(sp));
    ASSERT(ds18b20_decode(sp, NULL, &t, NULL) == DS18B20_DATA_BUS_SHORT,
           "all zero (would pass the CRC)");
    make_scratchpad(sp, 0x0191, 0x7F);
    sp[0] ^= 0x01;                                        /* single bit error */
    ASSERT(ds18b20_decode(sp, NULL, &t, NULL) == DS18B20_DATA_CRC, "CRC catches the bit flip");
    make_scratchpad(sp, 0x07F0, 0x7F);                    /* +127 °C */
    ASSERT(ds18b20_decode(sp, NULL, &t, NULL) == DS18B20_DATA_RANGE, "above +125 °C");
    ASSERT(strcmp(ds18b20_data_status_str(DS18B20_DATA_CRC), "CRC mismatch") == 0, "text");
    return 0;
}

int main(void)
{
    printf("ds18b20_convert\n");
    RUN_TEST(test_crc8);
    RUN_TEST(test_configuration);
    RUN_TEST(test_decodes_valid_readings);
    RUN_TEST(test_power_on_value);
    RUN_TEST(test_rejects_bad_scratchpads);
    TEST_SUMMARY();
    return TEST_EXIT_CODE();
}
