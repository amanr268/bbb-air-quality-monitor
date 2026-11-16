#ifndef AQM_ENS160_H
#define AQM_ENS160_H

#include <stdint.h>

/*
 * ScioSense ENS160 metal-oxide gas sensor over I2C (Linux i2c-dev).
 * 0x53 with ADDR high (the ENS160+AHT21 module default), 0x52 with ADDR low.
 * Standard mode: new TVOC / eCO2 / AQI-UBA values once per second.
 *
 * eCO2 is an ESTIMATE derived from the VOC signal, not a CO2 measurement.
 * Validity: after power-up the sensor warms up (~3 min, values approximate);
 * on its very first use it runs an initial start-up phase (~1 h).
 * Temperature / humidity compensation should be written from the AHT21.
 */

#define ENS160_ADDR      0x53
#define ENS160_ADDR_ALT  0x52

enum { ENS160_VALID = 0, ENS160_WARMUP = 1, ENS160_STARTUP = 2, ENS160_INVALID = 3 };

typedef struct {
    uint16_t tvoc_ppb;
    uint16_t eco2_ppm;
    uint8_t  aqi_uba;        /* ENS160's own 1..5 index (UBA) */
    uint8_t  validity;       /* ENS160_* */
} ens160_data_t;

typedef struct {
    int      fd;
    uint8_t  addr;
    uint8_t  last_status;
    uint32_t reads_ok, io_errors;
} ens160_t;

/* probe PART_ID 0x0160 at addr (0 = try 0x53 then 0x52), reset, start
 * standard mode. 0, -ENODEV (no ENS160) or -errno */
int  ens160_open(ens160_t *e, const char *i2c_dev, uint8_t addr);
void ens160_close(ens160_t *e);                  /* puts the sensor to sleep */

/* 1 = new data in *d, 0 = no new data yet, -EIO = sensor error flag,
 * -errno = bus error. d->validity is always updated on success. */
int  ens160_read(ens160_t *e, ens160_data_t *d);

/* write ambient temperature / humidity for compensation */
int  ens160_set_env(ens160_t *e, float temp_c, float rh_pct);

const char *ens160_validity_name(int v);

/* pure helpers (unit-tested) */
void ens160_encode_env(float temp_c, float rh_pct, uint8_t out[4]);
void ens160_parse_data(const uint8_t b[5], ens160_data_t *d);

#endif
