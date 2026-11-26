#include "aqm/adxl345.h"
#include "aqm/alarm.h"
#include "aqm/aqi.h"
#include "aqm/avg.h"
#include "aqm/button.h"
#include "aqm/calib.h"
#include "aqm/json.h"
#include "aqm/led.h"
#include "aqm/log.h"
#include "aqm/modbus.h"
#include "aqm/mqtt.h"
#include "aqm/notify.h"
#include "aqm/orient.h"
#include "aqm/proc.h"
#include "aqm/screen.h"
#include "aqm/shm.h"
#include "aqm/sig.h"
#include "aqm/store.h"
#include "aqm/ssd1306.h"
#include "aqm/timeutil.h"

#include <arpa/inet.h>
#include <signal.h>
#include <stdarg.h>
#include <errno.h>
#include <ifaddrs.h>
#include <linux/input.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysinfo.h>
#include <time.h>
#include <unistd.h>

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
#define MULTI_GAP_MS    450           /* presses closer than this belong together */
#define OFF_PRESSES     3             /* this many quick presses = screen off     */
#define FAULT_SHOW_MS   2000          /* sensor faulty this long -> fault ticker  */
#define TICKER_PX       3             /* ticker speed: px per 100 ms = 30 px/s    */
#define PMS_SILENT_MS   3000          /* no byte from the PMS5003 this long = no signal */
#define ACC_FAIL_MAX    20            /* ADXL345 reads failing in a row = fault    */
#define CRASH_FILE      "data/last_exit"   /* written by aqm_service.sh */
#define CRASH_SHOW_S    (24 * 3600)   /* a crash stays on the fault page this long */
#define MAX_FAULTS      10

/* rolling 1/5/10/20 min + 24 h averages of the samples this stage receives */
static aqm_avg_t g_avg;
/* MQTT publisher: NULL when disabled/not configured */
static aqm_mqtt_t *g_mqtt;
/* Modbus RTU slave thread (RS485): NULL when disabled */
static aqm_modbus_t *g_modbus;

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
    int          screen_off;          /* OLED blanked by a triple press          */
    int          presses;             /* presses not yet acted on                */
    int64_t      press_ns;            /* time of the last of them                */
    int          bright_pct;          /* OLED brightness 0..100 (set from the app) */
    int          contrast_sent;       /* brightness the panel has, -1 = unknown    */
    int          oled_pub;            /* OLED state changed: publish it            */
    int64_t      oled_pub_ns;         /* last time the state was published         */
    int64_t      fault_since_ns[4];   /* when each fault began, 0 = OK             */
    uint32_t     fault_mask;          /* AQM_ST_* faults shown on the ticker       */
    char         ticker[320];         /* the scrolling fault message               */
    int          ticker_x;            /* its current x position                    */
    const aqm_health_t *health;       /* sensor details from sense (shm)           */
    uint32_t     pms_bytes, pms_frames;    /* last seen counters ...               */
    int64_t      pms_bytes_ns, pms_frames_ns; /* ... and when they last changed    */
    int          acc_fail, acc_fault; /* ADXL345 reads failing in a row / fault     */
    char         crash[96];           /* last pipeline crash, "" = none            */
    char         crash_long[200];     /* the same, explained (for the phone app)   */
    /* software faults go to the phone app (MQTT <topic>/alerts, retained) */
    char         alerts[2048];        /* JSON of the current alerts                */
    char         alerts_sent[2048];   /* last published                            */
    int64_t      alerts_ns;           /* last publish                              */
    char         diag[480];           /* overlay / I2C diagnosis (cached)          */
    int64_t      diag_ns;
    uint32_t     stalls_seen;         /* ENS160 stall count last seen ...          */
    int64_t      stall_t;             /* ... and the wall time of the last one     */
    int64_t      crash_t;             /* its wall-clock time                       */
    int          nfaults;             /* fault pages (one per fault)               */
    char         ftitle[MAX_FAULTS][28];
    char         fbody[MAX_FAULTS][120];
    int          fault_k;             /* fault page shown (page 2 + fault_k)        */
    int64_t      start_ns;            /* when report started                       */
    aqm_orient_cal_t cal;             /* orientation calibration (for re-opening)  */
    int          cal_ok;
    int64_t      acc_retry_ns;
    char         fsig[MAX_FAULTS * 28];   /* titles joined: detects a change    */
} display_t;

/* ticker lines; the first three are the sample's AQM_ST_* bits, the others
 * are found by report itself */
