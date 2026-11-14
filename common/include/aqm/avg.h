#ifndef AQM_AVG_H
#define AQM_AVG_H

#include <stdint.h>

#include "aqm/types.h"

/*
 * Rolling averages over the 1 Hz sample stream (AirVeda-style 1/5/10/20 min
 * windows) plus a 24 h mean for the CPCB AQI, which is defined on 24 h averages.
 *
 *  - 1..20 min: one ring of the last 1200 one-second samples; each window keeps
 *    a running sum and count, so a push is O(windows x metrics), not O(samples).
 *  - 24 h: per-minute means in a 1440-slot ring (a minute counts if at least
 *    half of its seconds were valid).
 *  - Samples flagged with a sensor fault are left out of that sensor's metrics,
 *    so a dead sensor never drags an average towards 0.
 * Windows count samples, not wall-clock time (the pipeline delivers 1 per s).
 */

enum { AQM_M_PM25 = 0, AQM_M_PM10, AQM_M_TVOC, AQM_M_TEMP, AQM_M_RH, AQM_M_COUNT };
enum { AQM_W_1M = 0, AQM_W_5M, AQM_W_10M, AQM_W_20M, AQM_W_24H, AQM_W_COUNT };

#define AQM_AVG_RING      1200          /* seconds = largest short window   */
#define AQM_AVG_DAY       1440          /* minutes in 24 h                  */
#define AQM_AVG_DAY_MIN   (16 * 60)     /* CPCB: >= 16 h of data for a 24 h AQI */

typedef struct {
    /* last 1200 seconds */
    float    sec[AQM_AVG_RING][AQM_M_COUNT];
    uint8_t  sec_ok[AQM_AVG_RING];          /* bit m = metric m valid       */
    int      head;
    uint32_t pushed;
    double   sum[AQM_W_24H][AQM_M_COUNT];
    int      cnt[AQM_W_24H][AQM_M_COUNT];

    /* current minute */
    double   msum[AQM_M_COUNT];
    int      mcnt[AQM_M_COUNT];
    int      msec;

    /* last 1440 minutes */
    float    day[AQM_AVG_DAY][AQM_M_COUNT];
    uint8_t  day_ok[AQM_AVG_DAY];
    int      dhead;
    double   dsum[AQM_M_COUNT];
    int      dcnt[AQM_M_COUNT];

    /* seeding state (aqm_avg_seed_*) */
    int64_t  seed_now, seed_min, seed_live_from;
    double   ssum[AQM_M_COUNT];
    int      scnt[AQM_M_COUNT];
    int      seeded_min;
} aqm_avg_t;

void aqm_avg_init(aqm_avg_t *a);

/* add one sample (use the calibrated values). Returns 1 when a minute just
 * completed (handy for once-a-minute logging), else 0. */
int  aqm_avg_push(aqm_avg_t *a, const aqm_sample_t *s);

/* mean of metric m over window w. Returns the number of valid samples
 * (minutes for AQM_W_24H) behind it; 0 = no data, *mean untouched. */
int  aqm_avg_get(const aqm_avg_t *a, int w, int m, float *mean);

/* CPCB AQI from the 24 h PM2.5/PM10 means. Returns -1 until at least 16 h
 * of PM data exists; *minutes (optional) gets the minutes collected. */
int  aqm_avg_aqi_24h(const aqm_avg_t *a, int *minutes);

const char *aqm_avg_window_name(int w);     /* "1m", "5m", ... "24h" */

/*
 * Seeding from stored history (e.g. the SQLite buffer) after a restart, so
 * the 24 h mean does not start from zero. Feed rows oldest first, already
 * calibrated, with t_wall_s set:
 *   rows older than 20 min  -> grouped by wall-clock minute into the 24 h ring
 *                              (missing minutes are stored as "no data")
 *   rows of the last 20 min -> pushed like live samples (fills 1..20 min windows)
 * aqm_avg_seed_end() returns the number of valid 24 h minutes.
 */
void aqm_avg_seed_begin(aqm_avg_t *a, int64_t now_wall_s);
void aqm_avg_seed_row(aqm_avg_t *a, const aqm_sample_t *s);
int  aqm_avg_seed_end(aqm_avg_t *a);

#endif
