/*
 * autorotate: phone-style auto-rotation of the AQI screen.
 * Reads the ADXL345 at 10 Hz and turns the OLED layout between landscape
 * and portrait as the board is rotated. Shows demo air-quality values.
 *
 *   autorotate [-d /dev/i2c-2] [-o offset 0-3] [-H hold_samples] [-v]
 *
 *   -o  mounting correction if the screen ends up sideways/upside down
 *   -H  samples the new orientation must hold before switching (default 5 = 0.5 s)
 *   -v  print the gravity vector every 0.5 s (to work out the right -o)
 */
#include "aqm/adxl345.h"
#include "aqm/aqi.h"
#include "aqm/orient.h"
#include "aqm/screen.h"
#include "aqm/sig.h"
#include "aqm/ssd1306.h"
#include "aqm/timeutil.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define ADXL_ADDR  0x1D
#define OLED_ADDR  0x3C
#define PERIOD_MS  100            /* 10 Hz */

static const char *ORIENT_NAME[] = { "LANDSCAPE", "PORTRAIT", "LANDSCAPE (flipped)", "PORTRAIT (flipped)" };

/* slowly varying demo values so the screen is alive */
static void demo_sample(aqm_sample_t *s, double t)
{
    int a25, a10;

    memset(s, 0, sizeof(*s));
    s->t_wall_s = (int64_t)time(NULL);
    s->pm25_cal = (float)(45.0 + 15.0 * sin(t / 20.0));
    s->pm10_cal = s->pm25_cal * 1.6f;
    s->tvoc_ppb = (uint16_t)(150 + 30 * sin(t / 13.0));
    s->temp_c   = (float)(28.0 + 0.5 * sin(t / 30.0));
    s->rh_pct   = (float)(55.0 + 3.0 * sin(t / 25.0));
    a25 = aqm_aqi_pm25(s->pm25_cal);
    a10 = aqm_aqi_pm10(s->pm10_cal);
    s->aqi = (uint16_t)(a25 > a10 ? a25 : a10);
    s->aqi_cat = (uint8_t)aqm_aqi_category(s->aqi);
}

int main(int argc, char **argv)
{
    const char *dev = "/dev/i2c-2";
    int opt, offset = 0, hold = 5, verbose = 0, rc, n = 0;
    struct timespec next;
    aqm_orient_t o;
    aqm_sample_t s;
    adxl345_t acc;
    ssd1306_t lcd;

    while ((opt = getopt(argc, argv, "d:o:H:v")) != -1) {
        switch (opt) {
        case 'd': dev = optarg; break;
        case 'o': offset = atoi(optarg); break;
        case 'H': hold = atoi(optarg); break;
        case 'v': verbose = 1; break;
        default:
            fprintf(stderr, "usage: %s [-d /dev/i2c-2] [-o offset 0-3] [-H hold] [-v]\n", argv[0]);
            return 2;
        }
    }

    aqm_signals_init();
    if ((rc = adxl345_open(&acc, dev, ADXL_ADDR)) != 0) {
        fprintf(stderr, "ADXL345 at 0x%02x: %s\n", ADXL_ADDR, strerror(-rc));
        return 1;
    }
    if ((rc = ssd1306_open(&lcd, dev, OLED_ADDR)) != 0) {
        fprintf(stderr, "OLED at 0x%02x: %s\n", OLED_ADDR, strerror(-rc));
        adxl345_close(&acc);
        return 1;
    }

    aqm_orient_init(&o, hold, offset);
    ssd1306_set_rotation(&lcd, o.current);
    fprintf(stderr, "auto-rotate running (hold %d samples = %d ms). Rotate the board; Ctrl+C to stop.\n",
            hold, hold * PERIOD_MS);

    clock_gettime(CLOCK_MONOTONIC, &next);
    while (!aqm_should_stop()) {
        float gx, gy, gz;

        if ((rc = adxl345_read_g(&acc, &gx, &gy, &gz)) != 0) {
            fprintf(stderr, "accel read: %s\n", strerror(-rc));
        } else {
            if (aqm_orient_update(&o, gx, gy, gz)) {
                ssd1306_set_rotation(&lcd, o.current);
                fprintf(stderr, "-> %s (g: x %+.2f y %+.2f z %+.2f)\n",
                        ORIENT_NAME[o.current], gx, gy, gz);
            }
            if (verbose && n % 5 == 0)
                fprintf(stderr, "g: x %+.2f y %+.2f z %+.2f  raw=%d  showing %s\n",
                        gx, gy, gz, aqm_orient_classify(gx, gy, gz), ORIENT_NAME[o.current]);
        }

        /* redraw every sample: rotation changes appear within one frame */
        demo_sample(&s, n * PERIOD_MS / 1000.0);
        aqm_screen_draw(&lcd, &s);
        if ((rc = ssd1306_flush(&lcd)) != 0)
            fprintf(stderr, "OLED write: %s\n", strerror(-rc));

        n++;
        ts_add_ns(&next, (int64_t)PERIOD_MS * 1000000LL);
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    }

    ssd1306_close(&lcd);
    adxl345_close(&acc);
    fprintf(stderr, "stopped\n");
    return 0;
}