#define F_ACC    (1u << 8)            /* ADXL345 not answering                  */
static const struct {
    uint32_t    bit;
    const char *msg;
} g_faults[4] = {
    { AQM_ST_PMS_FAULT, "PM2.5/PM10 SENSOR (PMS5003) NOT WORKING" },
    { AQM_ST_ENS_FAULT, "TVOC/eCO2 SENSOR (ENS160) NOT WORKING" },
    { AQM_ST_AHT_FAULT, "TEMP/HUMIDITY SENSOR (AHT21) NOT WORKING" },
    { F_ACC,            "ACCELEROMETER (ADXL345) NOT WORKING" },
};

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
    dp->contrast_sent = -1;               /* (re)opened panel: apply our brightness */
    if (dp->warned)
        aqm_log("report", "OLED available now");

    if ((rc = aqm_orient_cal_load(&cal, cal_path)) != 0) {
        aqm_log("report", "no orientation calibration (%s): fixed landscape. "
                "Run build/tools/accel_cal to enable auto-rotate", cal_path);
    } else if ((dp->cal = cal, dp->cal_ok = 1,
                rc = adxl345_open(&dp->acc, I2C_DEV, ADXL_ADDR)) != 0) {
        aqm_log("report", "ADXL345 not available (%s): fixed landscape", strerror(-rc));
        dp->acc_fault = 1;                 /* calibrated, so it should be there */
        dp->acc_retry_ns = mono_ns() + 5000000000LL;
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
    dp->bright_pct = 81;                  /* = the driver's default contrast 0xCF */
    dp->contrast_sent = -1;
    dp->oled_pub = 1;
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
        return 0;       /* the fault ticker names the sensor; page 2 says what to check */
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
        if (g_mqtt) {
            char js[256];
            if (aqm_json_alarm(js, sizeof(js), &a) > 0)
                aqm_mqtt_publish(g_mqtt, "alarm", js, 1, 0);
        }
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

/* ------------------------------ fault page ------------------------------ */

/* "4:05 PM" */
static void hhmm_str(long long t, char *buf, size_t n)
{
    time_t tt = (time_t)t;
    struct tm tm;

    localtime_r(&tt, &tm);
    strftime(buf, n, "%I:%M %p", &tm);
    if (buf[0] == '0')
        memmove(buf, buf + 1, strlen(buf));
}

static void add_fault(display_t *dp, const char *title, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
static void add_fault(display_t *dp, const char *title, const char *fmt, ...)
{
    va_list ap;

    if (dp->nfaults >= MAX_FAULTS)
        return;
    snprintf(dp->ftitle[dp->nfaults], sizeof(dp->ftitle[0]), "%s", title);
    va_start(ap, fmt);
    vsnprintf(dp->fbody[dp->nfaults], sizeof(dp->fbody[0]), fmt, ap);
    va_end(ap);
    dp->nfaults++;
}

static int i2c_nack(int e)  { return e == EREMOTEIO || e == ENXIO; }
static int i2c_bus(int e)   { return e == ETIMEDOUT || e == EAGAIN || e == EIO; }
static int i2c_dev_bad(int e) { return e == ENOENT || e == ENODEV || e == EACCES || e == EPERM; }

/* an errno in a few plain words (fits one 21-character line) */
static const char *i2c_words(int e)
{
    switch (e) {
    case ETIMEDOUT: return "bus timeout";
    case EAGAIN:    return "bus busy / noise";
    case EIO:       return "transfer failed";
    case EREMOTEIO:
    case ENXIO:     return "no answer";
    case EINVAL:    return "bad request";
    default:        return "unknown";
    }
}

/* Fault page texts: shorthand, one fact per line, at most 5 lines of at
 * most 21 characters (landscape); portrait wraps them to 10 characters.
 * "->" = goes to / check this wire, "X" = not working. */
#define I2C_WIRES "SDA->P9.20\nSCL->P9.19\nVIN->P9.3 (3.3V)\nGND->P9.1"
#define SW_FAULT  "Software fault\nSee phone app"

/* one I2C chip on the ENS160+AHT21 module; partner_ok = the other chip works */
static void i2c_fault(display_t *dp, const char *title, const char *chip, int e,
                      int partner_ok, const char *partner)
{
    if (i2c_nack(e) && partner_ok)
        add_fault(dp, title, "No answer\n%s works, so\nwires are fine\n"
                  "Unplug VIN (P9.3) 10s\nStill X? %s bad", partner, chip);
    else if (i2c_nack(e))
        add_fault(dp, title, "No answer\n" I2C_WIRES);
    else if (i2c_bus(e))
        add_fault(dp, title, "I2C bus errors\nSDA->P9.20 loose?\nSCL->P9.19 loose?\n"
                  "Shorted module?");
    else if (i2c_dev_bad(e))
        add_fault(dp, title, SW_FAULT);     /* diagnosed in software, details in the app */
    else if (e)
        add_fault(dp, title, "Err: %s\n" I2C_WIRES, i2c_words(e));
}

/* everything wrong right now: one page each, title = which part */
static void build_faults(display_t *dp)
{
    const aqm_health_t *h = dp->health;
    int64_t now = mono_ns();
    uint32_t m = dp->fault_mask;
    int ae = 0, ee = 0;
    char sig[sizeof(dp->fsig)];
    int i;

    dp->nfaults = 0;
    if (h) {
        uint32_t b = __atomic_load_n(&h->pms_bytes, __ATOMIC_RELAXED);
        uint32_t f = __atomic_load_n(&h->pms_frames, __ATOMIC_RELAXED);

        if (b != dp->pms_bytes) { dp->pms_bytes = b; dp->pms_bytes_ns = now; }
        if (f != dp->pms_frames) { dp->pms_frames = f; dp->pms_frames_ns = now; }
        ae = __atomic_load_n(&h->aht_err, __ATOMIC_RELAXED);
        ee = __atomic_load_n(&h->ens_err, __ATOMIC_RELAXED);
    }

    /* PMS5003: are bytes arriving at all? valid frames? the sensor's own flag? */
    if (m & AQM_ST_PMS_FAULT) {
        if (!h || now - dp->pms_bytes_ns > (int64_t)PMS_SILENT_MS * 1000000LL)
            add_fault(dp, "PMS5003 FAULT", "No bytes\nTX->P9.26\nFan X? VCC->P9.7 5V\n"
                      "GND->P9.1");
        else if (__atomic_load_n(&h->pms_sensor_err, __ATOMIC_RELAXED))
            add_fault(dp, "PMS5003 FAULT", "Sensor error\nfan/laser\nUnplug VCC (P9.7)\nfor 10s");
        else
            add_fault(dp, "PMS5003 FAULT", "Bad data\nTX->P9.26 loose?\nGND->P9.1");
    }

    /* ENS160 + AHT21 share one module and the I2C bus */
    if ((m & AQM_ST_AHT_FAULT) && (m & AQM_ST_ENS_FAULT) && i2c_nack(ae) && i2c_nack(ee)) {
        add_fault(dp, "ENS+AHT FAULT", "No answer\n" I2C_WIRES);
    } else {
        if (m & AQM_ST_AHT_FAULT) {
            if (ae)
                i2c_fault(dp, "AHT21 FAULT", "AHT21", ae, !(m & AQM_ST_ENS_FAULT), "ENS160");
            else
                add_fault(dp, "AHT21 FAULT", "Bad readings\nVIN->P9.3 (3.3V)\nGND->P9.1");
        }
        if (m & AQM_ST_ENS_FAULT) {
            if (ee)
                i2c_fault(dp, "ENS160 FAULT", "ENS160", ee, !(m & AQM_ST_AHT_FAULT), "AHT21");
            else if (h && __atomic_load_n(&h->ens_invalid, __ATOMIC_RELAXED))
                add_fault(dp, "ENS160 FAULT", "Invalid output\nWait 1 min, or\n"
                          "unplug VIN (P9.3)\nfor 10s");
            else
                add_fault(dp, "ENS160 FAULT", "Stalled\nAuto-reset\nSee phone app");
        }
    }

    if (m & F_ACC)
        add_fault(dp, "ADXL345 FAULT", "No answer\n" I2C_WIRES);

    /* overlay devices: a missing driver can be seen, a broken wire can not */
    if (now - dp->start_ns > 10LL * 1000000000LL) {
        if (dp->led_fd < 0)
            add_fault(dp, "LED FAULT", "No driver\nOverlay not loaded?\nP8.14->1k" "\x7f" "\n->LED->GND");
        if (dp->btn_fd < 0)
            add_fault(dp, "BUTTON FAULT", "No driver\nOverlay not loaded?\nP8.12->btn\n->GND");
    }

    /* a crash is restarted automatically (systemd); it is reported in the app only */

    /* the list changed: stay on a fault page only if it still exists */
    sig[0] = '\0';
    for (i = 0; i < dp->nfaults; i++)
        strncat(sig, dp->ftitle[i], sizeof(sig) - strlen(sig) - 1);
    if (strcmp(sig, dp->fsig) != 0) {
        snprintf(dp->fsig, sizeof(dp->fsig), "%s", sig);
        if (dp->page == AQM_PAGE_FAULTS && dp->fault_k >= dp->nfaults) {
            if (dp->nfaults)
                dp->fault_k = dp->nfaults - 1;
            else
                dp->page = AQM_PAGE_MAIN;         /* all fixed: fault pages disappear */
        }
        dp->dirty = 1;                            /* page numbers changed */
    }
}

/* "SEGV" etc. for the fault page */
static const char *sig_short(int sig)
{
    static char buf[24];

    switch (sig) {
    case SIGSEGV: return "SEGV";
    case SIGABRT: return "ABRT";
    case SIGBUS:  return "BUS";
    case SIGFPE:  return "FPE";
    case SIGILL:  return "ILL";
    case SIGKILL: return "KILL";
    default:
        snprintf(buf, sizeof(buf), "signal %d", sig);
        return buf;
    }
}

/* the last pipeline restart, recorded by aqm_service.sh (which stage exited,
 * how) and by systemd's ExecStopPost (watchdog = frozen). "" = none */
static void load_crash(display_t *dp)
{
    FILE *f;
    long long t1 = 0, t2 = 0;
    char stage[32] = "", result[32] = "", ts[16];
    int rc = 0;

    dp->crash[0] = '\0';
    if ((f = fopen(CRASH_FILE, "r"))) {
        if (fscanf(f, "%lld %31s %d", &t1, stage, &rc) != 3)
            t1 = 0;
        fclose(f);
    }
    if ((f = fopen("data/last_stop", "r"))) {
        if (fscanf(f, "%lld %31s", &t2, result) != 2)
            t2 = 0;
        fclose(f);
    }
    if (t2 > t1 + 5 && !strcmp(result, "watchdog")) {
        hhmm_str(t2, ts, sizeof(ts));
        snprintf(dp->crash, sizeof(dp->crash), "%s\nFroze (watchdog)", ts);
        snprintf(dp->crash_long, sizeof(dp->crash_long), "At %s the pipeline stopped "
                 "responding; the watchdog restarted it automatically. Details: "
                 "journalctl -u aqm -b", ts);
        dp->crash_t = t2;
    } else if (t1 && rc != 0 && rc != 143) {     /* 143 = SIGTERM: normal stop */
        hhmm_str(t1, ts, sizeof(ts));
        if (rc > 128) {
            snprintf(dp->crash, sizeof(dp->crash), "%s\n%s: %s", ts, stage,
                     sig_short(rc - 128));
            snprintf(dp->crash_long, sizeof(dp->crash_long), "At %s the '%s' stage "
                     "crashed (%s); the pipeline was restarted automatically. Details: "
                     "journalctl -u aqm -b", ts, stage,
                     rc == 139 ? "segmentation fault" : strsignal(rc - 128));
        } else {
            snprintf(dp->crash, sizeof(dp->crash), "%s\n%s: exit %d", ts, stage, rc);
            snprintf(dp->crash_long, sizeof(dp->crash_long), "At %s the '%s' stage stopped "
                     "with exit code %d; the pipeline was restarted automatically. "
                     "Details: journalctl -u aqm -b", ts, stage, rc);
        }
        dp->crash_t = t1;
    }
    if (dp->crash[0]) {
        char one[sizeof(dp->crash)], *c;

        snprintf(one, sizeof(one), "%s", dp->crash);
        for (c = one; *c; c++)
            if (*c == '\n')
                *c = ' ';
        aqm_log("report", "last restart: %s", one);
    }
}

/* ------------------------- software faults -> app ------------------------- */

/* is `key=value` set (not commented out) in uEnv.txt text? */
static int uenv_has(const char *txt, const char *key, const char *val)
{
    const char *l = txt;

    while (l && *l) {
        const char *e = strchr(l, '\n');
        size_t n = e ? (size_t)(e - l) : strlen(l);
        char line[256];

        snprintf(line, sizeof(line), "%.*s", (int)n, l);
        {
            char *p = line;

            while (*p == ' ' || *p == '\t')
                p++;
            if (*p != '#' && !strncmp(p, key, strlen(key)) && strstr(p, val))
                return 1;
        }
        l = e ? e + 1 : NULL;
    }
    return 0;
}

/* Why are /dev/i2c-2 or the overlay devices missing? Checks what can be
 * checked in software and says exactly what to change. */
static void diagnose_overlay(char *out, size_t n)
{
    char txt[8192], uname_r[96] = "", path[160];
    size_t len;
    FILE *f;
    const char *u;

    if (!(f = fopen("/boot/uEnv.txt", "r"))) {
        snprintf(out, n, "Cannot read /boot/uEnv.txt (%s).", strerror(errno));
        return;
    }
    len = fread(txt, 1, sizeof(txt) - 1, f);
    txt[len] = '\0';
    fclose(f);
    if ((u = strstr(txt, "uname_r=")))
        sscanf(u + 8, "%95s", uname_r);

    if (!uenv_has(txt, "enable_uboot_overlays", "=1")) {
        snprintf(out, n, "/boot/uEnv.txt: the line 'enable_uboot_overlays=1' is missing "
                 "or commented out (#). Fix: sudo nano /boot/uEnv.txt, add it, save, reboot.");
        return;
    }
    if (!uenv_has(txt, "uboot_overlay_addr", "BB-AQM-00A0.dtbo")) {
        snprintf(out, n, "/boot/uEnv.txt: the line 'uboot_overlay_addr4=BB-AQM-00A0.dtbo' "
                 "is missing or commented out (#). Fix: sudo nano /boot/uEnv.txt, add it, "
                 "save, reboot.");
        return;
    }
    snprintf(path, sizeof(path), "/boot/dtbs/%s/overlays/BB-AQM-00A0.dtbo", uname_r);
    if (uname_r[0] && access(path, R_OK) != 0) {
        snprintf(out, n, "%s is missing. Fix: dtc -@ -I dts -O dtb -o /tmp/BB-AQM-00A0.dtbo "
                 "dts/BB-AQM-00A0.dts, sudo cp it there, reboot.", path);
        return;
    }
    if (access("/proc/device-tree/chosen/overlays/BB-AQM-00A0.kernel", F_OK) != 0) {
        snprintf(out, n, "uEnv.txt and the overlay file are correct, but U-Boot did not load "
                 "the overlay at boot. Reboot with the serial console (tio) open and look "
                 "for an overlay error from U-Boot.");
        return;
    }
    if (access("/dev/i2c-2", F_OK) != 0) {
        snprintf(out, n, "The overlay is loaded but /dev/i2c-2 is missing: the i2c-dev "
                 "driver is not loaded. Fix: sudo modprobe i2c-dev (and check "
                 "/etc/modules-load.d).");
        return;
    }
    if (access("/dev/i2c-2", R_OK | W_OK) != 0) {
        snprintf(out, n, "/dev/i2c-2 exists but the monitor may not open it (permission). "
                 "Fix: sudo systemctl restart aqm (aqm_devperm.sh sets the permissions).");
        return;
    }
    snprintf(out, n, "uEnv.txt, the overlay and /dev/i2c-2 all look correct now; the "
             "error should clear within a few seconds.");
}

/* JSON string body (quotes, backslashes, control characters escaped) */
static void json_str(char *out, size_t n, const char *in)
{
    size_t o = 0;

    for (; *in && o + 7 < n; in++) {
        unsigned char c = (unsigned char)*in;

        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c < 0x20 || c == 0x7f) {
            o += (size_t)snprintf(out + o, n - o, "\\u%04x", c == 0x7f ? 0x3a9 : c);
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
}

static void alert_add(char *js, size_t n, int *count, const char *title, const char *text)
{
    char t[96], x[512];
    size_t len = strlen(js);

    json_str(t, sizeof(t), title);
    json_str(x, sizeof(x), text);
    snprintf(js + len, n - len, "%s{\"title\":\"%s\",\"text\":\"%s\"}",
             *count ? "," : "", t, x);
    (*count)++;
}

/* Software faults (things to fix in software, or that fix themselves) are
 * published, retained, on <topic>/alerts for the phone app:
 *   {"ts":..., "items":[{"title":"...","text":"..."}, ...]}   (items empty = none)
 * The OLED only says "See phone app" for them. */
static void alerts_tick(display_t *dp)
{
    const aqm_health_t *h = dp->health;
    int64_t now = mono_ns(), wall = (int64_t)time(NULL);
    int ae = h ? __atomic_load_n(&h->aht_err, __ATOMIC_RELAXED) : 0;
    int ee = h ? __atomic_load_n(&h->ens_err, __ATOMIC_RELAXED) : 0;
    uint32_t st = h ? __atomic_load_n(&h->ens_stalls, __ATOMIC_RELAXED) : 0;
    int overlay = ((dp->fault_mask & AQM_ST_AHT_FAULT) && i2c_dev_bad(ae)) ||
                  ((dp->fault_mask & AQM_ST_ENS_FAULT) && i2c_dev_bad(ee)) ||
                  (now - dp->start_ns > 10LL * 1000000000LL &&
                   (dp->led_fd < 0 || dp->btn_fd < 0));
    char js[sizeof(dp->alerts)], txt[400], ts[16];
    int count = 0;

    js[0] = '\0';
    if (overlay) {
        if (!dp->diag[0] || now - dp->diag_ns > 30LL * 1000000000LL) {
            diagnose_overlay(dp->diag, sizeof(dp->diag));
            dp->diag_ns = now;
            aqm_log("report", "overlay / I2C diagnosis: %s", dp->diag);
        }
        alert_add(js, sizeof(js), &count, "Overlay / I2C bus not available", dp->diag);
    } else {
        dp->diag[0] = '\0';
    }

    if (st != dp->stalls_seen) {
        dp->stalls_seen = st;
        dp->stall_t = wall;
    }
    if (st) {
        hhmm_str(dp->stall_t, ts, sizeof(ts));
        snprintf(txt, sizeof(txt), "The ENS160 stopped delivering data %u time%s since the "
                 "monitor started (last at %s) and was reset automatically each time. "
                 "If this keeps happening: solder a 10 uF capacitor across the module's "
                 "VIN and GND.", st, st == 1 ? "" : "s", ts);
        alert_add(js, sizeof(js), &count, "ENS160 stalled", txt);
    }

    if (dp->crash_long[0] && wall - dp->crash_t < CRASH_SHOW_S)
        alert_add(js, sizeof(js), &count, "Pipeline restarted", dp->crash_long);

    snprintf(dp->alerts, sizeof(dp->alerts), "{\"ts\":%lld,\"items\":[%s]}",
             (long long)wall, js);
    /* publish on change (ignoring the timestamp) and every 60 s */
    if (aqm_mqtt_connected(g_mqtt) &&
        (strcmp(js, dp->alerts_sent) != 0 || now - dp->alerts_ns > 60LL * 1000000000LL)) {
        if (aqm_mqtt_publish(g_mqtt, "alerts", dp->alerts, 1, 1) == 0) {
            if (strcmp(js, dp->alerts_sent) != 0)
                aqm_log("report", "app alerts: %d", count);
            snprintf(dp->alerts_sent, sizeof(dp->alerts_sent), "%s", js);
            dp->alerts_ns = now;
        }
    }
}

/* Sensor faults: a sensor that has been faulty for FAULT_SHOW_MS gets a
 * message on a ticker that scrolls across the top of the main page (right to
 * left, repeating) until the sensor works again. Short glitches don't show. */
static void fault_tick(display_t *dp)
{
    int64_t now = mono_ns();
    uint32_t mask = 0;
    int i, w = dp->have_lcd ? ssd1306_width(&dp->lcd) : 128;
    uint32_t cur = dp->have_sample ? dp->last.status : 0;

    if (dp->acc_fault)
        cur |= F_ACC;
    for (i = 0; i < 4; i++) {
        if (cur & g_faults[i].bit) {
            if (!dp->fault_since_ns[i])
                dp->fault_since_ns[i] = now;
            if (now - dp->fault_since_ns[i] >= (int64_t)FAULT_SHOW_MS * 1000000LL)
                mask |= g_faults[i].bit;
        } else {
            dp->fault_since_ns[i] = 0;
        }
    }
    if (mask != dp->fault_mask) {
        dp->ticker[0] = '\0';
        for (i = 0; i < 4; i++) {
            uint32_t b = g_faults[i].bit;

            if ((mask & b) && !(dp->fault_mask & b))
                aqm_log("report", "on screen: %s", g_faults[i].msg);
            else if (!(mask & b) && (dp->fault_mask & b))
                aqm_log("report", "cleared: %s (working again)", g_faults[i].msg);
            if (mask & b) {
                size_t n = strlen(dp->ticker);

                snprintf(dp->ticker + n, sizeof(dp->ticker) - n, "%s%s",
                         n ? "   ***   " : "", g_faults[i].msg);
            }
        }
        if (mask) {
            size_t n = strlen(dp->ticker);

            snprintf(dp->ticker + n, sizeof(dp->ticker) - n, "   >>> PRESS BUTTON: PAGE 2");
        }
        dp->fault_mask = mask;
        dp->ticker_x = w;                       /* enter from the right edge */
        dp->banner.ticker = mask ? dp->ticker : NULL;
        dp->banner.ticker_x = dp->ticker_x;
        dp->dirty = 1;
    }
    if (mask && dp->page == AQM_PAGE_MAIN && !dp->screen_off) {
        dp->ticker_x -= TICKER_PX;
        if (dp->ticker_x < -aqm_screen_ticker_width(dp->ticker))
            dp->ticker_x = w;                   /* passed: start again */
        dp->banner.ticker_x = dp->ticker_x;
        dp->dirty = 1;
    }
}

/* LED: double blink every 2 s = the OLED itself is not working (nothing else
 * can tell you), fast blink = something on the fault page (a sensor not
 * working), slow blink = AQI Poor or worse, else off */
static void led_tick(display_t *dp)
{
    int want = 0;

    if (dp->led_fd < 0 || !dp->have_sample)
        return;
    if (!dp->have_lcd && mono_ns() - dp->start_ns > 10LL * 1000000000LL)
        want = (dp->tick % 20) == 0 || (dp->tick % 20) == 2;   /* double blink: no screen */
    else if (dp->fault_mask)
        want = dp->tick & 1;                        /* 5 Hz  */
    else if (dp->last.aqi_cat >= AQI_POOR)
        want = (dp->tick / 5) & 1;                  /* 1 Hz  */
    if (want != dp->led_on && aqm_led_set(dp->led_fd, want) == 0)
        dp->led_on = want;
}

/* short press: next page; info pages time out back to the main page */
/* panel off (0xAE, ~10 uA) / on (0xAF); everything else keeps running */
static void screen_power(display_t *dp, int on, const char *by)
{
    uint8_t cmd = on ? 0xAF : 0xAE;

    dp->screen_off = !on;
    if (dp->have_lcd)
        ssd1306_cmd(&dp->lcd, &cmd, 1);
    if (on) {
        dp->page = AQM_PAGE_MAIN;
        dp->lcd.shadow_valid = 0;          /* full redraw */
        dp->dirty = 1;
    }
    dp->oled_pub = 1;
    aqm_log("report", "OLED %s by %s", on ? "on" : "off", by);
}

/* OLED commands from the phone app (MQTT), brightness, and the retained state
 * topic <topic>/oled = {"on":true,"brightness":81} that the app displays */
static void oled_remote_tick(display_t *dp)
{
    int pw, br;

    if (aqm_mqtt_take_oled(g_mqtt, &pw, &br)) {
        if (br >= 0 && br != dp->bright_pct) {
            dp->bright_pct = br;
            dp->oled_pub = 1;
            aqm_log("report", "OLED brightness %d %% (app)", br);
        }
        if (pw >= 0 && pw == dp->screen_off)          /* state differs */
            screen_power(dp, pw, "app");
    }
    if (dp->have_lcd && dp->contrast_sent != dp->bright_pct &&
        ssd1306_contrast(&dp->lcd, (uint8_t)((dp->bright_pct * 255 + 50) / 100)) == 0)
        dp->contrast_sent = dp->bright_pct;
    if ((dp->oled_pub || mono_ns() - dp->oled_pub_ns > 60LL * 1000000000LL) &&
        aqm_mqtt_connected(g_mqtt)) {
        char js[64];

        snprintf(js, sizeof(js), "{\"on\":%s,\"brightness\":%d}",
                 dp->screen_off ? "false" : "true", dp->bright_pct);
        if (aqm_mqtt_publish(g_mqtt, "oled", js, 1, 1) == 0) {
            dp->oled_pub = 0;
            dp->oled_pub_ns = mono_ns();
        }
    }
}

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
    /* single press = next page, but only once no further press follows
     * within MULTI_GAP_MS, so a triple press does not flip pages first.
     * Triple press = OLED off; any press while off = OLED back on. */
    if (n > 0) {
        dp->presses += n;
        dp->press_ns = mono_ns();
    }
    if (dp->presses >= OFF_PRESSES && !dp->screen_off) {
        dp->presses = 0;
        screen_power(dp, 0, "button");
    } else if (dp->presses > 0 &&
               mono_ns() - dp->press_ns >= (int64_t)MULTI_GAP_MS * 1000000LL) {
        if (dp->screen_off) {
            screen_power(dp, 1, "button");
        } else if (dp->have_lcd) {
            int i;

            /* main -> one page per fault (only while there are faults) -> info pages */
            for (i = 0; i < dp->presses; i++) {
                if (dp->page == AQM_PAGE_MAIN && dp->nfaults) {
                    dp->page = AQM_PAGE_FAULTS;
                    dp->fault_k = 0;
                } else if (dp->page == AQM_PAGE_FAULTS && dp->fault_k + 1 < dp->nfaults) {
                    dp->fault_k++;
                } else if (dp->page == AQM_PAGE_FAULTS || dp->page == AQM_PAGE_MAIN) {
                    dp->page = AQM_PAGE_PARTICLES;
                } else {
                    dp->page = (dp->page + 1) % AQM_PAGE_COUNT;
                }
            }
            dp->page_until_ns = mono_ns() + (int64_t)PAGE_MS * 1000000LL;
            dp->dirty = 1;
        }
        dp->presses = 0;
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
    fault_tick(dp);
    build_faults(dp);
    alerts_tick(dp);
    led_tick(dp);

    if (!dp->have_lcd) {
        if (mono_ns() < dp->retry_ns)
            return;
        display_open(dp);
        if (!dp->have_lcd)
            return;
        if (dp->screen_off) {              /* re-opened panel stays dark */
            uint8_t off = 0xAE;
            ssd1306_cmd(&dp->lcd, &off, 1);
        }
    }

    /* ADXL345 unplugged: reads fail -> fault (auto-rotate pauses); retry */
    if (dp->have_acc) {
        if (adxl345_read_g(&dp->acc, &gx, &gy, &gz) != 0) {
            if (++dp->acc_fail == ACC_FAIL_MAX) {
                aqm_log("report", "ADXL345 not answering: auto-rotate paused");
                dp->acc_fault = 1;
            }
        } else {
            if (dp->acc_fault)
                aqm_log("report", "ADXL345 answering again");
            dp->acc_fail = dp->acc_fault = 0;
            if (aqm_orient_update(&dp->orient, gx, gy, gz)) {
                ssd1306_set_rotation(&dp->lcd, aqm_orient_rotation(&dp->orient));
                aqm_log("report", "display -> %s", aqm_orient_name(dp->orient.state));
                dp->dirty = 1;
            }
        }
    } else if (dp->acc_fault && dp->cal_ok && mono_ns() >= dp->acc_retry_ns) {
        if (adxl345_open(&dp->acc, I2C_DEV, ADXL_ADDR) == 0) {
            aqm_orient_init(&dp->orient, &dp->cal, ORIENT_HOLD);
            dp->have_acc = 1;
            dp->acc_fault = dp->acc_fail = 0;
            aqm_log("report", "ADXL345 found: auto-rotate enabled");
        }
        dp->acc_retry_ns = mono_ns() + 5000000000LL;
    }

    if (dp->banner.kind != AQM_BANNER_NONE && mono_ns() >= dp->banner_until_ns) {
        dp->banner.kind = AQM_BANNER_NONE;
        dp->dirty = 1;
    }

    if (dp->dirty && dp->have_sample && !dp->screen_off) {
        if (dp->page == AQM_PAGE_MAIN) {
            aqm_screen_draw(&dp->lcd, &dp->last, &dp->banner);
        } else if (dp->page == AQM_PAGE_FAULTS) {
            aqm_screen_draw_fault(&dp->lcd, 2 + dp->fault_k, 5 + dp->nfaults,
                                  dp->ftitle[dp->fault_k], dp->fbody[dp->fault_k]);
        } else {
            aqm_sysinfo_t si;
            get_sysinfo(&si);
            aqm_screen_draw_page(&dp->lcd, &dp->last, dp->page, &si, &g_avg, dp->nfaults);
        }
        if ((rc = ssd1306_flush(&dp->lcd)) != 0)
            aqm_log("report", "OLED write failed: %s", strerror(-rc));
        dp->dirty = 0;
    }
}

/* replay one stored row: same calibration as `convert`, then into the averages */
static int seed_cb(const aqm_sample_t *row, void *ctx)
{
    aqm_sample_t s = *row;

    aqm_calib_apply((const aqm_calib_t *)ctx, &s);
    aqm_avg_seed_row(&g_avg, &s);
    return aqm_should_stop();
}

/* after a restart, rebuild the averages from the last 24 h in the offline
 * buffer instead of waiting 16 h for a valid 24 h AQI */
static void seed_averages(void)
{
    const char *db = getenv("AQM_DB") ? getenv("AQM_DB") : "data/aqm.db";
    const char *cp = getenv("AQM_CALIB") ? getenv("AQM_CALIB") : "config/calib.conf";
    int64_t now = (int64_t)time(NULL), rows, t0 = mono_ns();
    aqm_store_t *st;
    aqm_calib_t cal;
    int mins;

    aqm_calib_defaults(&cal);
    if (aqm_calib_load(&cal, cp) != 0)
        aqm_log("report", "seed: %s not readable, using default calibration", cp);

    if (access(db, R_OK) != 0 || !(st = aqm_store_open(db))) {
        aqm_log("report", "seed: no offline buffer (%s), averages start empty", db);
        aqm_avg_init(&g_avg);
        return;
    }
    aqm_avg_seed_begin(&g_avg, now);
    rows = aqm_store_each_since(st, now - 86400, seed_cb, &cal);
    mins = aqm_avg_seed_end(&g_avg);
    aqm_store_close(st);
    aqm_log("report", "seed: %lld rows from %s -> %d min of 24 h history (%lld ms)%s",
            (long long)rows, db, mins, (long long)((mono_ns() - t0) / 1000000),
            mins >= AQM_AVG_DAY_MIN ? ", 24 h AQI ready" : "");
}

/* once a minute: PM2.5 / PM10 means per window and the 24 h AQI status */
static void log_averages(void)
{
    char line[256];
    int w, n = 0, mins, aqi24 = aqm_avg_aqi_24h(&g_avg, &mins);

    for (w = 0; w < AQM_W_COUNT; w++) {
        float a, b;
        int ok25 = aqm_avg_get(&g_avg, w, AQM_M_PM25, &a) > 0;
        int ok10 = aqm_avg_get(&g_avg, w, AQM_M_PM10, &b) > 0;

        if (ok25 && ok10)
            n += snprintf(line + n, sizeof(line) - (size_t)n, " %s %.0f/%.0f |",
                          aqm_avg_window_name(w), a, b);
        else
            n += snprintf(line + n, sizeof(line) - (size_t)n, " %s --/-- |",
                          aqm_avg_window_name(w));
    }
    if (g_mqtt) {
        char js[768];
        if (aqm_json_avg(js, sizeof(js), &g_avg, (long long)time(NULL)) > 0)
            aqm_mqtt_publish(g_mqtt, "avg", js, 1, 1);   /* retained: latest averages */
    }
    if (aqi24 >= 0)
        aqm_log("report", "avg PM2.5/PM10:%s AQI(24h) %d %s", line, aqi24,
                aqm_aqi_cat_name(aqm_aqi_category(aqi24)));
    else
        aqm_log("report", "avg PM2.5/PM10:%s AQI(24h) needs 16 h, have %d min", line, mins);
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

    /* history is loaded after the first live sample is on the OLED (below) */
    g_mqtt = aqm_mqtt_start(getenv("AQM_MQTT") ? getenv("AQM_MQTT") : "config/mqtt.conf");
    g_modbus = aqm_modbus_start(getenv("AQM_MODBUS") ? getenv("AQM_MODBUS") : "config/modbus.conf");
    display_init(&disp);
    disp.health = &shm->health;
    disp.start_ns = mono_ns();
    load_crash(&disp);

    while (!aqm_should_stop() && aqm_running(shm)) {
        aqm_sample_t s;
        /* short timeout: the loop also drives the 10 Hz accel / display tick */
        int rc = aqm_ring_pop(&shm->ring[AQM_RING_COMPARE_REPORT], &s, TICK_MS);

        drain_alarms(q, &disp);
        oled_remote_tick(&disp);

        if (rc == 0) {
            /* TODO Phase 5: MQTT publish, Modbus registers */
            {
                /* faulty sensors print "--", never their zeroed values */
                char pm[48], voc[16], cli[24], aq[24];

                if (s.status & AQM_ST_PMS_FAULT) {
                    snprintf(pm, sizeof(pm), "PM sensor FAULT (no data)");
                    snprintf(aq, sizeof(aq), "AQI  --");
                } else {
                    snprintf(pm, sizeof(pm), "PM2.5 %3.0f PM10 %3.0f ug/m3", s.pm25_cal, s.pm10_cal);
                    snprintf(aq, sizeof(aq), "AQI %3u %-12s", s.aqi,
                             aqm_aqi_cat_name((aqm_aqi_cat_t)s.aqi_cat));
                }
                if (s.status & AQM_ST_ENS_FAULT) snprintf(voc, sizeof(voc), "TVOC  -- ppb");
                else snprintf(voc, sizeof(voc), "TVOC %3u ppb", s.tvoc_ppb);
                if (s.status & AQM_ST_AHT_FAULT) snprintf(cli, sizeof(cli), "  -- C --%%RH");
                else snprintf(cli, sizeof(cli), "%4.1f C %2.0f%%RH", s.temp_c, s.rh_pct);
                aqm_log("report", "seq %u | %s | %s | %s | %s | e2e %lld us", s.seq, pm, voc,
                        cli, aq, (long long)((mono_ns() - s.t_mono_ns) / 1000));
            }
            /* a sample reaching the last stage proves all 5 stages are alive:
             * pet the systemd watchdog and publish a one-line status */
            if (!disp.have_sample) {
                /* boot: put live data on the OLED first, then load the 24 h
                 * history (slow on a cold SD card). Before READY=1, so the
                 * systemd watchdog is not running yet. */
                disp.last = s;
                disp.have_sample = 1;
                disp.dirty = 1;
                display_tick(&disp);
                seed_averages();
                aqm_notify("READY=1");
            }
            {
                char st[96];
                if (s.status & AQM_ST_PMS_FAULT)
                    snprintf(st, sizeof(st), "WATCHDOG=1\nSTATUS=seq %u | PM sensor FAULT", s.seq);
                else
                    snprintf(st, sizeof(st), "WATCHDOG=1\nSTATUS=seq %u | PM2.5 %.0f | AQI %u %s",
                             s.seq, s.pm25_cal, s.aqi, aqm_aqi_cat_name((aqm_aqi_cat_t)s.aqi_cat));
                aqm_notify(st);
            }
            if (g_mqtt) {
                char js[384];
                if (aqm_json_sample(js, sizeof(js), &s) > 0)
                    aqm_mqtt_publish(g_mqtt, "sample", js, 0, 0);
            }
            if (aqm_avg_push(&g_avg, &s))
                log_averages();
            aqm_modbus_update(g_modbus, &s, &g_avg);   /* registers for RS485 masters */
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
    /* blank the OLED and switch the LED off FIRST: if anything below hangs and
     * systemd has to kill us, the panel must not keep showing stale data */
    display_close(&disp);
    aqm_mqtt_stop(g_mqtt);
    aqm_modbus_stop(g_modbus);
    mq_close(q);
    aqm_alarm_unlink();
    aqm_shm_detach(shm);
    return 0;
}
