#include "aqm/avg.h"

#include "aqm/aqi.h"

#include <string.h>

static const int WIN_LEN[AQM_W_24H] = { 60, 300, 600, 1200 };

void aqm_avg_init(aqm_avg_t *a)
{
    memset(a, 0, sizeof(*a));
}

/* which metrics of a sample are usable */
static uint8_t valid_mask(const aqm_sample_t *s)
{
    uint8_t ok = 0;

    if (!(s->status & AQM_ST_PMS_FAULT))
        ok |= (1u << AQM_M_PM25) | (1u << AQM_M_PM10);
    if (!(s->status & AQM_ST_ENS_FAULT))
        ok |= 1u << AQM_M_TVOC;
    if (!(s->status & AQM_ST_AHT_FAULT))
        ok |= (1u << AQM_M_TEMP) | (1u << AQM_M_RH);
    return ok;
}

/* close the current minute and slide the 24 h ring */
static void end_minute(aqm_avg_t *a)
{
    int m, old = a->dhead;          /* slot being overwritten (oldest minute) */
    uint8_t ok = 0;

    for (m = 0; m < AQM_M_COUNT; m++) {
        if (a->day_ok[old] & (1u << m)) {
            a->dsum[m] -= a->day[old][m];
            a->dcnt[m]--;
        }
        if (a->mcnt[m] * 2 >= a->msec) {           /* >= half the minute valid */
            a->day[old][m] = (float)(a->msum[m] / a->mcnt[m]);
            a->dsum[m] += a->day[old][m];
            a->dcnt[m]++;
            ok |= 1u << m;
        }
        a->msum[m] = 0;
        a->mcnt[m] = 0;
    }
    a->day_ok[old] = ok;
    a->dhead = (a->dhead + 1) % AQM_AVG_DAY;
    a->msec = 0;
}

int aqm_avg_push(aqm_avg_t *a, const aqm_sample_t *s)
{
    float v[AQM_M_COUNT];
    uint8_t ok = valid_mask(s);
    int w, m;

    v[AQM_M_PM25] = s->pm25_cal;
    v[AQM_M_PM10] = s->pm10_cal;
    v[AQM_M_TVOC] = (float)s->tvoc_ppb;
    v[AQM_M_TEMP] = s->temp_c;
    v[AQM_M_RH]   = s->rh_pct;

    /* 1) drop the sample that leaves each full window (read before the ring
     *    slot is overwritten: for the 20 min window it is the head slot) */
    for (w = 0; w < AQM_W_24H; w++) {
        int idx;

        if (a->pushed < (uint32_t)WIN_LEN[w])
            continue;
        idx = (a->head - WIN_LEN[w] + AQM_AVG_RING) % AQM_AVG_RING;
        for (m = 0; m < AQM_M_COUNT; m++)
            if (a->sec_ok[idx] & (1u << m)) {
                a->sum[w][m] -= a->sec[idx][m];
                a->cnt[w][m]--;
            }
    }

    /* 2) store the new sample and add it everywhere */
    memcpy(a->sec[a->head], v, sizeof(v));
    a->sec_ok[a->head] = ok;
    a->head = (a->head + 1) % AQM_AVG_RING;
    a->pushed++;
    for (m = 0; m < AQM_M_COUNT; m++) {
        if (!(ok & (1u << m)))
            continue;
        for (w = 0; w < AQM_W_24H; w++) {
            a->sum[w][m] += v[m];
            a->cnt[w][m]++;
        }
        a->msum[m] += v[m];
        a->mcnt[m]++;
    }

    /* 3) minute bookkeeping for the 24 h mean */
    if (++a->msec == 60) {
        end_minute(a);
        return 1;
    }
    return 0;
}

int aqm_avg_get(const aqm_avg_t *a, int w, int m, float *mean)
{
    int n;

    if (w < 0 || w >= AQM_W_COUNT || m < 0 || m >= AQM_M_COUNT)
        return 0;
    if (w == AQM_W_24H) {
        n = a->dcnt[m];
        if (n > 0)
            *mean = (float)(a->dsum[m] / n);
        return n;
    }
    n = a->cnt[w][m];
    if (n > 0)
        *mean = (float)(a->sum[w][m] / n);
    return n;
}

int aqm_avg_aqi_24h(const aqm_avg_t *a, int *minutes)
{
    float pm25, pm10;
    int n25 = aqm_avg_get(a, AQM_W_24H, AQM_M_PM25, &pm25);
    int n10 = aqm_avg_get(a, AQM_W_24H, AQM_M_PM10, &pm10);
    int a25, a10;

    if (minutes)
        *minutes = n25 > n10 ? n25 : n10;
    if (n25 < AQM_AVG_DAY_MIN && n10 < AQM_AVG_DAY_MIN)
        return -1;
    a25 = n25 >= AQM_AVG_DAY_MIN ? aqm_aqi_pm25(pm25) : 0;
    a10 = n10 >= AQM_AVG_DAY_MIN ? aqm_aqi_pm10(pm10) : 0;
    return a25 > a10 ? a25 : a10;
}

const char *aqm_avg_window_name(int w)
{
    static const char *n[AQM_W_COUNT] = { "1m", "5m", "10m", "20m", "24h" };

    return (w >= 0 && w < AQM_W_COUNT) ? n[w] : "?";
}
