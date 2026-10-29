#include "aqm/log.h"
#include "aqm/proc.h"
#include "aqm/shm.h"
#include "aqm/sig.h"
#include "aqm/timeutil.h"

#include <errno.h>
#include <string.h>

int main(int argc, char **argv)
{
    int prio = aqm_parse_prio(argc, argv, 70);
    aqm_shm_t *shm;

    aqm_signals_init();
    shm = aqm_attach_wait("capture");
    if (!shm)
        return 0;
    aqm_rt_setup("capture", prio);

    while (!aqm_should_stop() && aqm_running(shm)) {
        aqm_sample_t s;
        int rc = aqm_ring_pop(&shm->ring[AQM_RING_SENSE_CAPTURE], &s, 500);

        if (rc == -ETIMEDOUT || rc == -EINTR)
            continue;                 /* re-check stop flags */
        if (rc) {
            aqm_log("capture", "pop failed: %s", strerror(-rc));
            break;
        }

        aqm_log("capture", "seq %u pm2.5 %u tvoc %u status 0x%x | ipc latency %lld us",
                s.seq, s.pm25, s.tvoc_ppb, s.status,
                (long long)((mono_ns() - s.t_mono_ns) / 1000));

        /* TODO Phase 3: persist to SQLite for the 48 h offline buffer */

        if (aqm_ring_push(&shm->ring[AQM_RING_CAPTURE_CONVERT], &s) == -EAGAIN)
            aqm_log("capture", "ring full, dropped seq %u", s.seq);
    }

    aqm_log("capture", "stopping");
    aqm_shm_detach(shm);
    return 0;
}
