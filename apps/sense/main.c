#include "aqm/aht21.h"
#include "aqm/ens160.h"
#include "aqm/log.h"
#include "aqm/pms5003.h"
#include "aqm/shm.h"
#include "aqm/sig.h"
#include "aqm/timeutil.h"

#include <errno.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* ---------- simulated sensors (replaced by real drivers in Phase 6) ---------- */

static uint32_t rng_state = 12345;

static uint32_t rng(void)
{
    rng_state = rng_state * 1103515245u + 12345u;
    return rng_state >> 16;
}

static int noise(int span)
{
    return (int)(rng() % (uint32_t)(2 * span + 1)) - span;
}

static void sim_read(aqm_sample_t *s)
{
    static double pm25 = 45.0, tvoc = 150.0;
    static double temp_c = 28.0, rh = 55.0;

    /* floating-point mean reversion (integer (60 - pm25) / 20 was 0 for
     * |error| < 20, so the value wandered instead of returning).
     * Baseline 45 ug/m3 = AQI 75 (Satisfactory), clear of the 60/100 border */
    pm25 += noise(3) + (45.0 - pm25) * 0.10;      /* drift back to ~45   */
    if (rng() % 120 == 0)
        pm25 += 150.0;                            /* pollution spike     */
    if (pm25 < 5.0)
        pm25 = 5.0;

    tvoc += noise(10) + (150.0 - tvoc) * 0.10;
    if (tvoc < 0.0)
        tvoc = 0.0;

    temp_c += noise(1) * 0.05;
    rh     += noise(1) * 0.2;

    s->pm25     = (uint16_t)(pm25 + 0.5);
    s->pm1      = (uint16_t)(pm25 * 0.7 + 0.5);
    s->pm10     = (uint16_t)(pm25 * 1.6 + 0.5);
    s->tvoc_ppb = (uint16_t)(tvoc + 0.5);
    s->eco2_ppm = (uint16_t)(400.0 + tvoc + 0.5);

    /* AHT21 raw format: 20-bit values */
    s->aht_t_raw  = (uint32_t)((temp_c + 50.0) / 200.0 * (1 << 20));
    s->aht_rh_raw = (uint32_t)(rh / 100.0 * (1 << 20));

    if (rng() % 300 == 0)
        s->status |= AQM_ST_PMS_FAULT;            /* fault injection     */
}

/* ---------------------------- real-time setup ----------------------------- */

static void setup_rt(int prio)
{
    struct sched_param sp = { .sched_priority = prio };

    /* lock all pages in RAM: no page faults inside the timed loop */
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
        aqm_log("sense", "mlockall failed: %s", strerror(errno));

    if (prio > 0 && sched_setscheduler(0, SCHED_FIFO, &sp) != 0)
        aqm_log("sense", "SCHED_FIFO %d failed: %s (using SCHED_OTHER)",
                prio, strerror(errno));
}

/* ------------------------------ real PMS5003 ------------------------------ */

#define PMS_TIMEOUT_NS  (3LL * 1000000000LL)   /* no frame for 3 s = fault */

static pms5003_t pms;
static int       have_pms;
/* -R (the real board): a sensor that is missing is a FAULT from the start,
 * never replaced by simulated values. Without -R (development on a PC or a
 * board without sensors) missing sensors are simulated. */
static int       strict, pms_required;
static int64_t   pms_last_ns;

static void pms_init(const char *dev)
{
    int rc;

    if (!strcmp(dev, "none")) {
        aqm_log("sense", "PMS5003 disabled: simulated PM values");
        return;
    }
    if ((rc = pms5003_open(&pms, dev)) != 0) {
        pms_required = strict;
        aqm_log("sense", "PMS5003 on %s not available (%s): %s", dev, strerror(-rc),
                strict ? "FAULT" : "simulated PM values");
        return;
    }
    have_pms = 1;
    pms_last_ns = mono_ns();             /* grace period after start-up */
    aqm_log("sense", "PMS5003 on %s: real PM values", dev);
}

/* non-blocking: takes whatever frames arrived since the last tick */
static void pms_apply(aqm_sample_t *s, int64_t now)
{
    int rc;

    if (!have_pms) {
        if (pms_required) {
            s->pm1 = s->pm25 = s->pm10 = 0;
            s->status |= AQM_ST_PMS_FAULT;
        }
        return;
    }
    s->status &= ~AQM_ST_PMS_FAULT;      /* ignore the simulator's injected fault */

    rc = pms5003_poll(&pms);
    if (rc > 0)
        pms_last_ns = now;
    else if (rc < 0)
        aqm_log("sense", "PMS5003 read error: %s", strerror(-rc));

    if (pms.have_frame && now - pms_last_ns < PMS_TIMEOUT_NS && !pms.last.error) {
        s->pm1  = pms.last.pm1;
        s->pm25 = pms.last.pm25;
        s->pm10 = pms.last.pm10;
    } else {
        s->pm1 = s->pm25 = s->pm10 = 0;
        s->status |= AQM_ST_PMS_FAULT;   /* stale or flagged: let compare raise it */
    }
}

