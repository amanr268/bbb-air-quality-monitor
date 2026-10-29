#include "aqm/proc.h"

#include "aqm/log.h"
#include "aqm/sig.h"

#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

int aqm_parse_prio(int argc, char **argv, int default_prio)
{
    int opt, prio = default_prio;

    while ((opt = getopt(argc, argv, "p:")) != -1) {
        if (opt == 'p') {
            prio = atoi(optarg);
        } else {
            fprintf(stderr, "usage: %s [-p rt_prio (0 = SCHED_OTHER)]\n", argv[0]);
            exit(2);
        }
    }
    return prio;
}

void aqm_rt_setup(const char *name, int prio)
{
    struct sched_param sp = { .sched_priority = prio };

    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
        aqm_log(name, "mlockall failed: %s", strerror(errno));

    if (prio > 0) {
        if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0) {
            aqm_log(name, "SCHED_FIFO %d failed: %s (using SCHED_OTHER)",
                    prio, strerror(errno));
            return;
        }
        aqm_log(name, "policy SCHED_FIFO prio %d", prio);
    } else {
        aqm_log(name, "policy SCHED_OTHER");
    }
}

aqm_shm_t *aqm_attach_wait(const char *name)
{
    aqm_shm_t *shm = NULL;
    int warned = 0;

    while (!aqm_should_stop() && !(shm = aqm_shm_attach())) {
        if (!warned) {
            aqm_log(name, "waiting for shm (%s)", strerror(errno));
            warned = 1;
        }
        usleep(200000);
    }
    if (shm)
        aqm_log(name, "attached");
    return shm;
}
