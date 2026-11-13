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
static int64_t   pms_last_ns;

static void pms_init(const char *dev)
{
    int rc;

    if (!strcmp(dev, "none")) {
        aqm_log("sense", "PMS5003 disabled: simulated PM values");
        return;
    }
    if ((rc = pms5003_open(&pms, dev)) != 0) {
        aqm_log("sense", "PMS5003 on %s not available (%s): simulated PM values",
                dev, strerror(-rc));
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

    if (!have_pms)
        return;
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

/* ---------------------------------- main ---------------------------------- */

int main(int argc, char **argv)
{
    int prio = 80, period_ms = 1000, opt;
    const char *pms_dev = "/dev/ttyS1";
    int64_t period_ns, jmin = INT64_MAX, jmax = 0, jsum = 0;
    uint32_t seq = 0, n = 0;
    struct timespec next;
    aqm_shm_t *shm;

    while ((opt = getopt(argc, argv, "p:t:P:")) != -1) {
        switch (opt) {
        case 'p': prio = atoi(optarg); break;
        case 't': period_ms = atoi(optarg); break;
        case 'P': pms_dev = optarg; break;
        default:
            fprintf(stderr, "usage: %s [-p rt_prio (0 = off)] [-t period_ms] "
                    "[-P /dev/ttyS1 | none]\n", argv[0]);
            return 2;
        }
    }

    aqm_signals_init();

    shm = aqm_shm_create();
    if (!shm) {
        aqm_log("sense", "shm create failed: %s", strerror(errno));
        return 1;
    }

    pms_init(pms_dev);
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
        sim_read(&s);              /* TVOC / temp / RH still simulated */
        pms_apply(&s, now);        /* real PM1.0 / PM2.5 / PM10         */

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
            n = 0; jmin = INT64_MAX; jmax = 0; jsum = 0;
        }
    }

    aqm_log("sense", "stopping");
    if (have_pms)
        pms5003_close(&pms);
    aqm_stop(shm);              /* tell every other stage to exit */
    aqm_shm_detach(shm);
    aqm_shm_destroy();
    return 0;
}
