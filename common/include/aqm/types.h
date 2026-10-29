#ifndef AQM_TYPES_H
#define AQM_TYPES_H

#include <stdint.h>

/* status bits */
#define AQM_ST_PMS_FAULT   (1u << 0)
#define AQM_ST_ENS_FAULT   (1u << 1)
#define AQM_ST_AHT_FAULT   (1u << 2)

/* One sample flows through all stages; each stage fills its own fields. */
typedef struct {
    uint32_t seq;          /* sample counter                      */
    uint32_t status;       /* AQM_ST_* fault bits                 */
    int64_t  t_mono_ns;    /* CLOCK_MONOTONIC at sense (latency)  */
    int64_t  t_wall_s;     /* CLOCK_REALTIME seconds (logging)    */

    /* sense: raw sensor values */
    uint16_t pm1, pm25, pm10;      /* ug/m3, PMS5003              */
    uint16_t tvoc_ppb, eco2_ppm;   /* ENS160                      */
    uint32_t aht_t_raw, aht_rh_raw;/* AHT21 20-bit raw            */

    /* convert: engineering units, calibrated */
    float temp_c, rh_pct;
    float pm25_cal, pm10_cal;

    /* compare: AQI result */
    uint16_t aqi;
    uint8_t  aqi_cat;      /* 0 Good .. 5 Severe (CPCB)           */
    uint8_t  alarms;       /* bitmask                             */
} aqm_sample_t;

#endif
