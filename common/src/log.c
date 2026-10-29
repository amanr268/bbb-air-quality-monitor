#include "aqm/log.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

void aqm_log(const char *proc, const char *fmt, ...)
{
    struct timespec ts;
    va_list ap;

    clock_gettime(CLOCK_REALTIME, &ts);
    fprintf(stderr, "[%ld.%03ld] %-8s ", (long)ts.tv_sec, ts.tv_nsec / 1000000, proc);

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