/* -------------------------- real ENS160 + AHT21 --------------------------- */
/*
 * Both sit on the I2C bus (default /dev/i2c-2). Policy, per sensor:
 *   never delivered data  -> simulated values (development without the module)
 *   detected and working  -> real values
 *   detected, then fails  -> AQM_ST_ENS_FAULT / AQM_ST_AHT_FAULT, values 0;
 *                            never silently back to simulated data
 * Missing sensors are probed again every 5 s (hot-plug, and the boot-time
 * window where udev has not yet opened /dev/i2c-2 to the gpio group).
 */
#define I2C_RETRY_NS    (5LL * 1000000000LL)
#define I2C_SETTLE_NS   (1500LL * 1000000LL)   /* first data after open      */
#define I2C_FAIL_MAX    3                      /* consecutive misses = fault */
#define I2C_REOPEN      10                     /* bus errors before re-probe */
#define ENS_STALE_NS    (3LL * 1000000000LL)
/* the ENS160 can stop measuring while still answering on the bus (seen in the
 * field: STATUS 0x01, OPMODE still 0x02, no new data). No bus errors follow,
 * so the "gone" path never fires: restart it after this long without data. */
#define ENS_STALL_NS    (10LL * 1000000000LL)

static const char *i2c_dev = "/dev/i2c-2";
static aht21_t     aht = { .fd = -1 };
static ens160_t    ens = { .fd = -1 };
static int         have_aht, have_ens, seen_aht, seen_ens;
static int         aht_fail, ens_fail, aht_last_rc = 1, ens_last_rc = 1;
static int64_t     i2c_retry_ns, aht_ready_ns, ens_ready_ns, ens_data_ns;
static uint32_t    aht_t_last, aht_rh_last;
static int         aht_have_val, ens_have_val, ens_validity = -1, env_tick;
static ens160_data_t ens_last;

static void i2c_try_open(int64_t now)
{
    int rc;

    if (now < i2c_retry_ns)
        return;
    i2c_retry_ns = now + I2C_RETRY_NS;

    if (!have_aht) {
        rc = aht21_open(&aht, i2c_dev);
        if (rc == 0) {
            have_aht = 1;
            aht_fail = 0;
            aht_have_val = 0;
            aht_ready_ns = now + I2C_SETTLE_NS;
            aht_last_rc = 0;
            aqm_log("sense", "AHT21 found at 0x%02x on %s: real temperature / humidity",
                    AHT21_ADDR, i2c_dev);
        } else if (rc != aht_last_rc) {
            aht_last_rc = rc;
            aqm_log("sense", "AHT21 not available on %s (%s): %s", i2c_dev, strerror(-rc),
                    seen_aht ? "FAULT" : "simulated temperature / humidity");
        }
    }
    if (!have_ens) {
        rc = ens160_open(&ens, i2c_dev, 0);
        if (rc == 0) {
            have_ens = 1;
            ens_fail = 0;
            ens_have_val = 0;
            ens_validity = -1;
            env_tick = 9;                           /* compensate on first data */
            ens_ready_ns = now + I2C_SETTLE_NS;
            ens_last_rc = 0;
            aqm_log("sense", "ENS160 found at 0x%02x on %s: real TVOC / eCO2",
                    ens.addr, i2c_dev);
        } else if (rc != ens_last_rc) {
            ens_last_rc = rc;
            aqm_log("sense", "ENS160 not available on %s (%s): %s", i2c_dev, strerror(-rc),
                    seen_ens ? "FAULT" : "simulated TVOC / eCO2");
        }
    }
}

