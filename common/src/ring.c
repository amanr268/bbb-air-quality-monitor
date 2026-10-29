#include "aqm/ring.h"

#include <errno.h>
#include <time.h>

static int ring_lock(aqm_ring_t *r)
{
    int rc = pthread_mutex_lock(&r->lock);

    if (rc == EOWNERDEAD) {
        /* previous owner died holding the lock: data is still consistent
         * because every critical section is a single copy + index update */
        pthread_mutex_consistent(&r->lock);
        rc = 0;
    }
    return -rc;
}

static void deadline_ms(struct timespec *ts, int ms)
{
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec  += ms / 1000;
    ts->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

int aqm_ring_init(aqm_ring_t *r)
{
    pthread_mutexattr_t a;
    int rc;

    pthread_mutexattr_init(&a);
    pthread_mutexattr_setpshared(&a, PTHREAD_PROCESS_SHARED);
    pthread_mutexattr_setprotocol(&a, PTHREAD_PRIO_INHERIT);
    pthread_mutexattr_setrobust(&a, PTHREAD_MUTEX_ROBUST);
    rc = pthread_mutex_init(&r->lock, &a);
    pthread_mutexattr_destroy(&a);
    if (rc)
        return -rc;

    /* pshared = 1: semaphores live in shared memory, used across processes */
    if (sem_init(&r->items, 1, 0) || sem_init(&r->spaces, 1, AQM_RING_LEN))
        return -errno;

    r->head = r->tail = 0;
    r->dropped = 0;
    return 0;
}

int aqm_ring_push(aqm_ring_t *r, const aqm_sample_t *s)
{
    int rc;

    if (sem_trywait(&r->spaces) != 0) {          /* full: never block */
        if (ring_lock(r) == 0) {
            r->dropped++;
            pthread_mutex_unlock(&r->lock);
        }
        return -EAGAIN;
    }

    rc = ring_lock(r);
    if (rc) {
        sem_post(&r->spaces);
        return rc;
    }
    r->buf[r->head % AQM_RING_LEN] = *s;
    r->head++;
    pthread_mutex_unlock(&r->lock);

    sem_post(&r->items);
    return 0;
}

int aqm_ring_pop(aqm_ring_t *r, aqm_sample_t *out, int timeout_ms)
{
    struct timespec ts;
    int rc;

    deadline_ms(&ts, timeout_ms);
    if (sem_timedwait(&r->items, &ts) != 0)
        return -errno;                            /* ETIMEDOUT or EINTR */

    rc = ring_lock(r);
    if (rc) {
        sem_post(&r->items);
        return rc;
    }
    *out = r->buf[r->tail % AQM_RING_LEN];
    r->tail++;
    pthread_mutex_unlock(&r->lock);

    sem_post(&r->spaces);
    return 0;
}
