#ifndef AQM_ADXL345_H
#define AQM_ADXL345_H

#include <stdint.h>

/* ADXL345 3-axis accelerometer over I2C (Linux i2c-dev).
 * Address 0x1D with SDO high, 0x53 with SDO low. */

typedef struct {
    int fd;
} adxl345_t;

int  adxl345_open(adxl345_t *a, const char *i2c_dev, uint8_t addr);  /* 0 or -errno */
void adxl345_close(adxl345_t *a);

/* raw counts (full resolution, +-2 g: ~256 counts per g) */
int  adxl345_read_raw(adxl345_t *a, int16_t *x, int16_t *y, int16_t *z);
/* acceleration in g */
int  adxl345_read_g(adxl345_t *a, float *gx, float *gy, float *gz);

#endif