static void aht_apply(aqm_sample_t *s, int64_t now)
{
    uint32_t t, rh;
    int rc;

    if (!have_aht) {
        if (seen_aht) {                             /* lost after being found */
            s->aht_t_raw = s->aht_rh_raw = 0;
            s->status |= AQM_ST_AHT_FAULT;
        }
        return;
    }
    if (now < aht_ready_ns) {                       /* first measurement running */
        if (seen_aht) {                             /* re-detected: still no data */
            s->aht_t_raw = s->aht_rh_raw = 0;
            s->status |= AQM_ST_AHT_FAULT;
        }
        return;
    }

    /* result of the measurement started 1 s ago, then start the next one:
     * the 80 ms conversion never blocks this real-time loop */
    rc = aht21_fetch(&aht, &t, &rh);
    if (rc == 0) {
        aht_t_last = t;
        aht_rh_last = rh;
        aht_have_val = 1;
        aht_fail = 0;
        seen_aht = 1;
    } else {
        aht_fail++;
    }
    aht21_trigger(&aht);

    if (aht_have_val && aht_fail < I2C_FAIL_MAX) {
        s->aht_t_raw = aht_t_last;                  /* a single miss holds the last value */
        s->aht_rh_raw = aht_rh_last;
    } else {
        s->aht_t_raw = s->aht_rh_raw = 0;
        s->status |= AQM_ST_AHT_FAULT;
    }
    if (rc != 0 && rc != -EBUSY && rc != -EBADMSG && aht_fail >= I2C_REOPEN) {
        aqm_log("sense", "AHT21 gone (%s): re-probing", strerror(-rc));
        aht21_close(&aht);
        have_aht = 0;
        aht_last_rc = 1;
    }
}

static void ens_apply(aqm_sample_t *s, int64_t now)
{
    ens160_data_t d;
    int rc;

    if (!have_ens) {
        if (seen_ens) {
            s->tvoc_ppb = s->eco2_ppm = 0;
            s->status |= AQM_ST_ENS_FAULT;
        }
        return;
    }
    if (now < ens_ready_ns) {
        if (seen_ens) {
            s->tvoc_ppb = s->eco2_ppm = 0;
            s->status |= AQM_ST_ENS_FAULT;
        }
        return;
    }

    /* humidity / temperature compensation from the AHT21, every 10 s */
    if (have_aht && aht_have_val && !(s->status & AQM_ST_AHT_FAULT) && ++env_tick >= 10) {
        env_tick = 0;
        ens160_set_env(&ens, aht21_temp_c(aht_t_last), aht21_rh(aht_rh_last));
    }

    memset(&d, 0, sizeof(d));
    rc = ens160_read(&ens, &d);
    if (rc == 1 && d.aqi_uba == 0)
        rc = 0;                     /* UBA is 1..5: an all-zero frame is not data */
    if (rc == 1) {
        ens_last = d;
        ens_data_ns = now;
        ens_have_val = 1;
        ens_fail = 0;
        seen_ens = 1;
    } else if (rc < 0) {
        ens_fail++;
    }
    if (rc >= 0 || rc == -EIO) {
        if (d.validity != ens_validity) {
            aqm_log("sense", "ENS160 status: %s%s", ens160_validity_name(d.validity),
                    d.validity == ENS160_WARMUP ? " (~3 min, values approximate)" :
                    d.validity == ENS160_STARTUP ? " (first use, ~1 h)" : "");
            ens_validity = d.validity;
        }
    }

    /* warm-up data is shown (approximate); "invalid", errors or stale = fault */
    if (ens_have_val && ens_fail < I2C_FAIL_MAX && now - ens_data_ns < ENS_STALE_NS &&
        ens_last.validity != ENS160_INVALID) {
        s->tvoc_ppb = ens_last.tvoc_ppb;
        s->eco2_ppm = ens_last.eco2_ppm;
    } else {
        s->tvoc_ppb = s->eco2_ppm = 0;
        s->status |= AQM_ST_ENS_FAULT;
    }
    if (rc < 0 && rc != -EIO && ens_fail >= I2C_REOPEN) {
        aqm_log("sense", "ENS160 gone (%s): re-probing", strerror(-rc));
        ens160_close(&ens);
        have_ens = 0;
        ens_last_rc = 1;
    } else {
        int64_t since = ens_data_ns > ens_ready_ns ? ens_data_ns : ens_ready_ns;

        if (now - since > ENS_STALL_NS) {
            aqm_log("sense", "ENS160 stalled (no new data for %lld s, status 0x%02x): "
                    "resetting it", (long long)((now - since) / 1000000000LL),
                    ens.last_status);
            ens160_close(&ens);
            have_ens = 0;
            ens_last_rc = 1;
            i2c_retry_ns = 0;       /* re-open (reset + standard mode) on the next tick */
        }
    }
}

static void i2c_apply(aqm_sample_t *s, int64_t now)
{
    if (!strcmp(i2c_dev, "none"))
        return;
    if (!have_aht || !have_ens)
        i2c_try_open(now);
    aht_apply(s, now);
    ens_apply(s, now);
}

/* ---------------------------------- main ---------------------------------- */

