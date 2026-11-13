#include "aqm/adxl345.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define REG_DEVID        0x00
#define REG_BW_RATE      0x2C
#define REG_POWER_CTL    0x2D
#define REG_DATA_FORMAT  0x31
#define REG_DATAX0       0x32

#define DEVID            0xE5
#define G_PER_COUNT      0.0039f     /* full-resolution mode: 3.9 mg/LSB */

static int wr(adxl345_t *a, uint8_t reg, uint8_t val)
{
    uint8_t b[2] = { reg, val };

    return write(a->fd, b, 2) == 2 ? 0 : -errno;
}

static int rd(adxl345_t *a, uint8_t reg, uint8_t *buf, int n)
{
    if (write(a->fd, &reg, 1) != 1)
        return -errno;
    return read(a->fd, buf, (size_t)n) == n ? 0 : -errno;
}

int adxl345_open(adxl345_t *a, const char *i2c_dev, uint8_t addr)
{
    uint8_t id;
    int rc;

    a->fd = open(i2c_dev, O_RDWR);
    if (a->fd < 0)
        return -errno;
    if (ioctl(a->fd, I2C_SLAVE, addr) < 0) {
        rc = -errno;
        goto fail;
    }
    if ((rc = rd(a, REG_DEVID, &id, 1)) != 0)
        goto fail;
    if (id != DEVID) {
        rc = -ENODEV;                      /* something else at this address */
        goto fail;
    }
    if ((rc = wr(a, REG_DATA_FORMAT, 0x08)) ||   /* FULL_RES, +-2 g        */
        (rc = wr(a, REG_BW_RATE, 0x0A)) ||       /* 100 Hz output rate      */
        (rc = wr(a, REG_POWER_CTL, 0x08)))       /* measurement mode        */
        goto fail;
    return 0;

fail:
    close(a->fd);
    a->fd = -1;
    return rc;
}

void adxl345_close(adxl345_t *a)
{
    if (a->fd >= 0) {
        wr(a, REG_POWER_CTL, 0x00);        /* standby: saves power */
        close(a->fd);
        a->fd = -1;
    }
}

int adxl345_read_raw(adxl345_t *a, int16_t *x, int16_t *y, int16_t *z)
{
    uint8_t b[6];
    int rc = rd(a, REG_DATAX0, b, 6);      /* X0 X1 Y0 Y1 Z0 Z1, little-endian */

    if (rc)
        return rc;
    *x = (int16_t)(b[0] | (b[1] << 8));
    *y = (int16_t)(b[2] | (b[3] << 8));
    *z = (int16_t)(b[4] | (b[5] << 8));
    return 0;
}

int adxl345_read_g(adxl345_t *a, float *gx, float *gy, float *gz)
{
    int16_t x, y, z;
    int rc = adxl345_read_raw(a, &x, &y, &z);

    if (rc)
        return rc;
    *gx = x * G_PER_COUNT;
    *gy = y * G_PER_COUNT;
    *gz = z * G_PER_COUNT;
    return 0;
}
