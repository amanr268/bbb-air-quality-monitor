#include "aqm/ens160.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define REG_PART_ID     0x00     /* 2 bytes, LE, 0x0160 */
#define REG_OPMODE      0x10
#define REG_COMMAND     0x12
#define REG_TEMP_IN     0x13     /* 2 bytes: (T + 273.15) * 64 */
#define REG_STATUS      0x20
#define REG_DATA_AQI    0x21     /* AQI, TVOC (2), ECO2 (2): 5 bytes */

#define OP_SLEEP        0x00
#define OP_IDLE         0x01
#define OP_STANDARD     0x02
#define OP_RESET        0xF0
#define CMD_NOP         0x00
#define CMD_CLRGPR      0xCC

#define ST_ERROR        0x40
#define ST_NEWDAT       0x02

static void sleep_ms(int ms)
{
    struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000L };

    nanosleep(&t, NULL);
}

static int wr(ens160_t *e, const uint8_t *b, int n)
{
    return write(e->fd, b, (size_t)n) == n ? 0 : -errno;
}

static int wr1(ens160_t *e, uint8_t reg, uint8_t val)
{
    uint8_t b[2] = { reg, val };

    return wr(e, b, 2);
}

static int rd(ens160_t *e, uint8_t reg, uint8_t *buf, int n)
{
    if (write(e->fd, &reg, 1) != 1)
        return -errno;
    return read(e->fd, buf, (size_t)n) == n ? 0 : -errno;
}

static int probe(ens160_t *e, uint8_t addr)
{
    uint8_t id[2];

    if (ioctl(e->fd, I2C_SLAVE, addr) < 0)
        return -errno;
    if (rd(e, REG_PART_ID, id, 2))
        return -ENODEV;                       /* NACK: nothing there */
    if ((id[0] | (id[1] << 8)) != 0x0160)
        return -ENODEV;                       /* some other chip */
    e->addr = addr;
    return 0;
}

int ens160_open(ens160_t *e, const char *i2c_dev, uint8_t addr)
{
    int rc;

    e->reads_ok = e->io_errors = 0;
    e->last_status = 0;
    e->fd = open(i2c_dev, O_RDWR | O_CLOEXEC);
    if (e->fd < 0)
        return -errno;

    if (addr)
        rc = probe(e, addr);
    else if ((rc = probe(e, ENS160_ADDR)) != 0)
        rc = probe(e, ENS160_ADDR_ALT);
    if (rc)
        goto fail;

    /* reset -> idle -> clear GPR -> standard (1 Hz) */
    if ((rc = wr1(e, REG_OPMODE, OP_RESET)))
        goto fail;
    sleep_ms(20);
    if ((rc = wr1(e, REG_OPMODE, OP_IDLE)))
        goto fail;
    sleep_ms(10);
    if ((rc = wr1(e, REG_COMMAND, CMD_NOP)) || (rc = wr1(e, REG_COMMAND, CMD_CLRGPR)))
        goto fail;
    sleep_ms(10);
    if ((rc = wr1(e, REG_OPMODE, OP_STANDARD)))
        goto fail;
    sleep_ms(50);
    return 0;

fail:
    close(e->fd);
    e->fd = -1;
    return rc;
}

void ens160_close(ens160_t *e)
{
    if (e->fd >= 0) {
        wr1(e, REG_OPMODE, OP_SLEEP);
        close(e->fd);
        e->fd = -1;
    }
}

void ens160_encode_env(float temp_c, float rh_pct, uint8_t out[4])
{
    float t = (temp_c + 273.15f) * 64.0f;
    float h = rh_pct * 512.0f;
    uint16_t ti, hi;

    if (t < 0) t = 0;
    if (t > 65535) t = 65535;
    if (h < 0) h = 0;
    if (h > 51200) h = 51200;                 /* 100 %RH */
    ti = (uint16_t)t;
    hi = (uint16_t)h;
    out[0] = (uint8_t)(ti & 0xFF);
    out[1] = (uint8_t)(ti >> 8);
    out[2] = (uint8_t)(hi & 0xFF);
    out[3] = (uint8_t)(hi >> 8);
}

void ens160_parse_data(const uint8_t b[5], ens160_data_t *d)
{
    d->aqi_uba  = b[0] & 0x07;
    d->tvoc_ppb = (uint16_t)(b[1] | (b[2] << 8));
    d->eco2_ppm = (uint16_t)(b[3] | (b[4] << 8));
}

int ens160_set_env(ens160_t *e, float temp_c, float rh_pct)
{
    uint8_t b[5] = { REG_TEMP_IN };
    int rc;

    ens160_encode_env(temp_c, rh_pct, &b[1]);   /* TEMP_IN, RH_IN: 0x13..0x16 */
    rc = wr(e, b, 5);
    if (rc)
        e->io_errors++;
    return rc;
}

int ens160_read(ens160_t *e, ens160_data_t *d)
{
    uint8_t st, b[5];
    int rc;

    if ((rc = rd(e, REG_STATUS, &st, 1))) {
        e->io_errors++;
        return rc;
    }
    e->last_status = st;
    d->validity = (uint8_t)((st >> 2) & 0x03);
    if (st & ST_ERROR)
        return -EIO;
    if (!(st & ST_NEWDAT))
        return 0;
    if ((rc = rd(e, REG_DATA_AQI, b, 5))) {
        e->io_errors++;
        return rc;
    }
    ens160_parse_data(b, d);
    e->reads_ok++;
    return 1;
}

const char *ens160_validity_name(int v)
{
    static const char *n[] = { "valid", "warm-up", "initial start-up", "invalid" };

    return (v >= 0 && v <= 3) ? n[v] : "?";
}
