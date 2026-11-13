#include "aqm/adxl345.h"
#include "aqm/alarm.h"
#include "aqm/aqi.h"
#include "aqm/log.h"
#include "aqm/orient.h"
#include "aqm/proc.h"
#include "aqm/screen.h"
#include "aqm/shm.h"
#include "aqm/sig.h"
#include "aqm/ssd1306.h"
#include "aqm/timeutil.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define I2C_DEV         "/dev/i2c-2"
#define OLED_ADDR       0x3C
#define ADXL_ADDR       0x1D
#define TICK_MS         100           /* loop period: accel + ring poll, 10 Hz   */
#define BANNER_MS       10000         /* how long an alert stays on screen       */
#define ORIENT_HOLD     5             /* samples (= 0.5 s at 10 Hz)              */

/* local display state: everything optional, the pipeline runs without it */
typedef struct {
    ssd1306_t    lcd;
    adxl345_t    acc;
    aqm_orient_t orient;
    int          have_lcd, have_acc;
    aqm_sample_t last;                /* last sample shown                       */
    int          have_sample;
    aqm_banner_t banner;              /* alert shown on screen (kind NONE = none) */
    int64_t      banner_until_ns;
    int          dirty;               /* redraw needed                           */
} display_t;

static void display_init(display_t *dp)
{
    const char *cal_path = getenv("AQM_ORIENT") ? getenv("AQM_ORIENT") : "config/orient.conf";
    aqm_orient_cal_t cal;
    int rc;

    memset(dp, 0, sizeof(*dp));
    dp->acc.fd = -1;

    if ((rc = ssd1306_open(&dp->lcd, I2C_DEV, OLED_ADDR)) != 0) {
        aqm_log("report", "OLED not available (%s): text-only mode", strerror(-rc));
        return;
    }
    dp->have_lcd = 1;

    if ((rc = aqm_orient_cal_load(&cal, cal_path)) != 0) {
        aqm_log("report", "no orientation calibration (%s): fixed landscape. "
                "Run build/tools/accel_cal to enable auto-rotate", cal_path);
    } else if ((rc = adxl345_open(&dp->acc, I2C_DEV, ADXL_ADDR)) != 0) {
        aqm_log("report", "ADXL345 not available (%s): fixed landscape", strerror(-rc));
    } else {
        aqm_orient_init(&dp->orient, &cal, ORIENT_HOLD);
        dp->have_acc = 1;
        aqm_log("report", "OLED + auto-rotate enabled (%s)", cal_path);
    }
    ssd1306_set_rotation(&dp->lcd, dp->have_acc ? aqm_orient_rotation(&dp->orient) : 0);

    /* placeholder until the first sample arrives */
    ssd1306_clear(&dp->lcd);
    ssd1306_text(&dp->lcd, 0, 0, "AQM starting...", &font_6_10);
    ssd1306_flush(&dp->lcd);
}

static void display_close(display_t *dp)
{
    if (dp->have_lcd)
        ssd1306_close(&dp->lcd);
    if (dp->have_acc)
        adxl345_close(&dp->acc);
}

/* on-screen alert for an alarm (warnings and critical only) */
static int alarm_banner(const aqm_alarm_t *a, aqm_banner_t *b)
{
    if (a->prio < AQM_PRIO_WARN)
        return 0;
    switch (a->type) {
    case AQM_ALM_AQI:
        b->kind = AQM_BANNER_AQI;
        b->aqi_cat = (int)aqm_aqi_category(a->value);
        break;
    case AQM_ALM_FAULT:
        b->kind = AQM_BANNER_FAULT;
        break;
    case AQM_ALM_TVOC:
        b->kind = AQM_BANNER_TVOC;
        break;
    default:
        return 0;
    }
    return 1;
}

