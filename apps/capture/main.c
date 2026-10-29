#include "aqm/log.h"
#include "aqm/shm.h"
#include "aqm/sig.h"
#include "aqm/timeutil.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    aqm_shm_t *shm = NULL;

    aqm_signals_init();

    /* wait for sense to create the shared memory */
    while (!aqm_should_stop() && !(shm = aqm_shm_attach())) {
        aqm_log("capture", "waiting for shm (%s)", strerror(errno));
        sleep(1);
    }
    if (!shm)
        return 0;

    aqm_log("capture", "attached");

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
    }

    aqm_log("capture", "stopping");
    aqm_shm_detach(shm);
    return 0;
}
