/*
 * ENS160 + AHT21 drivers against simulated chips.
 * The test is linked with -Wl,--wrap=read,--wrap=write,--wrap=ioctl: the
 * drivers' i2c-dev calls land in the fake devices below, so their real
 * init / read sequences run without hardware.
 */
#include "aqm/aht21.h"
#include "aqm/ens160.h"

#include <errno.h>
#include <linux/i2c-dev.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
    return 1; } } while (0)

ssize_t __real_read(int fd, void *buf, size_t n);
ssize_t __real_write(int fd, const void *buf, size_t n);
int     __real_ioctl(int fd, unsigned long req, ...);

/* ------------------------------ fake bus --------------------------------- */
static int     fd_addr[64];               /* slave address per fd, 0 = real fd */
static uint8_t ens_at = 0x53, aht_present = 1;

/* ENS160 model */
static uint8_t ens_reg[256], ens_ptr, ens_opmode;
static int     ens_status_reads, ens_newdat;
static uint8_t ens_temp_in[4];

/* AHT21 model */
static uint8_t aht_status = 0x18, aht_frame[7], aht_last_cmd, aht_regbuf[3];
static int     aht_busy, aht_bad_crc, aht_resets, aht_triggers;
static double  aht_t = 25.3, aht_rh = 47.5;

static void aht_make_frame(void)
{
    uint32_t rh = (uint32_t)(aht_rh / 100.0 * 1048576.0);
    uint32_t t  = (uint32_t)((aht_t + 50.0) / 200.0 * 1048576.0);

    aht_frame[0] = (uint8_t)(0x18 | (aht_busy ? 0x80 : 0));
    aht_frame[1] = (uint8_t)(rh >> 12);
    aht_frame[2] = (uint8_t)(rh >> 4);
    aht_frame[3] = (uint8_t)(((rh & 0x0F) << 4) | ((t >> 16) & 0x0F));
    aht_frame[4] = (uint8_t)(t >> 8);
    aht_frame[5] = (uint8_t)t;
    aht_frame[6] = aht21_crc8(aht_frame, 6) ^ (aht_bad_crc ? 0x5A : 0);
}

/* 32-bit Debian 13 (armhf) builds with 64-bit time_t, where glibc redirects
 * ioctl() to __ioctl_time64(): intercept both names */
int __wrap___ioctl_time64(int fd, unsigned long req, ...);

static int fake_ioctl(int fd, unsigned long req, unsigned long arg);

int __wrap_ioctl(int fd, unsigned long req, ...)
{
    va_list ap;
    unsigned long arg;

    va_start(ap, req);
    arg = va_arg(ap, unsigned long);
    va_end(ap);
    return fake_ioctl(fd, req, arg);
}

int __wrap___ioctl_time64(int fd, unsigned long req, ...)
{
    va_list ap;
    unsigned long arg;

    va_start(ap, req);
    arg = va_arg(ap, unsigned long);
    va_end(ap);
    return fake_ioctl(fd, req, arg);
}

static int fake_ioctl(int fd, unsigned long req, unsigned long arg)
{
    if (req == I2C_SLAVE && fd >= 0 && fd < 64) {
        fd_addr[fd] = (int)arg;
        return 0;
    }
    return __real_ioctl(fd, req, arg);
}

static ssize_t nack(void) { errno = EREMOTEIO; return -1; }

ssize_t __wrap_write(int fd, const void *buf, size_t n)
{
    const uint8_t *b = buf;
    int a = (fd >= 0 && fd < 64) ? fd_addr[fd] : 0;

    if (!a)
        return __real_write(fd, buf, n);
    if (a == ens_at) {
        ens_ptr = b[0];
        if (n >= 2) {
            size_t i;
            for (i = 1; i < n; i++)
                ens_reg[(uint8_t)(b[0] + i - 1)] = b[i];
            if (b[0] == 0x10) {                     /* OPMODE */
                ens_opmode = b[1];
                if (b[1] == 0xF0) { ens_status_reads = 0; ens_opmode = 0; }
            }
            if (b[0] == 0x13 && n == 5)
                memcpy(ens_temp_in, &b[1], 4);
        }
        return (ssize_t)n;
    }
    if (a == 0x38 && aht_present) {
        aht_last_cmd = b[0];
        if (b[0] == 0xAC && n == 3) {
            aht_triggers++;
            aht_make_frame();
        }
        if ((b[0] & 0xF0) == 0xB0)
            aht_resets++;
        return (ssize_t)n;
    }
    return nack();
}

