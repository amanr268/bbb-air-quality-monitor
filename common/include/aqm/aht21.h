#ifndef AQM_AHT21_H
#define AQM_AHT21_H

#include <stdint.h>

/*
 * ASAIR AHT21 temperature / humidity sensor over I2C (Linux i2c-dev), 0x38.
 *
 * A measurement takes ~80 ms. To keep the 1 Hz real-time loop in `sense`
 * from ever waiting, the driver is split: aht21_fetch() reads the result of
 * the measurement started on the PREVIOUS tick, then aht21_trigger() starts
 * the next one. The value is 1 s old, the loop never blocks for 80 ms.
 *
 * Raw values are the 20-bit sensor words (same as aqm_sample_t.aht_*_raw):
 *   T  = raw / 2^20 * 200 - 50  [deg C]      RH = raw / 2^20 * 100  [%]
 */

#define AHT21_ADDR 0x38

typedef struct {
    int      fd;
    uint32_t frames_ok, crc_errors, busy, io_errors;
} aht21_t;

int  aht21_open(aht21_t *a, const char *i2c_dev);   /* probe + init + first trigger */
void aht21_close(aht21_t *a);
int  aht21_trigger(aht21_t *a);                     /* start a measurement: 0/-errno */
/* read the last measurement: 0, -EBUSY (not finished), -EBADMSG (CRC),
 * or -errno from the bus */
int  aht21_fetch(aht21_t *a, uint32_t *t_raw, uint32_t *rh_raw);

/* pure helpers (unit-tested) */
uint8_t aht21_crc8(const uint8_t *d, int n);        /* poly 0x31, init 0xFF */
int     aht21_parse(const uint8_t f[7], uint32_t *t_raw, uint32_t *rh_raw);
float   aht21_temp_c(uint32_t t_raw);
float   aht21_rh(uint32_t rh_raw);

#endif
