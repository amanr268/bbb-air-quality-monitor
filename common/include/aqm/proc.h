#ifndef AQM_PROC_H
#define AQM_PROC_H

#include "aqm/shm.h"

/* parse "-p <prio>" (0 = SCHED_OTHER); exits on bad usage */
int        aqm_parse_prio(int argc, char **argv, int default_prio);
/* lock memory, set SCHED_FIFO if prio > 0, log the result */
void       aqm_rt_setup(const char *name, int prio);
/* attach to /aqm_shm, retrying until it exists or a stop signal arrives */
aqm_shm_t *aqm_attach_wait(const char *name);

#endif