ssize_t __wrap_read(int fd, void *buf, size_t n)
{
    uint8_t *b = buf;
    int a = (fd >= 0 && fd < 64) ? fd_addr[fd] : 0;

    if (!a)
        return __real_read(fd, buf, n);
    if (a == ens_at) {
        if (ens_ptr == 0x00) {                       /* PART_ID */
            b[0] = 0x60; b[1] = 0x01;
        } else if (ens_ptr == 0x20) {                /* STATUS */
            int validity = ens_status_reads < 3 ? 1 : 0;   /* warm-up, then valid */
            ens_status_reads++;
            ens_newdat = ens_opmode == 0x02;
            b[0] = (uint8_t)((ens_opmode == 0x02 ? 0x80 : 0) | (validity << 2) |
                             (ens_newdat ? 0x02 : 0));
        } else if (ens_ptr == 0x21) {                /* AQI TVOC ECO2 */
            static const uint8_t d[5] = { 0x02, 0x9A, 0x00, 0x8C, 0x02 };  /* UBA 2, 154 ppb, 652 ppm */
            memcpy(b, d, n < 5 ? n : 5);
            ens_newdat = 0;
        } else {
            memcpy(b, &ens_reg[ens_ptr], n);
        }
        return (ssize_t)n;
    }
    if (a == 0x38 && aht_present) {
        if (aht_last_cmd == 0x71 && n == 1)
            b[0] = aht_status;
        else if (n == 3)
            memcpy(b, aht_regbuf, 3);
        else if (n == 7)
            memcpy(b, aht_frame, 7);
        return (ssize_t)n;
    }
    return nack();
}

/* --------------------------------- tests --------------------------------- */
int main(void)
{
    static const uint8_t check[] = "123456789";
    ens160_t e;
    ens160_data_t d;
    aht21_t a;
    uint32_t t, rh;
    uint8_t env[4];
    int i, rc, got_warm = 0, got_valid = 0;

    /* CRC-8 (poly 0x31, init 0xFF) check value */
    CHECK(aht21_crc8(check, 9) == 0xF7);

    /* compensation encoding: 25 C -> (298.15 * 64) = 19081, 50 % -> 25600 */
    ens160_encode_env(25.0f, 50.0f, env);
    CHECK(env[0] == 0x89 && env[1] == 0x4A && env[2] == 0x00 && env[3] == 0x64);

    /* ENS160 at 0x53: probe, reset, standard mode, warm-up then valid data */
    CHECK(ens160_open(&e, "/dev/null", 0) == 0 && e.addr == 0x53);
    CHECK(ens_opmode == 0x02);
    for (i = 0; i < 5; i++) {
        rc = ens160_read(&e, &d);
        CHECK(rc == 1);
        CHECK(d.tvoc_ppb == 154 && d.eco2_ppm == 652 && d.aqi_uba == 2);
        if (d.validity == ENS160_WARMUP) got_warm = 1;
        if (d.validity == ENS160_VALID)  got_valid = 1;
    }
    CHECK(got_warm && got_valid);
    CHECK(ens160_set_env(&e, 25.0f, 50.0f) == 0);
    CHECK(!memcmp(ens_temp_in, env, 4));
    ens160_close(&e);
    CHECK(ens_opmode == 0x00);                       /* left asleep */

    /* ENS160 strapped to 0x52 is found by the fallback probe */
    ens_at = 0x52;
    CHECK(ens160_open(&e, "/dev/null", 0) == 0 && e.addr == 0x52);
    ens160_close(&e);

    /* no ENS160 at all */
    ens_at = 0x00;
    CHECK(ens160_open(&e, "/dev/null", 0) == -ENODEV && e.fd == -1);
    ens_at = 0x53;

    /* AHT21 needing its calibration registers restored (status 0x00) */
    aht_status = 0x00;
    CHECK(aht21_open(&a, "/dev/null") == 0);
    CHECK(aht_resets == 3 && aht_triggers == 1);
    aht21_close(&a);

    /* normal AHT21: fetch previous measurement, trigger next */
    aht_status = 0x18;
    aht_resets = aht_triggers = 0;
    CHECK(aht21_open(&a, "/dev/null") == 0 && aht_resets == 0);
    CHECK(aht21_fetch(&a, &t, &rh) == 0);
    CHECK(fabsf(aht21_temp_c(t) - 25.3f) < 0.01f && fabsf(aht21_rh(rh) - 47.5f) < 0.01f);
    CHECK(aht21_trigger(&a) == 0 && aht_triggers == 2);

    aht_busy = 1; aht_make_frame();
    CHECK(aht21_fetch(&a, &t, &rh) == -EBUSY && a.busy == 1);
    aht_busy = 0; aht_bad_crc = 1; aht_make_frame();
    CHECK(aht21_fetch(&a, &t, &rh) == -EBADMSG && a.crc_errors == 1);
    aht_bad_crc = 0; aht_make_frame();
    CHECK(aht21_fetch(&a, &t, &rh) == 0 && a.frames_ok == 2);

    /* sensor unplugged: bus errors, not garbage */
    aht_present = 0;
    CHECK(aht21_fetch(&a, &t, &rh) == -EREMOTEIO && a.io_errors == 1);
    aht21_close(&a);
    CHECK(aht21_open(&a, "/dev/null") == -EREMOTEIO && a.fd == -1);

    printf("i2c sensors ok\n");
    return 0;
}
