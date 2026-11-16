/*
 * Simulated ENS160 (0x53) + AHT21 (0x38) for running the real `sense` binary
 * without hardware. Link with -Wl,--wrap=read,--wrap=write,--wrap=ioctl and
 * run with -I /dev/null. Which chips are "plugged in" is read from the file
 * named by $FAKE_I2C (words "ens" and/or "aht"), so they can be plugged and
 * unplugged while the program runs.
 */
#include "aqm/aht21.h"

#include <errno.h>
#include <linux/i2c-dev.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

ssize_t __real_read(int fd, void *buf, size_t n);
ssize_t __real_write(int fd, const void *buf, size_t n);
int     __real_ioctl(int fd, unsigned long req, ...);

static int     fd_addr[256];
static uint8_t ens_ptr, ens_opmode, aht_cmd, aht_frame[7];
static int     ens_reads;

static int present(const char *what)
{
    char buf[64] = "";
    const char *p = getenv("FAKE_I2C");
    FILE *f = p ? fopen(p, "r") : NULL;

    if (f) {
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        buf[n] = 0;
        fclose(f);
    }
    return strstr(buf, what) != NULL;
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
    if (req == I2C_SLAVE && fd >= 0 && fd < 256) {
        fd_addr[fd] = (int)arg;
        return 0;
    }
    return __real_ioctl(fd, req, arg);
}

static ssize_t nack(void) { errno = EREMOTEIO; return -1; }

ssize_t __wrap_write(int fd, const void *buf, size_t n)
{
    const uint8_t *b = buf;
    int a = (fd >= 0 && fd < 256) ? fd_addr[fd] : 0;

    if (!a)
        return __real_write(fd, buf, n);
    if (a == 0x53 && present("ens")) {
        ens_ptr = b[0];
        if (b[0] == 0x10 && n >= 2) {
            ens_opmode = b[1];
            if (b[1] == 0xF0) ens_reads = 0;
        }
        return (ssize_t)n;
    }
    if (a == 0x38 && present("aht")) {
        aht_cmd = b[0];
        if (b[0] == 0xAC) {                         /* 26.4 C, 48.0 %RH */
            uint32_t rh = (uint32_t)(48.0 / 100.0 * 1048576.0);
            uint32_t t  = (uint32_t)((26.4 + 50.0) / 200.0 * 1048576.0);
            aht_frame[0] = 0x18;
            aht_frame[1] = (uint8_t)(rh >> 12);
            aht_frame[2] = (uint8_t)(rh >> 4);
            aht_frame[3] = (uint8_t)(((rh & 0x0F) << 4) | ((t >> 16) & 0x0F));
            aht_frame[4] = (uint8_t)(t >> 8);
            aht_frame[5] = (uint8_t)t;
            aht_frame[6] = aht21_crc8(aht_frame, 6);
        }
        return (ssize_t)n;
    }
    return nack();
}

ssize_t __wrap_read(int fd, void *buf, size_t n)
{
    uint8_t *b = buf;
    int a = (fd >= 0 && fd < 256) ? fd_addr[fd] : 0;

    if (!a)
        return __real_read(fd, buf, n);
    if (a == 0x53 && present("ens")) {
        if (ens_ptr == 0x00) { b[0] = 0x60; b[1] = 0x01; }
        else if (ens_ptr == 0x20) {
            int validity = ens_reads < 4 ? 1 : 0;     /* 4 s "warm-up", then valid */
            ens_reads++;
            b[0] = (uint8_t)((ens_opmode == 0x02 ? 0x82 : 0) | (validity << 2));
        } else if (ens_ptr == 0x21) {                 /* UBA 2, 212 ppb, 690 ppm */
            uint8_t d[5] = { 2, 212, 0, 0xB2, 0x02 };
            memcpy(b, d, n < 5 ? n : 5);
        } else memset(b, 0, n);
        return (ssize_t)n;
    }
    if (a == 0x38 && present("aht")) {
        if (n == 1) b[0] = 0x18;
        else memcpy(b, aht_frame, n < 7 ? n : 7);
        return (ssize_t)n;
    }
    return nack();
}
