/*
 * autorotate: landscape / portrait switching of the AQI screen from the
 * ADXL345, using the calibration written by accel_cal.
 *
 *   autorotate [-d /dev/i2c-2] [-c config/orient.conf] [-H hold] [-v]
 *
 *   -H  samples the new state must persist (default 5 = 0.5 s at 10 Hz)
 *   -v  print angle-to-portrait every 0.5 s
 *
 * States: LANDSCAPE, PORTRAIT (calibrated pose), PORTRAIT (180) (calibrated
 * pose turned upside down). Display rotation per state comes from orient.conf
 * (landscape_rot / portrait_rot / portrait2_rot).
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

#define PERIOD_MS  100            /* 10 Hz */

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
    const char *dev = "/dev/i2c-2", *path = "config/orient.conf";
    int opt, hold = 5, verbose = 0, rc, n = 0;
    struct timespec next;
    aqm_orient_cal_t cal;
    aqm_orient_t o;
    aqm_sample_t s;
    adxl345_t acc;
    ssd1306_t lcd;

    while ((opt = getopt(argc, argv, "d:c:H:v")) != -1) {
        switch (opt) {
        case 'd': dev = optarg; break;
        case 'c': path = optarg; break;
        case 'H': hold = atoi(optarg); break;
        case 'v': verbose = 1; break;
        default:
            fprintf(stderr, "usage: %s [-d /dev/i2c-2] [-c config/orient.conf] [-H hold] [-v]\n", argv[0]);
            return 2;
        }
    }

    if ((rc = aqm_orient_cal_load(&cal, path)) != 0) {
        fprintf(stderr, "no calibration in %s (%s).\nRun ./build/tools/accel_cal first.\n",
                path, strerror(-rc));
        return 1;
    }

    aqm_signals_init();
    if ((rc = adxl345_open(&acc, dev, 0x1D)) != 0) {
        fprintf(stderr, "ADXL345 at 0x1d: %s\n", strerror(-rc));
        return 1;
    }
    if ((rc = ssd1306_open(&lcd, dev, 0x3C)) != 0) {
        fprintf(stderr, "OLED at 0x3c: %s\n", strerror(-rc));
        adxl345_close(&acc);
        return 1;
    }

    aqm_orient_init(&o, &cal, hold);
    ssd1306_set_rotation(&lcd, aqm_orient_rotation(&o));
    fprintf(stderr, "autorotate: portrait within %.0f deg of the calibrated pose or its 180 deg "
            "flip, back to landscape beyond %.0f deg, hold %d ms. Ctrl+C to stop.\n",
            AQM_ORIENT_ENTER_DEG, AQM_ORIENT_EXIT_DEG, hold * PERIOD_MS);

    clock_gettime(CLOCK_MONOTONIC, &next);
    while (!aqm_should_stop()) {
        float gx, gy, gz;

        if ((rc = adxl345_read_g(&acc, &gx, &gy, &gz)) != 0) {
            fprintf(stderr, "accel read: %s\n", strerror(-rc));
        } else {
            if (aqm_orient_update(&o, gx, gy, gz)) {
                ssd1306_set_rotation(&lcd, aqm_orient_rotation(&o));
                fprintf(stderr, "-> %s\n", aqm_orient_name(o.state));
            }
            if (verbose && n % 5 == 0) {
                float ang = aqm_orient_angle(&o, gx, gy, gz);

                if (ang < 0.0f)
                    fprintf(stderr, "flat (no change)            showing %s\n",
                            aqm_orient_name(o.state));
                else
                    fprintf(stderr, "%3.0f deg from portrait pose  showing %s\n",
                            ang, aqm_orient_name(o.state));
            }
        }

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
