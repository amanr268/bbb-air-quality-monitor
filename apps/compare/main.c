#include "aqm/alarm.h"
#include "aqm/aqi.h"
#include "aqm/log.h"
#include "aqm/proc.h"
#include "aqm/shm.h"
#include "aqm/sig.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define TVOC_HIGH_PPB   500     /* raise above this        */
#define TVOC_CLEAR_PPB  400     /* clear below this (hysteresis) */

#define ALM_BIT_AQI_POOR  (1u << 0)
#define ALM_BIT_FAULT     (1u << 1)
#define ALM_BIT_TVOC      (1u << 2)

static void raise_alarm(mqd_t q, const aqm_sample_t *s, uint8_t type,
                        unsigned prio, const char *fmt, ...)
    __attribute__((format(printf, 5, 6)));

static void raise_alarm(mqd_t q, const aqm_sample_t *s, uint8_t type,
                        unsigned prio, const char *fmt, ...)
{
    aqm_alarm_t a;
    va_list ap;

    memset(&a, 0, sizeof(a));
    a.seq      = s->seq;
    a.type     = type;
    a.prio     = (uint8_t)prio;
    a.value    = s->aqi;
    a.t_wall_s = s->t_wall_s;

    va_start(ap, fmt);
    vsnprintf(a.text, sizeof(a.text), fmt, ap);
    va_end(ap);

    /* queue is O_NONBLOCK: never stall the pipeline on a slow reader */
    if (aqm_alarm_send(q, &a) != 0)
        aqm_log("compare", "alarm queue full, dropped: %s", a.text);
}

static unsigned prio_for(aqm_aqi_cat_t c)
{
    if (c >= AQI_SEVERE)
        return AQM_PRIO_CRIT;
    if (c >= AQI_POOR)
        return AQM_PRIO_WARN;
    return AQM_PRIO_INFO;
}

int main(int argc, char **argv)
{
    int prio = aqm_parse_prio(argc, argv, 50);
    int last_cat = -1, tvoc_high = 0;
    uint32_t last_status = 0;
    aqm_shm_t *shm;
    mqd_t q;

    aqm_signals_init();
    shm = aqm_attach_wait("compare");
    if (!shm)
        return 0;
    aqm_rt_setup("compare", prio);

    q = aqm_alarm_open(1);
    if (q == (mqd_t)-1) {
        aqm_log("compare", "mq_open failed: %s", strerror(errno));
        aqm_shm_detach(shm);
        return 1;
    }

    while (!aqm_should_stop() && aqm_running(shm)) {
        aqm_sample_t s;
        aqm_aqi_cat_t cat;
        uint32_t new_faults, cleared;
        int a25, a10, rc;

        rc = aqm_ring_pop(&shm->ring[AQM_RING_CONVERT_COMPARE], &s, 500);
        if (rc == -ETIMEDOUT || rc == -EINTR)
            continue;
        if (rc) {
            aqm_log("compare", "pop failed: %s", strerror(-rc));
            break;
        }

        /* overall AQI = worst sub-index */
        a25   = aqm_aqi_pm25(s.pm25_cal);
        a10   = aqm_aqi_pm10(s.pm10_cal);
        s.aqi = (uint16_t)(a25 > a10 ? a25 : a10);
        cat   = aqm_aqi_category(s.aqi);
        s.aqi_cat = (uint8_t)cat;
        s.alarms  = 0;

        /* edge-triggered: alarm on category CHANGE, not every second */
        if (!(s.status & AQM_ST_PMS_FAULT) && (int)cat != last_cat) {
            if (last_cat < 0)
                raise_alarm(q, &s, AQM_ALM_AQI, AQM_PRIO_INFO,
                            "AQI %u %s (initial)", s.aqi, aqm_aqi_cat_name(cat));
            else
                raise_alarm(q, &s, AQM_ALM_AQI, prio_for(cat), "AQI %u: %s -> %s",
                            s.aqi, aqm_aqi_cat_name((aqm_aqi_cat_t)last_cat),
                            aqm_aqi_cat_name(cat));
            last_cat = (int)cat;
        }
        if (cat >= AQI_POOR)
            s.alarms |= ALM_BIT_AQI_POOR;

        /* sensor faults: report new and cleared bits */
        new_faults = s.status & ~last_status;
        cleared    = last_status & ~s.status;
        if (new_faults)
            raise_alarm(q, &s, AQM_ALM_FAULT, AQM_PRIO_CRIT,
                        "sensor fault 0x%x", (unsigned)new_faults);
        if (cleared)
            raise_alarm(q, &s, AQM_ALM_FAULT, AQM_PRIO_INFO,
                        "sensor fault cleared 0x%x", (unsigned)cleared);
        last_status = s.status;
        if (s.status)
            s.alarms |= ALM_BIT_FAULT;

        /* TVOC with hysteresis: no chattering around one threshold */
        if (!tvoc_high && s.tvoc_ppb > TVOC_HIGH_PPB) {
            tvoc_high = 1;
            raise_alarm(q, &s, AQM_ALM_TVOC, AQM_PRIO_WARN, "TVOC high %u ppb", s.tvoc_ppb);
        } else if (tvoc_high && s.tvoc_ppb < TVOC_CLEAR_PPB) {
            tvoc_high = 0;
            raise_alarm(q, &s, AQM_ALM_TVOC, AQM_PRIO_INFO, "TVOC normal %u ppb", s.tvoc_ppb);
        }
        if (tvoc_high)
            s.alarms |= ALM_BIT_TVOC;

        if (aqm_ring_push(&shm->ring[AQM_RING_COMPARE_REPORT], &s) == -EAGAIN)
            aqm_log("compare", "ring full, dropped seq %u", s.seq);
    }

    aqm_log("compare", "stopping");
    mq_close(q);
    aqm_shm_detach(shm);
    return 0;
}
