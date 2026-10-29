#ifndef AQM_TIMEUTIL_H
#define AQM_TIMEUTIL_H

#include <stdint.h>
#include <time.h>

#define NSEC_PER_SEC 1000000000LL

static inline int64_t ts_to_ns(const struct timespec *t)
{
    return (int64_t)t->tv_sec * NSEC_PER_SEC + t->tv_nsec;
}

static inline int64_t mono_ns(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return ts_to_ns(&t);
}

static inline void ts_add_ns(struct timespec *t, int64_t ns)
{
    ns += t->tv_nsec;
    t->tv_sec  += ns / NSEC_PER_SEC;
    t->tv_nsec  = ns % NSEC_PER_SEC;
}

#endif
