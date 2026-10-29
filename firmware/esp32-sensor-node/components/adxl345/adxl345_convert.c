/* adxl345_convert.c — see adxl345_convert.h */

#include "adxl345_convert.h"

#include <stddef.h>

#include "adxl345_regs.h"

bool adxl345_rate_code(uint32_t rate_hz, uint8_t *code)
{
    /* Data sheet Table 7: 3200 Hz = 0x0F, halving per step down to 12.5 Hz = 0x07. */
    static const struct { uint32_t hz; uint8_t code; } rates[] = {
        {3200, 0x0F}, {1600, 0x0E}, {800, 0x0D}, {400, 0x0C}, {200, 0x0B},
        {100, 0x0A}, {50, 0x09}, {25, 0x08},
    };
    for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
        if (rates[i].hz == rate_hz) {
            if (code) *code = rates[i].code;
            return true;
        }
    }
    return false;
}

bool adxl345_data_format(uint8_t range_g, bool full_res, bool self_test, uint8_t *value)
{
    uint8_t r;
    switch (range_g) {
        case 2:  r = 0x00; break;
        case 4:  r = 0x01; break;
        case 8:  r = 0x02; break;
        case 16: r = 0x03; break;
        default: return false;
    }
    if (value) {
        *value = (uint8_t)(r | (full_res ? ADXL345_FORMAT_FULL_RES : 0) |
                           (self_test ? ADXL345_FORMAT_SELF_TEST : 0));
    }
    return true;
}

uint8_t adxl345_fifo_ctl_stream(uint8_t samples)
{
    if (samples < 1) samples = 1;
    if (samples > 31) samples = 31;
    return (uint8_t)(ADXL345_FIFO_MODE_STREAM | samples);
}

void adxl345_decode_xyz(const uint8_t raw[6], int16_t out[3])
{
    for (int a = 0; a < 3; a++) {
        out[a] = (int16_t)(uint16_t)((uint16_t)raw[2 * a] | ((uint16_t)raw[2 * a + 1] << 8));
    }
}

uint8_t adxl345_fifo_entries(uint8_t fifo_status)
{
    uint8_t n = fifo_status & ADXL345_FIFO_ENTRIES_MASK;
    return n > ADXL345_FIFO_DEPTH ? ADXL345_FIFO_DEPTH : n;
}

static double interpolate(double vs, const double v[4], const double f[4])
{
    if (vs <= v[0]) return f[0];
    for (int i = 1; i < 4; i++) {
        if (vs <= v[i]) return f[i - 1] + (f[i] - f[i - 1]) * (vs - v[i - 1]) / (v[i] - v[i - 1]);
    }
    return f[3];
}

void adxl345_self_test_limits(double vs, adxl345_st_limits_t *out)
{
    static const double volts[4] = {2.0, 2.5, 3.3, 3.6};
    static const double fxy[4]   = {0.64, 1.00, 1.77, 2.11};
    static const double fz[4]    = {0.80, 1.00, 1.47, 1.69};
    static const double min25[3] = {50.0, -540.0, 75.0};
    static const double max25[3] = {540.0, -50.0, 875.0};
    if (!out) return;
    double kxy = interpolate(vs, volts, fxy);
    double kz  = interpolate(vs, volts, fz);
    for (int a = 0; a < 3; a++) {
        double k = a == 2 ? kz : kxy;
        out->min_lsb[a] = min25[a] * k;
        out->max_lsb[a] = max25[a] * k;
    }
}

bool adxl345_self_test_evaluate(const double mean_off_lsb[3], const double mean_on_lsb[3],
                                double vs, double delta[3])
{
    if (!mean_off_lsb || !mean_on_lsb) return false;
    adxl345_st_limits_t lim;
    adxl345_self_test_limits(vs, &lim);
    bool ok = true;
    for (int a = 0; a < 3; a++) {
        double d = mean_on_lsb[a] - mean_off_lsb[a];
        if (delta) delta[a] = d;
        if (!(d >= lim.min_lsb[a] && d <= lim.max_lsb[a])) ok = false;
    }
    return ok;
}
