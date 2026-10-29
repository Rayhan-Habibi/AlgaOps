/*
 * adxl345_regs.h — ADXL345 register map (Analog Devices ADXL345 data sheet,
 * Rev. G, "Register Map" and "Register Definitions").  Check the revision of
 * the data sheet that matches the purchased part before changing values.
 */

#ifndef ADXL345_REGS_H
#define ADXL345_REGS_H

/* 7-bit I2C addresses: ALT ADDRESS (SDO) pin low / high. */
#define ADXL345_I2C_ADDR_SDO_LOW   0x53
#define ADXL345_I2C_ADDR_SDO_HIGH  0x1D

#define ADXL345_REG_DEVID          0x00   /* fixed device ID, reads 0xE5          */
#define ADXL345_REG_OFSX           0x1E
#define ADXL345_REG_OFSY           0x1F
#define ADXL345_REG_OFSZ           0x20
#define ADXL345_REG_BW_RATE        0x2C   /* D4 LOW_POWER, D3:D0 rate code         */
#define ADXL345_REG_POWER_CTL      0x2D   /* D3 Measure                            */
#define ADXL345_REG_INT_ENABLE     0x2E
#define ADXL345_REG_INT_MAP        0x2F
#define ADXL345_REG_INT_SOURCE     0x30   /* D7 DATA_READY, D1 Watermark, D0 Overrun */
#define ADXL345_REG_DATA_FORMAT    0x31   /* D7 SELF_TEST, D3 FULL_RES, D1:D0 range */
#define ADXL345_REG_DATAX0         0x32   /* X0 X1 Y0 Y1 Z0 Z1, little endian      */
#define ADXL345_REG_FIFO_CTL       0x38   /* D7:D6 FIFO_MODE, D4:D0 samples        */
#define ADXL345_REG_FIFO_STATUS    0x39   /* D7 FIFO_TRIG, D5:D0 entries           */

#define ADXL345_DEVID_VALUE        0xE5

#define ADXL345_POWER_CTL_MEASURE  0x08

#define ADXL345_INT_DATA_READY     0x80
#define ADXL345_INT_WATERMARK      0x02
#define ADXL345_INT_OVERRUN        0x01

#define ADXL345_FORMAT_SELF_TEST   0x80
#define ADXL345_FORMAT_FULL_RES    0x08

#define ADXL345_FIFO_MODE_BYPASS   0x00
#define ADXL345_FIFO_MODE_FIFO     0x40
#define ADXL345_FIFO_MODE_STREAM   0x80
#define ADXL345_FIFO_DEPTH         32
#define ADXL345_FIFO_ENTRIES_MASK  0x3F

#endif /* ADXL345_REGS_H */
