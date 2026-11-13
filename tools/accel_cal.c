/*
 * accel_cal: one-time orientation calibration for the ADXL345.
 * Guides you through three poses (instructions on the terminal AND the OLED)
 * and writes config/orient.conf for autorotate / report.
 *
 *   accel_cal [-d /dev/i2c-2] [-c config/orient.conf]
 */
#include "aqm/adxl345.h"
#include "aqm/orient.h"
#include "aqm/ssd1306.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SAMPLES     40          /* averaged per pose (~0.8 s) */
#define MAX_SPREAD  0.08f       /* g: larger means the board moved */

static ssd1306_t lcd;
static int have_lcd;

static void screen(const char *l1, const char *l2, const char *l3, const char *l4)
{
    if (!have_lcd)
        return;
    ssd1306_clear(&lcd);
    ssd1306_text(&lcd, 0, 0,  l1, &font_6_10);
    ssd1306_text(&lcd, 0, 16, l2, &font_6_10);
    ssd1306_text(&lcd, 0, 30, l3, &font_6_10);
    ssd1306_text(&lcd, 0, 46, l4, &font_6_10);
    ssd1306_flush(&lcd);
}

static void wait_enter(void)
{
    int c;

    while ((c = getchar()) != '\n' && c != EOF)
        ;
}

/* average SAMPLES readings; fails if the board was moving */
static int measure(adxl345_t *a, float out[3])
{
    float lo[3] = { 9, 9, 9 }, hi[3] = { -9, -9, -9 }, sum[3] = { 0, 0, 0 };
    int i, k;

    for (i = 0; i < SAMPLES; i++) {
        float g[3];

        if (adxl345_read_g(a, &g[0], &g[1], &g[2]) != 0)
            return -EIO;
        for (k = 0; k < 3; k++) {
            sum[k] += g[k];
            if (g[k] < lo[k]) lo[k] = g[k];
            if (g[k] > hi[k]) hi[k] = g[k];
        }
        usleep(20000);
    }
    for (k = 0; k < 3; k++) {
        out[k] = sum[k] / SAMPLES;
        if (hi[k] - lo[k] > MAX_SPREAD)
            return -EAGAIN;
    }
    return 0;
}

static void pose(adxl345_t *a, int step, const char *name, const char *what1,
                 const char *what2, const char *what3, float out[3])
{
    char title[32];

    snprintf(title, sizeof(title), "CAL %d/3: %s", step, name);
    for (;;) {
        printf("\n[%d/3] %s\n  %s\n  %s\n  %s\n  Hold still, then press Enter...",
               step, name, what1, what2, what3);
        fflush(stdout);
        screen(title, what1, what2, "then press Enter");
        wait_enter();
        screen(title, "measuring...", "hold still", "");
        int rc = measure(a, out);
        if (rc == 0)
            break;
        printf("  %s - try again.\n", rc == -EAGAIN ? "board moved during measurement" : "read error");
    }
    printf("  measured: x %+.2f  y %+.2f  z %+.2f g\n", out[0], out[1], out[2]);
}

int main(int argc, char **argv)
{
    const char *dev = "/dev/i2c-2", *path = "config/orient.conf";
    float flat[3], land[3], port[3];
    aqm_orient_cal_t cal;
    adxl345_t acc;
    char err[160];
    int opt, rc;

    while ((opt = getopt(argc, argv, "d:c:")) != -1) {
        switch (opt) {
        case 'd': dev = optarg; break;
        case 'c': path = optarg; break;
        default:
            fprintf(stderr, "usage: %s [-d /dev/i2c-2] [-c config/orient.conf]\n", argv[0]);
            return 2;
        }
    }

    if ((rc = adxl345_open(&acc, dev, 0x1D)) != 0) {
        fprintf(stderr, "ADXL345 at 0x1d: %s\n", strerror(-rc));
        return 1;
    }
    have_lcd = ssd1306_open(&lcd, dev, 0x3C) == 0;

    printf("ADXL345 orientation calibration. Keep the OLED and sensor fixed together\n"
           "exactly as they will be in the finished device.\n");

    pose(&acc, 1, "FLAT", "Lay it flat on the table,", "screen facing up.", "", flat);
    pose(&acc, 2, "LANDSCAPE", "Hold it upright facing you,", "normal landscape position", "(GND edge of ADXL at top).", land);
    pose(&acc, 3, "PORTRAIT", "Hold it upright facing you,", "turned to portrait", "(ADXL345 label edge at top).", port);

    if (aqm_orient_cal_compute(&cal, flat, land, port, err, sizeof(err)) != 0) {
        printf("\nCalibration rejected: %s\nRun accel_cal again.\n", err);
        screen("CALIBRATION", "REJECTED", "see terminal", "");
        rc = 1;
        goto out;
    }

    mkdir("config", 0775);                      /* default location */
    if ((rc = aqm_orient_cal_save(&cal, path)) != 0) {
        printf("\ncannot write %s: %s\n", path, strerror(-rc));
        rc = 1;
        goto out;
    }
    printf("\nCalibration saved to %s\n", path);
    printf("  landscape direction: %+.2f %+.2f %+.2f\n", cal.land[0], cal.land[1], cal.land[2]);
    printf("  portrait  direction: %+.2f %+.2f %+.2f\n", cal.port[0], cal.port[1], cal.port[2]);
    printf("Now run: ./build/tools/autorotate -v\n");
    screen("CALIBRATION", "SAVED", path, "");
    rc = 0;

out:
    sleep(2);
    if (have_lcd)
        ssd1306_close(&lcd);
    adxl345_close(&acc);
    return rc;
}