int main(int argc, char **argv)
{
    int prio = 80, period_ms = 1000, opt;
    const char *pms_dev = "/dev/ttyS1";
    int64_t period_ns, jmin = INT64_MAX, jmax = 0, jsum = 0;
    uint32_t seq = 0, n = 0;
    struct timespec next;
    aqm_shm_t *shm;

    while ((opt = getopt(argc, argv, "p:t:P:I:R")) != -1) {
        switch (opt) {
        case 'R': strict = 1; break;
        case 'p': prio = atoi(optarg); break;
        case 't': period_ms = atoi(optarg); break;
        case 'P': pms_dev = optarg; break;
        case 'I': i2c_dev = optarg; break;
        default:
            fprintf(stderr, "usage: %s [-p rt_prio (0 = off)] [-t period_ms] "
                    "[-P /dev/ttyS1 | none] [-I /dev/i2c-2 | none] [-R]\n"
                    "  -R  real board: a missing sensor is a FAULT, never simulated\n", argv[0]);
            return 2;
        }
    }

    aqm_signals_init();

    shm = aqm_shm_create();
    if (!shm) {
        aqm_log("sense", "shm create failed: %s", strerror(errno));
        return 1;
    }

    if (strict && strcmp(i2c_dev, "none"))
        seen_aht = seen_ens = 1;             /* missing from the start = FAULT */
    pms_init(pms_dev);
    if (!strcmp(i2c_dev, "none"))
        aqm_log("sense", "ENS160 / AHT21 disabled: simulated TVOC / eCO2 / T / RH");
    else
        i2c_try_open(mono_ns());
    setup_rt(prio);
    aqm_log("sense", "running: period %d ms, SCHED_FIFO prio %d", period_ms, prio);

    period_ns = (int64_t)period_ms * 1000000LL;
    clock_gettime(CLOCK_MONOTONIC, &next);

    while (!aqm_should_stop()) {
        aqm_sample_t s;
        int64_t now, late;
        int rc;

        /* absolute deadline: no drift, even if one cycle runs long */
        ts_add_ns(&next, period_ns);
        do {
            rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
        } while (rc == EINTR && !aqm_should_stop());
        if (aqm_should_stop())
            break;

        now  = mono_ns();
        late = now - ts_to_ns(&next);              /* wake-up jitter */
        if (late < jmin) jmin = late;
        if (late > jmax) jmax = late;
        jsum += late;

        memset(&s, 0, sizeof(s));
        s.seq       = seq++;
        s.t_mono_ns = now;
        s.t_wall_s  = (int64_t)time(NULL);
        sim_read(&s);              /* fallback values for absent sensors */
        i2c_apply(&s, now);        /* real TVOC / eCO2 / T / RH          */
        pms_apply(&s, now);        /* real PM1.0 / PM2.5 / PM10          */

        if (aqm_ring_push(&shm->ring[AQM_RING_SENSE_CAPTURE], &s) == -EAGAIN)
            aqm_log("sense", "ring full, dropped seq %u", s.seq);

        if (++n == 10) {
            aqm_log("sense", "seq %u | wake jitter us: min %lld avg %lld max %lld",
                    s.seq, (long long)(jmin / 1000),
                    (long long)(jsum / n / 1000), (long long)(jmax / 1000));
            if (have_pms)
                aqm_log("sense", "PMS5003: %u frames ok, %u bad checksum, %u bad length, "
                        "%u resync bytes | PM2.5 %u", pms.frames_ok, pms.bad_checksum,
                        pms.bad_length, pms.resync_bytes, s.pm25);
            if (have_aht)
                aqm_log("sense", "AHT21: %u ok, %u busy, %u bad CRC, %u bus errors | %.1f C %.1f %%RH",
                        aht.frames_ok, aht.busy, aht.crc_errors, aht.io_errors,
                        aht21_temp_c(aht_t_last), aht21_rh(aht_rh_last));
            if (have_ens)
                aqm_log("sense", "ENS160: %u reads, %u bus errors, %s | TVOC %u ppb eCO2 %u ppm UBA %u",
                        ens.reads_ok, ens.io_errors, ens160_validity_name(ens_validity),
                        ens_last.tvoc_ppb, ens_last.eco2_ppm, ens_last.aqi_uba);
            n = 0; jmin = INT64_MAX; jmax = 0; jsum = 0;
        }
    }

    aqm_log("sense", "stopping");
    if (have_pms)
        pms5003_close(&pms);
    if (have_aht)
        aht21_close(&aht);
    if (have_ens)
        ens160_close(&ens);
    aqm_stop(shm);              /* tell every other stage to exit */
    aqm_shm_detach(shm);
    aqm_shm_destroy();
    return 0;
}
