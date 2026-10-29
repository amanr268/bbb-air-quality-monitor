#ifndef AQM_ALARM_H
#define AQM_ALARM_H

#include <mqueue.h>
#include <stdint.h>

#define AQM_MQ_NAME "/aqm_alarm"

/* alarm types */
enum { AQM_ALM_AQI = 1, AQM_ALM_FAULT, AQM_ALM_TVOC };

/* POSIX mqueue priority: higher number is delivered first */
enum { AQM_PRIO_INFO = 1, AQM_PRIO_WARN = 5, AQM_PRIO_CRIT = 10 };

typedef struct {
    uint32_t seq;          /* sample that triggered it */
    uint8_t  type;         /* AQM_ALM_*                */
    uint8_t  prio;         /* AQM_PRIO_*               */
    uint16_t value;        /* AQI or raw value         */
    int64_t  t_wall_s;
    char     text[48];
} aqm_alarm_t;

mqd_t aqm_alarm_open(int nonblock);                    /* creates if needed */
int   aqm_alarm_send(mqd_t q, const aqm_alarm_t *a);   /* 0 or -errno       */
int   aqm_alarm_recv(mqd_t q, aqm_alarm_t *a);         /* 0, -EAGAIN if empty */
void  aqm_alarm_unlink(void);

#endif
