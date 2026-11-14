#include "aqm/adxl345.h"
#include "aqm/alarm.h"
#include "aqm/aqi.h"
#include "aqm/button.h"
#include "aqm/led.h"
#include "aqm/log.h"
#include "aqm/notify.h"
#include "aqm/orient.h"
#include "aqm/proc.h"
#include "aqm/screen.h"
#include "aqm/shm.h"
#include "aqm/sig.h"
#include "aqm/ssd1306.h"
#include "aqm/timeutil.h"

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <linux/input.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysinfo.h>

#define I2C_DEV         "/dev/i2c-2"
#define OLED_ADDR       0x3C
#define ADXL_ADDR       0x1D
#define TICK_MS         100           /* loop period: accel + ring poll, 10 Hz   */
#define BANNER_MS       10000         /* how long an alert stays on screen       */
#define ORIENT_HOLD     5             /* samples (= 0.5 s at 10 Hz)              */
#define OLED_RETRY_MS   2000          /* retry a missing OLED this often         */
#define BUTTON_NAME     "aqm-keys"    /* gpio-keys node in the DT overlay        */
#define LED_NAME        "aqm:alarm"   /* gpio-leds node in the DT overlay        */
#define PAGE_MS         30000         /* info page falls back to main after this */

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
    int64_t      retry_ns;            /* next OLED open attempt                  */
    int          warned;              /* open failure already logged             */
    int          page;                /* AQM_PAGE_*, button cycles it            */
    int64_t      page_until_ns;       /* back to the main page after this        */
    int          btn_fd, led_fd;      /* -1 = not (yet) available                */
    int          btn_warned, led_warned;
    int64_t      io_retry_ns;
    int          led_on;              /* current LED state                       */
    unsigned     tick;                /* 10 Hz tick counter (LED blink phase)    */
} display_t;

/* open OLED (+ ADXL345). At boot /dev/i2c-2 can briefly be root-only until
 * udev applies its permissions, so a failure is retried from display_tick. */
static void display_open(display_t *dp)
{
    const char *cal_path = getenv("AQM_ORIENT") ? getenv("AQM_ORIENT") : "config/orient.conf";
    aqm_orient_cal_t cal;
    int rc;

    if ((rc = ssd1306_open(&dp->lcd, I2C_DEV, OLED_ADDR)) != 0) {
        if (!dp->warned)
            aqm_log("report", "OLED not available (%s): text-only, retrying every %d s",
                    strerror(-rc), OLED_RETRY_MS / 1000);
        dp->warned = 1;
        dp->retry_ns = mono_ns() + (int64_t)OLED_RETRY_MS * 1000000LL;
        return;
    }
    dp->have_lcd = 1;
    if (dp->warned)
        aqm_log("report", "OLED available now");

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
    dp->dirty = 1;                     /* show the last sample right away */
}

/* button and LED come from the DT overlay; like the OLED they may be
 * root-only for a moment at boot, so they are retried too */
static void io_open(display_t *dp)
{
    int rc;

    if (dp->btn_fd < 0) {
        if ((rc = aqm_button_open(BUTTON_NAME)) >= 0) {
            dp->btn_fd = rc;
            aqm_log("report", "button (%s) ready: press to change page", BUTTON_NAME);
        } else if (!dp->btn_warned) {
            aqm_log("report", "button %s not available (%s), retrying", BUTTON_NAME, strerror(-rc));
            dp->btn_warned = 1;
        }
    }
    if (dp->led_fd < 0) {
        if ((rc = aqm_led_open(LED_NAME)) >= 0) {
            dp->led_fd = rc;
            aqm_led_set(dp->led_fd, 0);
            aqm_log("report", "LED (%s) ready", LED_NAME);
        } else if (!dp->led_warned) {
            aqm_log("report", "LED %s not available (%s), retrying", LED_NAME, strerror(-rc));
            dp->led_warned = 1;
        }
    }
    dp->io_retry_ns = mono_ns() + (int64_t)OLED_RETRY_MS * 1000000LL;
}

static void display_init(display_t *dp)
{
    memset(dp, 0, sizeof(*dp));
    dp->acc.fd = -1;
    dp->btn_fd = dp->led_fd = -1;
    display_open(dp);
    io_open(dp);
}