/* log every queued alarm (the mqueue delivers highest priority first) */
static void drain_alarms(mqd_t q, display_t *dp)
{
    aqm_alarm_t a;

    while (aqm_alarm_recv(q, &a) == 0) {
        aqm_log("report", "ALARM p%-2u seq %u: %s", a.prio, a.seq, a.text);
        if (dp->have_lcd && alarm_banner(&a, &dp->banner)) {
            dp->banner_until_ns = mono_ns() + (int64_t)BANNER_MS * 1000000LL;
            dp->dirty = 1;
        }
    }
}

static void display_tick(display_t *dp)
{
    float gx, gy, gz;
    int rc;

    if (!dp->have_lcd)
        return;

    if (dp->have_acc && adxl345_read_g(&dp->acc, &gx, &gy, &gz) == 0 &&
        aqm_orient_update(&dp->orient, gx, gy, gz)) {
        ssd1306_set_rotation(&dp->lcd, aqm_orient_rotation(&dp->orient));
        aqm_log("report", "display -> %s", aqm_orient_name(dp->orient.state));
        dp->dirty = 1;
    }

    if (dp->banner.kind != AQM_BANNER_NONE && mono_ns() >= dp->banner_until_ns) {
        dp->banner.kind = AQM_BANNER_NONE;
        dp->dirty = 1;
    }

    if (dp->dirty && dp->have_sample) {
        aqm_screen_draw(&dp->lcd, &dp->last, &dp->banner);
        if ((rc = ssd1306_flush(&dp->lcd)) != 0)
            aqm_log("report", "OLED write failed: %s", strerror(-rc));
        dp->dirty = 0;
    }
}

int main(int argc, char **argv)
{
    int prio = aqm_parse_prio(argc, argv, 0);
    display_t disp;
    aqm_shm_t *shm;
    mqd_t q;

    aqm_signals_init();
    shm = aqm_attach_wait("report");
    if (!shm)
        return 0;
    aqm_rt_setup("report", prio);

    q = aqm_alarm_open(1);
    if (q == (mqd_t)-1) {
        aqm_log("report", "mq_open failed: %s", strerror(errno));
        aqm_shm_detach(shm);
        return 1;
    }

    display_init(&disp);

    while (!aqm_should_stop() && aqm_running(shm)) {
        aqm_sample_t s;
        /* short timeout: the loop also drives the 10 Hz accel / display tick */
        int rc = aqm_ring_pop(&shm->ring[AQM_RING_COMPARE_REPORT], &s, TICK_MS);

        drain_alarms(q, &disp);

        if (rc == 0) {
            /* TODO Phase 5: MQTT publish, Modbus registers */
            if (s.status & AQM_ST_PMS_FAULT)
                aqm_log("report",
                        "seq %u | PM sensor FAULT (no data) | TVOC %3u ppb | %4.1f C %2.0f%%RH | "
                        "AQI  -- | e2e %lld us",
                        s.seq, s.tvoc_ppb, s.temp_c, s.rh_pct,
                        (long long)((mono_ns() - s.t_mono_ns) / 1000));
            else
                aqm_log("report",
                        "seq %u | PM2.5 %3.0f PM10 %3.0f ug/m3 | TVOC %3u ppb | %4.1f C %2.0f%%RH | "
                        "AQI %3u %-12s | e2e %lld us",
                        s.seq, s.pm25_cal, s.pm10_cal, s.tvoc_ppb, s.temp_c, s.rh_pct,
                        s.aqi, aqm_aqi_cat_name((aqm_aqi_cat_t)s.aqi_cat),
                        (long long)((mono_ns() - s.t_mono_ns) / 1000));
            disp.last = s;
            disp.have_sample = 1;
            disp.dirty = 1;
        } else if (rc != -ETIMEDOUT && rc != -EINTR) {
            aqm_log("report", "pop failed: %s", strerror(-rc));
            break;
        }

        display_tick(&disp);
    }

    aqm_log("report", "stopping");
    display_close(&disp);
    mq_close(q);
    aqm_alarm_unlink();
    aqm_shm_detach(shm);
    return 0;
}
