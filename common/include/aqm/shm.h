#ifndef AQM_SHM_H
#define AQM_SHM_H

#include <stdint.h>

#include "aqm/ring.h"

#define AQM_SHM_NAME   "/aqm_shm"
#define AQM_SHM_MAGIC  0x41514D31u   /* "AQM1" */

enum aqm_ring_id {
    AQM_RING_SENSE_CAPTURE = 0,
    AQM_RING_CAPTURE_CONVERT,
    AQM_RING_CONVERT_COMPARE,
    AQM_RING_COMPARE_REPORT,
    AQM_RING_COUNT
};

/* Sensor health for the fault page: written by sense every second, read by
 * report. Plain counters / last error codes (each field atomic on its own). */
typedef struct {
    uint32_t pms_bytes;     /* bytes received from the PMS5003 since start     */
    uint32_t pms_frames;    /* valid PMS5003 frames since start                */
    uint32_t pms_sensor_err;/* 1 = the last frame had the sensor's error flag  */
    int32_t  aht_err;       /* errno of the last failed AHT21 access, 0 = ok   */
    int32_t  ens_err;       /* errno of the last failed ENS160 access, 0 = ok  */
    uint32_t ens_invalid;   /* 1 = the ENS160 reports "invalid output"         */
    uint32_t ens_stalls;    /* ENS160 stall resets since start                 */
} aqm_health_t;

typedef struct {
    uint32_t magic;        /* written last: attach checks it   */
    uint32_t version;
    int      running;      /* 1 = run, 0 = all stages shut down */
    aqm_ring_t ring[AQM_RING_COUNT];
    aqm_health_t health;   /* sense -> report (fault page)      */
} aqm_shm_t;

aqm_shm_t *aqm_shm_create(void);   /* owner: creates + initialises */
aqm_shm_t *aqm_shm_attach(void);   /* other processes              */
void       aqm_shm_detach(aqm_shm_t *p);
int        aqm_shm_destroy(void);  /* removes /dev/shm/aqm_shm     */

static inline int aqm_running(aqm_shm_t *p)
{
    return __atomic_load_n(&p->running, __ATOMIC_ACQUIRE);
}

static inline void aqm_stop(aqm_shm_t *p)
{
    __atomic_store_n(&p->running, 0, __ATOMIC_RELEASE);
}

#endif
