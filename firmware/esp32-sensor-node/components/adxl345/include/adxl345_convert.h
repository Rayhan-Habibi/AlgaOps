/*
 * adxl345_convert.h — hardware-independent ADXL345 logic (host-tested):
 * register values, sample decoding and the self-test evaluation.
 */

#ifndef ADXL345_CONVERT_H
#define ADXL345_CONVERT_H

#include <stdbool.h>
#include <stdint.h>

/* Full-resolution scale factor: typical 3.9 mg/LSB on every range (data sheet
 * "Sensitivity at XOUT, YOUT, ZOUT", full resolution).  Per-part sensitivity
 * varies by a few percent; it is absorbed by the per-asset baseline. */
#define ADXL345_G_PER_LSB_FULL_RES 0.0039

/** BW_RATE rate code for an output data rate in Hz (6.25 Hz codes excluded). */
bool adxl345_rate_code(uint32_t rate_hz, uint8_t *code);

/** DATA_FORMAT value: right-justified, active-high interrupts, 4-wire SPI bit 0. */
bool adxl345_data_format(uint8_t range_g, bool full_res, bool self_test, uint8_t *value);

/** FIFO_CTL value for stream mode with a watermark of @p samples (1..31). */
uint8_t adxl345_fifo_ctl_stream(uint8_t samples);

/** Decode one FIFO entry read from DATAX0..DATAZ1 (little endian, two's complement). */
void adxl345_decode_xyz(const uint8_t raw[6], int16_t out[3]);

/** Entries waiting in the FIFO (FIFO_STATUS D5:D0). */
uint8_t adxl345_fifo_entries(uint8_t fifo_status);

/* ---- self-test --------------------------------------------------------------
 * Data sheet Table 14: self-test output change in LSB for full resolution at
 * VS = 2.5 V: X +50..+540, Y -540..-50, Z +75..+875.  Table 15 scales the
 * limits for other supply voltages (2.0 V: X/Y 0.64, Z 0.80; 2.5 V: 1.00;
 * 3.3 V: X/Y 1.77, Z 1.47; 3.6 V: X/Y 2.11, Z 1.69), interpolated linearly
 * here.  The change is measured as mean(ST on) - mean(ST off) over >= 100
 * samples at >= 100 Hz with the part in full resolution (±16 g recommended). */

typedef struct {
    double min_lsb[3];
    double max_lsb[3];
} adxl345_st_limits_t;

/** Limits for supply voltage @p vs (clamped to 2.0..3.6 V). */
void adxl345_self_test_limits(double vs, adxl345_st_limits_t *out);

/** True when every axis change lies inside the limits; @p delta receives the changes. */
bool adxl345_self_test_evaluate(const double mean_off_lsb[3], const double mean_on_lsb[3],
                                double vs, double delta[3]);

#endif /* ADXL345_CONVERT_H */
