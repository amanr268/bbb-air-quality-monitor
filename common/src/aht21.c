#include "aqm/aht21.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define ST_BUSY        0x80
#define ST_CAL_MASK    0x18          /* both bits set = calibration loaded */

static void sleep_ms(int ms)
{
    struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000L };

    nanosleep(&t, NULL);
}

static int wr(aht21_t *a, const uint8_t *b, int n)
{
    return write(a->fd, b, (size_t)n) == n ? 0 : -errno;
}

static int rd(aht21_t *a, uint8_t *b, int n)
{
    return read(a->fd, b, (size_t)n) == n ? 0 : -errno;
}

uint8_t aht21_crc8(const uint8_t *d, int n)
{
    uint8_t crc = 0xFF;
    int i, b;

    for (i = 0; i < n; i++) {
        crc ^= d[i];
        for (b = 0; b < 8; b++)
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
    }
    return crc;
}

int aht21_parse(const uint8_t f[7], uint32_t *t_raw, uint32_t *rh_raw)
{
    if (f[0] & ST_BUSY)
        return -EBUSY;
    if (aht21_crc8(f, 6) != f[6])
        return -EBADMSG;
    *rh_raw = ((uint32_t)f[1] << 12) | ((uint32_t)f[2] << 4) | (f[3] >> 4);
    *t_raw  = ((uint32_t)(f[3] & 0x0F) << 16) | ((uint32_t)f[4] << 8) | f[5];
    return 0;
}

float aht21_temp_c(uint32_t t_raw)
{
    return (float)t_raw / 1048576.0f * 200.0f - 50.0f;
}

float aht21_rh(uint32_t rh_raw)
{
    return (float)rh_raw / 1048576.0f * 100.0f;
}

/* ASAIR start-up procedure for a register that lost its calibration value */
static int reset_reg(aht21_t *a, uint8_t reg)
{
    uint8_t cmd[3] = { reg, 0x00, 0x00 }, r[3];
    int rc;

    if ((rc = wr(a, cmd, 3)))
        return rc;
    sleep_ms(5);
    if ((rc = rd(a, r, 3)))
        return rc;
    sleep_ms(10);
    cmd[0] = (uint8_t)(0xB0 | reg);
    cmd[1] = r[1];
    cmd[2] = r[2];
    return wr(a, cmd, 3);
}

int aht21_open(aht21_t *a, const char *i2c_dev)
{
    uint8_t q = 0x71, st;
    int rc;

    a->frames_ok = a->crc_errors = a->busy = a->io_errors = 0;
    a->fd = open(i2c_dev, O_RDWR | O_CLOEXEC);
    if (a->fd < 0)
        return -errno;
    if (ioctl(a->fd, I2C_SLAVE, AHT21_ADDR) < 0) {
        rc = -errno;
        goto fail;
    }
    /* status byte; a NACK here means no sensor at 0x38 */
    if ((rc = wr(a, &q, 1)) || (rc = rd(a, &st, 1)))
        goto fail;
    if ((st & ST_CAL_MASK) != ST_CAL_MASK) {
        if ((rc = reset_reg(a, 0x1B)) || (rc = reset_reg(a, 0x1C)) ||
            (rc = reset_reg(a, 0x1E)))
            goto fail;
        sleep_ms(10);
    }
    if ((rc = aht21_trigger(a)))
        goto fail;
    return 0;

fail:
    close(a->fd);
    a->fd = -1;
    return rc;
}

void aht21_close(aht21_t *a)
{
    if (a->fd >= 0) {
        close(a->fd);
        a->fd = -1;
    }
}

int aht21_trigger(aht21_t *a)
{
    static const uint8_t cmd[3] = { 0xAC, 0x33, 0x00 };
    int rc = wr(a, cmd, 3);

    if (rc)
        a->io_errors++;
    return rc;
}

int aht21_fetch(aht21_t *a, uint32_t *t_raw, uint32_t *rh_raw)
{
    uint8_t f[7];
    int rc = rd(a, f, 7);

    if (rc) {
        a->io_errors++;
        return rc;
    }
    rc = aht21_parse(f, t_raw, rh_raw);
    if (rc == -EBUSY)
        a->busy++;
    else if (rc == -EBADMSG)
        a->crc_errors++;
    else
        a->frames_ok++;
    return rc;
}
