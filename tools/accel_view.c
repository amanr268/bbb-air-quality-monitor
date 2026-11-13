/*
 * accel_view: live ADXL345 readings on the OLED (sensor + display check).
 *
 *   accel_view [-d /dev/i2c-2] [-a 0x1d] [-p pbm_dir]
 *
 * Screen (10 updates/s):
 *   ADXL345 @1D  10Hz
 *   X +0.02 |-----|##      bar: -1 g ... 0 ... +1 g
 *   Y -0.01 |---#|
 *   Z +1.00 |    |#####
 *   |g| 1.00  FLAT
 *
 * -p DIR: no hardware; renders frames with simulated motion to PBM files.
 */
#include "aqm/adxl345.h"
#include "aqm/orient.h"
#include "aqm/sig.h"
#include "aqm/ssd1306.h"
#include "aqm/timeutil.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define BAR_X0    54          /* bar area: x 54..127                */
#define BAR_W     74
#define BAR_H     8
#define BAR_ZERO  (BAR_X0 + BAR_W / 2)
#define BAR_PX_G  ((BAR_W / 2) - 1)     /* pixels per 1 g           */

static const char *ORIENT_SHORT[] = { "LAND", "PORT", "LAND-F", "PORT-F" };

static void bar(ssd1306_t *d, int y, float g)
{
    int len = (int)lroundf(g * BAR_PX_G);

    if (len > BAR_PX_G)  len = BAR_PX_G;      /* clip beyond +-1 g */
    if (len < -BAR_PX_G) len = -BAR_PX_G;

    ssd1306_rect(d, BAR_X0, y, BAR_W, BAR_H, 0);                 /* frame       */
    ssd1306_line(d, BAR_ZERO, y - 1, BAR_ZERO, y + BAR_H);       /* zero marker */
    if (len > 0)
        ssd1306_rect(d, BAR_ZERO + 1, y + 2, len, BAR_H - 4, 1);
    else if (len < 0)
        ssd1306_rect(d, BAR_ZERO + len, y + 2, -len, BAR_H - 4, 1);
}

static void draw(ssd1306_t *d, unsigned addr, float gx, float gy, float gz)
{
    const aqm_font_t *f = &font_6_10;
    float mag = sqrtf(gx * gx + gy * gy + gz * gz);
    int o = aqm_orient_classify(gx, gy, gz);
    char buf[32];

    ssd1306_clear(d);
    snprintf(buf, sizeof(buf), "ADXL345 @%02X  10Hz", addr);
    ssd1306_text(d, 0, 0, buf, f);

    snprintf(buf, sizeof(buf), "X%+5.2f", gx);
    ssd1306_text(d, 0, 13, buf, f);
    bar(d, 14, gx);
    snprintf(buf, sizeof(buf), "Y%+5.2f", gy);
    ssd1306_text(d, 0, 26, buf, f);
    bar(d, 27, gy);
    snprintf(buf, sizeof(buf), "Z%+5.2f", gz);
    ssd1306_text(d, 0, 39, buf, f);
    bar(d, 40, gz);

    snprintf(buf, sizeof(buf), "|g| %4.2f  %s", mag, o < 0 ? "FLAT/45" : ORIENT_SHORT[o]);
    ssd1306_text(d, 0, 54, buf, f);
}

static int dump_pbm(const ssd1306_t *d, const char *dir, int n)
{
    char path[512];
    FILE *fp;
    int x, y;

    snprintf(path, sizeof(path), "%s/frame_%04d.pbm", dir, n);
    if (!(fp = fopen(path, "w")))
        return -errno;
    fprintf(fp, "P1\n%d %d\n", SSD1306_W, SSD1306_H);
    for (y = 0; y < SSD1306_H; y++) {
        for (x = 0; x < SSD1306_W; x++)
            fputc((d->fb[(y / 8) * SSD1306_W + x] >> (y % 8)) & 1 ? '1' : '0', fp);
        fputc('\n', fp);
    }
    fclose(fp);
    return 0;
}

int main(int argc, char **argv)
{
    const char *dev = "/dev/i2c-2", *pbm = NULL;
    unsigned addr = 0x1D;
    int opt, rc, n = 0;
    struct timespec next;
    adxl345_t acc = { .fd = -1 };
    ssd1306_t lcd;

    while ((opt = getopt(argc, argv, "d:a:p:")) != -1) {
        switch (opt) {
        case 'd': dev = optarg; break;
        case 'a': addr = (unsigned)strtoul(optarg, NULL, 0); break;
        case 'p': pbm = optarg; break;
        default:
            fprintf(stderr, "usage: %s [-d /dev/i2c-2] [-a 0x1d] [-p pbm_dir]\n", argv[0]);
            return 2;
        }
    }

    aqm_signals_init();
    memset(&lcd, 0, sizeof(lcd));
    lcd.fd = -1;

    if (!pbm) {
        if ((rc = adxl345_open(&acc, dev, (uint8_t)addr)) != 0) {
            fprintf(stderr, "ADXL345 at 0x%02x: %s%s\n", addr, strerror(-rc),
                    rc == -ENODEV ? " (wrong chip ID)" : "");
            return 1;
        }
        if ((rc = ssd1306_open(&lcd, dev, 0x3C)) != 0) {
            fprintf(stderr, "OLED at 0x3c: %s\n", strerror(-rc));
            adxl345_close(&acc);
            return 1;
        }
        fprintf(stderr, "showing live X/Y/Z on the OLED; tilt the board. Ctrl+C to stop.\n");
    }

    clock_gettime(CLOCK_MONOTONIC, &next);
    while (!aqm_should_stop() && (!pbm || n < 40)) {
        float gx, gy, gz;

        if (pbm) {                              /* simulated slow tumble */
            double a = n * 0.12;
            gx = (float)(sin(a) * cos(a * 0.5));
            gy = (float)(-cos(a) * cos(a * 0.5));
            gz = (float)sin(a * 0.5);
        } else if ((rc = adxl345_read_g(&acc, &gx, &gy, &gz)) != 0) {
            fprintf(stderr, "accel read: %s\n", strerror(-rc));
            gx = gy = gz = 0.0f;
        }

        draw(&lcd, addr, gx, gy, gz);
        if (pbm) {
            dump_pbm(&lcd, pbm, n);
        } else {
            if ((rc = ssd1306_flush(&lcd)) != 0)
                fprintf(stderr, "OLED write: %s\n", strerror(-rc));
            if (n % 10 == 0)                    /* also print once a second */
                printf("x %+.2f  y %+.2f  z %+.2f  |g| %.2f\n", gx, gy, gz,
                       sqrtf(gx * gx + gy * gy + gz * gz));
            fflush(stdout);
            ts_add_ns(&next, 100000000LL);      /* 10 Hz */
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
        }
        n++;
    }

    if (!pbm) {
        ssd1306_close(&lcd);
        adxl345_close(&acc);
    }
    return 0;
}