static void display_close(display_t *dp)
{
    if (dp->have_lcd)
        ssd1306_close(&dp->lcd);
    if (dp->have_acc)
        adxl345_close(&dp->acc);
    aqm_button_close(dp->btn_fd);
    aqm_led_close(dp->led_fd);          /* leaves the LED off */
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
            dp->page = AQM_PAGE_MAIN;   /* an alert always shows on the main page */
            dp->dirty = 1;
        }
    }
}

/* first non-loopback IPv4 address (wlan0 on the BBBW) */
static void get_sysinfo(aqm_sysinfo_t *si)
{
    struct ifaddrs *ifa, *i;
    struct sysinfo sy;

    memset(si, 0, sizeof(*si));
    if (sysinfo(&sy) == 0)
        si->uptime_s = sy.uptime;
    if (getifaddrs(&ifa) != 0)
        return;
    for (i = ifa; i; i = i->ifa_next) {
        if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && strcmp(i->ifa_name, "lo") != 0) {
            inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, si->ip, sizeof(si->ip));
            break;
        }
    }
    freeifaddrs(ifa);
}

/* LED: fast blink = PM sensor fault, slow blink = AQI Poor or worse, else off */
static void led_tick(display_t *dp)
{
    int want = 0;

    if (dp->led_fd < 0 || !dp->have_sample)
        return;
    if (dp->last.status & AQM_ST_PMS_FAULT)
        want = dp->tick & 1;                        /* 5 Hz  */
    else if (dp->last.aqi_cat >= AQI_POOR)
        want = (dp->tick / 5) & 1;                  /* 1 Hz  */
    if (want != dp->led_on && aqm_led_set(dp->led_fd, want) == 0)
        dp->led_on = want;
}

/* short press: next page; info pages time out back to the main page */
static void button_tick(display_t *dp)
{
    int n;

    if (dp->btn_fd < 0)
        return;
    n = aqm_button_presses(dp->btn_fd, KEY_PROG1);
    if (n < 0) {
        aqm_log("report", "button lost (%s), retrying", strerror(-n));
        aqm_button_close(dp->btn_fd);
        dp->btn_fd = -1;
        return;
    }
    if (n > 0 && dp->have_lcd) {
        dp->page = (dp->page + n) % AQM_PAGE_COUNT;
        dp->page_until_ns = mono_ns() + (int64_t)PAGE_MS * 1000000LL;
        dp->dirty = 1;
    }
    if (dp->page != AQM_PAGE_MAIN && mono_ns() >= dp->page_until_ns) {
        dp->page = AQM_PAGE_MAIN;
        dp->dirty = 1;
    }
}

static void display_tick(display_t *dp)
{
    float gx, gy, gz;
    int rc;

    dp->tick++;
    if ((dp->btn_fd < 0 || dp->led_fd < 0) && mono_ns() >= dp->io_retry_ns)
        io_open(dp);
    button_tick(dp);
    led_tick(dp);

    if (!dp->have_lcd) {
        if (mono_ns() < dp->retry_ns)
            return;
        display_open(dp);
        if (!dp->have_lcd)
            return;
    }

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
        if (dp->page == AQM_PAGE_MAIN) {
            aqm_screen_draw(&dp->lcd, &dp->last, &dp->banner);
        } else {
            aqm_sysinfo_t si;
            get_sysinfo(&si);
            aqm_screen_draw_page(&dp->lcd, &dp->last, dp->page, &si);
        }
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
            /* a sample reaching the last stage proves all 5 stages are alive:
             * pet the systemd watchdog and publish a one-line status */
            if (!disp.have_sample)
                aqm_notify("READY=1");
            {
                char st[96];
                if (s.status & AQM_ST_PMS_FAULT)
                    snprintf(st, sizeof(st), "WATCHDOG=1\nSTATUS=seq %u | PM sensor FAULT", s.seq);
                else
                    snprintf(st, sizeof(st), "WATCHDOG=1\nSTATUS=seq %u | PM2.5 %.0f | AQI %u %s",
                             s.seq, s.pm25_cal, s.aqi, aqm_aqi_cat_name((aqm_aqi_cat_t)s.aqi_cat));
                aqm_notify(st);
            }
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
    aqm_notify("STOPPING=1");
    display_close(&disp);
    mq_close(q);
    aqm_alarm_unlink();
    aqm_shm_detach(shm);
    return 0;
}
