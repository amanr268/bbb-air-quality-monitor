#include "aqm/calib.h"
#include "aqm/log.h"
#include "aqm/proc.h"
#include "aqm/shm.h"
#include "aqm/sig.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static const char *calib_path(void)
{
    const char *p = getenv("AQM_CALIB");

    return p ? p : "config/calib.conf";
}

static void load_calib(aqm_calib_t *cal)
{
    aqm_calib_t t;
    int rc;

    aqm_calib_defaults(&t);
    rc = aqm_calib_load(&t, calib_path());
    if (rc) {
        aqm_log("convert", "calib %s: %s (keeping current values)",
                calib_path(), strerror(-rc));
        return;
    }
    *cal = t;
    aqm_log("convert", "calib loaded: pm2.5 x%.2f %+.1f | pm10 x%.2f %+.1f | temp %+.1f | rh %+.1f",
            cal->pm25_gain, cal->pm25_offset, cal->pm10_gain, cal->pm10_offset,
            cal->temp_offset, cal->rh_offset);
}

/* raw -> calibrated now lives in calib.c (aqm_calib_apply), shared with
 * report, which replays stored rows through the same conversion */
static void convert(aqm_sample_t *s, const aqm_calib_t *cal)
{
    aqm_calib_apply(cal, s);
}

int main(int argc, char **argv)
{
    int prio = aqm_parse_prio(argc, argv, 60);
    aqm_calib_t cal;
    aqm_shm_t *shm;

    aqm_signals_init();
    aqm_calib_defaults(&cal);
    load_calib(&cal);

    shm = aqm_attach_wait("convert");
    if (!shm)
        return 0;
    aqm_rt_setup("convert", prio);

    while (!aqm_should_stop() && aqm_running(shm)) {
        aqm_sample_t s;
        int rc;

        if (aqm_reload_requested())           /* SIGHUP: re-read calibration */
            load_calib(&cal);

        rc = aqm_ring_pop(&shm->ring[AQM_RING_CAPTURE_CONVERT], &s, 500);
        if (rc == -ETIMEDOUT || rc == -EINTR)
            continue;
        if (rc) {
            aqm_log("convert", "pop failed: %s", strerror(-rc));
            break;
        }

        convert(&s, &cal);

        if (s.seq % 10 == 0)
            aqm_log("convert", "seq %u | %.2f C %.1f %%RH | pm2.5 %u -> %.1f | pm10 %u -> %.1f",
                    s.seq, s.temp_c, s.rh_pct, s.pm25, s.pm25_cal, s.pm10, s.pm10_cal);

        if (aqm_ring_push(&shm->ring[AQM_RING_CONVERT_COMPARE], &s) == -EAGAIN)
            aqm_log("convert", "ring full, dropped seq %u", s.seq);
    }

    aqm_log("convert", "stopping");
    aqm_shm_detach(shm);
    return 0;
}
