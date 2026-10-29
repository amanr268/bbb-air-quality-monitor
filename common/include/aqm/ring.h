#ifndef AQM_RING_H
#define AQM_RING_H

#include <pthread.h>
#include <semaphore.h>
#include <stdint.h>

#include "aqm/types.h"

#define AQM_RING_LEN 64    /* power of 2 so uint32 wrap-around is safe */

typedef struct {
    pthread_mutex_t lock;   /* pshared + PI + robust */
    sem_t items;            /* filled slots          */
    sem_t spaces;           /* free slots            */
    uint32_t head;          /* next write index      */
    uint32_t tail;          /* next read index       */
    uint64_t dropped;       /* pushes refused (full) */
    aqm_sample_t buf[AQM_RING_LEN];
} aqm_ring_t;

int  aqm_ring_init(aqm_ring_t *r);
/* non-blocking: returns 0, or -EAGAIN if full (sample dropped) */
int  aqm_ring_push(aqm_ring_t *r, const aqm_sample_t *s);
/* blocking with timeout: returns 0, -ETIMEDOUT, or -EINTR */
int  aqm_ring_pop(aqm_ring_t *r, aqm_sample_t *out, int timeout_ms);

#endif
