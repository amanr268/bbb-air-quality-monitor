#include "aqm/alarm.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/types.h>

mqd_t aqm_alarm_open(int nonblock)
{
    struct mq_attr at;

    memset(&at, 0, sizeof(at));
    at.mq_maxmsg  = 10;                    /* default per-user limit */
    at.mq_msgsize = sizeof(aqm_alarm_t);

    return mq_open(AQM_MQ_NAME,
                   O_CREAT | O_RDWR | (nonblock ? O_NONBLOCK : 0),
                   0660, &at);
}

int aqm_alarm_send(mqd_t q, const aqm_alarm_t *a)
{
    return mq_send(q, (const char *)a, sizeof(*a), a->prio) ? -errno : 0;
}

int aqm_alarm_recv(mqd_t q, aqm_alarm_t *a)
{
    unsigned int prio;
    ssize_t n = mq_receive(q, (char *)a, sizeof(*a), &prio);

    return n < 0 ? -errno : 0;
}

void aqm_alarm_unlink(void)
{
    mq_unlink(AQM_MQ_NAME);
}
