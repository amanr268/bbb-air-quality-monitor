#include "aqm/log.h"
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
    static int pm25 = 60, tvoc = 150;
    static double temp_c = 28.0, rh = 55.0;

    pm25 += noise(3) + (60 - pm25) / 20;          /* drift back to ~60   */
    if (rng() % 120 == 0)
        pm25 += 150;                              /* pollution spike     */
    if (pm25 < 5)
        pm25 = 5;

    tvoc += noise(10) + (150 - tvoc) / 20;
    if (tvoc < 0)
        tvoc = 0;

    temp_c += noise(1) * 0.05;
    rh     += noise(1) * 0.2;

    s->pm25     = (uint16_t)pm25;
    s->pm1      = (uint16_t)(pm25 * 7 / 10);
    s->pm10     = (uint16_t)(pm25 * 16 / 10);
    s->tvoc_ppb = (uint16_t)tvoc;
    s->eco2_ppm = (uint16_t)(400 + tvoc);

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

/* ---------------------------------- main ---------------------------------- */

int main(int argc, char **argv)
{
    int prio = 80, period_ms = 1000, opt;
    int64_t period_ns, jmin = INT64_MAX, jmax = 0, jsum = 0;
    uint32_t seq = 0, n = 0;
    struct timespec next;
    aqm_shm_t *shm;

    while ((opt = getopt(argc, argv, "p:t:")) != -1) {
        switch (opt) {
        case 'p': prio = atoi(optarg); break;
        case 't': period_ms = atoi(optarg); break;
        default:
            fprintf(stderr, "usage: %s [-p rt_prio (0 = off)] [-t period_ms]\n", argv[0]);
            return 2;
        }
    }

    aqm_signals_init();

    shm = aqm_shm_create();
    if (!shm) {
        aqm_log("sense", "shm create failed: %s", strerror(errno));
        return 1;
    }

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
        sim_read(&s);

        if (aqm_ring_push(&shm->ring[AQM_RING_SENSE_CAPTURE], &s) == -EAGAIN)
            aqm_log("sense", "ring full, dropped seq %u", s.seq);

        if (++n == 10) {
            aqm_log("sense", "seq %u | wake jitter us: min %lld avg %lld max %lld",
                    s.seq, (long long)(jmin / 1000),
                    (long long)(jsum / n / 1000), (long long)(jmax / 1000));
            n = 0; jmin = INT64_MAX; jmax = 0; jsum = 0;
        }
    }

    aqm_log("sense", "stopping");
    aqm_stop(shm);              /* tell every other stage to exit */
    aqm_shm_detach(shm);
    aqm_shm_destroy();
    return 0;
}
