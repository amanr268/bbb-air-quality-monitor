#include "aqm/alarm.h"
#include "aqm/aqi.h"
#include "aqm/log.h"
#include "aqm/proc.h"
#include "aqm/shm.h"
#include "aqm/sig.h"
#include "aqm/timeutil.h"

#include <errno.h>
#include <string.h>

/* print every queued alarm; the mqueue hands them over highest priority first */
static void drain_alarms(mqd_t q)
{
    aqm_alarm_t a;

    while (aqm_alarm_recv(q, &a) == 0)
        aqm_log("report", "ALARM p%-2u seq %u: %s", a.prio, a.seq, a.text);
}

int main(int argc, char **argv)
{
    int prio = aqm_parse_prio(argc, argv, 0);
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

    while (!aqm_should_stop() && aqm_running(shm)) {
        aqm_sample_t s;
        int rc = aqm_ring_pop(&shm->ring[AQM_RING_COMPARE_REPORT], &s, 500);

        drain_alarms(q);

        if (rc == -ETIMEDOUT || rc == -EINTR)
            continue;
        if (rc) {
            aqm_log("report", "pop failed: %s", strerror(-rc));
            break;
        }

        /* TODO Phase 5: MQTT publish, OLED, Modbus registers */
        aqm_log("report",
                "seq %u | PM2.5 %3.0f PM10 %3.0f ug/m3 | TVOC %3u ppb | %4.1f C %2.0f%%RH | "
                "AQI %3u %-12s | e2e %lld us",
                s.seq, s.pm25_cal, s.pm10_cal, s.tvoc_ppb, s.temp_c, s.rh_pct,
                s.aqi, aqm_aqi_cat_name((aqm_aqi_cat_t)s.aqi_cat),
                (long long)((mono_ns() - s.t_mono_ns) / 1000));
    }

    aqm_log("report", "stopping");
    mq_close(q);
    aqm_alarm_unlink();
    aqm_shm_detach(shm);
    return 0;
}
