/* ADXL345 register values, sample decoding and the data-sheet self-test limits. */

#include "adxl345_convert.h"
#include "adxl345_regs.h"
#include "test_harness.h"

static int test_rate_codes(void)
{
    uint8_t c = 0;
    ASSERT(adxl345_rate_code(3200, &c) && c == 0x0F, "3200 Hz");
    ASSERT(adxl345_rate_code(1600, &c) && c == 0x0E, "1600 Hz");
    ASSERT(adxl345_rate_code(800, &c) && c == 0x0D, "800 Hz");
    ASSERT(adxl345_rate_code(400, &c) && c == 0x0C, "400 Hz");
    ASSERT(adxl345_rate_code(100, &c) && c == 0x0A, "100 Hz");
    ASSERT(adxl345_rate_code(25, &c) && c == 0x08, "25 Hz");
    ASSERT(!adxl345_rate_code(1000, &c), "not an ADXL345 rate");
    ASSERT(!adxl345_rate_code(0, &c), "zero");
    return 0;
}

static int test_data_format(void)
{
    uint8_t v = 0;
    ASSERT(adxl345_data_format(16, true, false, &v) && v == 0x0B, "±16 g full resolution");
    ASSERT(adxl345_data_format(8, true, false, &v) && v == 0x0A, "±8 g full resolution");
    ASSERT(adxl345_data_format(2, true, true, &v) && v == 0x88, "±2 g with self-test");
    ASSERT(adxl345_data_format(4, false, false, &v) && v == 0x01, "±4 g 10-bit");
    ASSERT(!adxl345_data_format(3, true, false, &v), "no ±3 g range");
    return 0;
}

static int test_fifo(void)
{
    ASSERT(adxl345_fifo_ctl_stream(16) == 0x90, "stream mode, watermark 16");
    ASSERT(adxl345_fifo_ctl_stream(0) == 0x81, "watermark clamped to 1");
    ASSERT(adxl345_fifo_ctl_stream(40) == 0x9F, "watermark clamped to 31");
    ASSERT(adxl345_fifo_entries(0x8A) == 10, "FIFO_TRIG bit ignored");
    ASSERT(adxl345_fifo_entries(0x25) == 32, "never more than the FIFO depth");
    return 0;
}

static int test_decode(void)
{
    const uint8_t raw[6] = { 0x00, 0x01, 0xFF, 0xFF, 0x00, 0x80 };
    int16_t xyz[3];
    adxl345_decode_xyz(raw, xyz);
    ASSERT(xyz[0] == 256 && xyz[1] == -1 && xyz[2] == -32768, "little endian two's complement");
    const uint8_t g1[6] = { 0x05, 0x00, 0xFB, 0xFF, 0x00, 0x01 };
    adxl345_decode_xyz(g1, xyz);
    ASSERT(xyz[0] == 5 && xyz[1] == -5 && xyz[2] == 256, "1 g on Z = 256 LSB");
    ASSERT_NEAR(256 * ADXL345_G_PER_LSB_FULL_RES, 0.9984, 1e-9, "3.9 mg/LSB");
    return 0;
}

static int test_self_test_limits(void)
{
    adxl345_st_limits_t l;
    adxl345_self_test_limits(2.5, &l);                     /* data sheet Table 14 */
    ASSERT(l.min_lsb[0] == 50.0 && l.max_lsb[0] == 540.0, "X at 2.5 V");
    ASSERT(l.min_lsb[1] == -540.0 && l.max_lsb[1] == -50.0, "Y at 2.5 V");
    ASSERT(l.min_lsb[2] == 75.0 && l.max_lsb[2] == 875.0, "Z at 2.5 V");
    adxl345_self_test_limits(3.3, &l);                     /* Table 15: 1.77 / 1.47 */
    ASSERT_NEAR(l.min_lsb[0], 88.5, 1e-9, "X min at 3.3 V");
    ASSERT_NEAR(l.max_lsb[0], 955.8, 1e-9, "X max at 3.3 V");
    ASSERT_NEAR(l.min_lsb[2], 110.25, 1e-9, "Z min at 3.3 V");
    ASSERT_NEAR(l.max_lsb[2], 1286.25, 1e-9, "Z max at 3.3 V");
    adxl345_self_test_limits(1.8, &l);                     /* clamped to 2.0 V */
    ASSERT_NEAR(l.min_lsb[0], 50.0 * 0.64, 1e-9, "below the table: 2.0 V factors");
    adxl345_self_test_limits(2.9, &l);                     /* halfway 2.5 .. 3.3 V */
    ASSERT_NEAR(l.max_lsb[0], 540.0 * 1.385, 1e-9, "interpolated");
    return 0;
}

static int test_self_test_evaluation(void)
{
    const double off[3] = { 3.0, -2.0, 256.0 };
    const double good[3] = { 303.0, -302.0, 700.0 };       /* +300 / -300 / +444 LSB */
    double d[3];
    ASSERT(adxl345_self_test_evaluate(off, good, 3.3, d), "plausible response passes");
    ASSERT_NEAR(d[0], 300.0, 1e-9, "X change");
    ASSERT_NEAR(d[2], 444.0, 1e-9, "Z change");
    const double weak[3] = { 40.0, -302.0, 700.0 };        /* X moves only 37 LSB */
    ASSERT(!adxl345_self_test_evaluate(off, weak, 3.3, d), "weak axis fails");
    const double flipped[3] = { 303.0, 298.0, 700.0 };     /* Y moves the wrong way */
    ASSERT(!adxl345_self_test_evaluate(off, flipped, 3.3, d), "wrong sign fails");
    ASSERT(!adxl345_self_test_evaluate(off, off, 3.3, d), "no response (stuck) fails");
    return 0;
}

int main(void)
{
    printf("adxl345_convert\n");
    RUN_TEST(test_rate_codes);
    RUN_TEST(test_data_format);
    RUN_TEST(test_fifo);
    RUN_TEST(test_decode);
    RUN_TEST(test_self_test_limits);
    RUN_TEST(test_self_test_evaluation);
    TEST_SUMMARY();
    return TEST_EXIT_CODE();
}
