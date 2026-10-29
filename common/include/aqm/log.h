#ifndef AQM_LOG_H
#define AQM_LOG_H

void aqm_log(const char *proc, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#endif
