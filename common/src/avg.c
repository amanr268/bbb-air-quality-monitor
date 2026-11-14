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

/* append one minute to the 24 h ring (sum/cnt per metric over `secs` s);
 * a metric counts if at least half of the minute was valid */
static void day_push(aqm_avg_t *a, const double *sum, const int *cnt, int secs)
{
    int m, old = a->dhead;          /* slot being overwritten (oldest minute) */
    uint8_t ok = 0;

    for (m = 0; m < AQM_M_COUNT; m++) {
        if (a->day_ok[old] & (1u << m)) {
            a->dsum[m] -= a->day[old][m];
            a->dcnt[m]--;
        }
        if (cnt[m] > 0 && cnt[m] * 2 >= secs) {
            a->day[old][m] = (float)(sum[m] / cnt[m]);
            a->dsum[m] += a->day[old][m];
            a->dcnt[m]++;
            ok |= 1u << m;
        }
    }
    a->day_ok[old] = ok;
    a->dhead = (a->dhead + 1) % AQM_AVG_DAY;
}

/* close the current minute of live samples */
static void end_minute(aqm_avg_t *a)
{
    day_push(a, a->msum, a->mcnt, a->msec);
    memset(a->msum, 0, sizeof(a->msum));
    memset(a->mcnt, 0, sizeof(a->mcnt));
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

/* ---- seeding from history ------------------------------------------------ */

static void seed_flush(aqm_avg_t *a)
{
    if (a->seed_min < 0)
        return;
    day_push(a, a->ssum, a->scnt, 60);
    memset(a->ssum, 0, sizeof(a->ssum));
    memset(a->scnt, 0, sizeof(a->scnt));
}

/* emit "no data" minutes until the bucket for minute `upto` is current */
static void seed_advance(aqm_avg_t *a, int64_t upto)
{
    static const double z[AQM_M_COUNT];
    static const int zc[AQM_M_COUNT];

    if (a->seed_min < 0) {
        a->seed_min = upto;
        return;
    }
    if (upto <= a->seed_min)
        return;
    seed_flush(a);
    /* gaps (board off): at most a day's worth of empty minutes */
    for (a->seed_min++; a->seed_min < upto; a->seed_min++)
        if (upto - a->seed_min <= AQM_AVG_DAY)
            day_push(a, z, zc, 60);
}

void aqm_avg_seed_begin(aqm_avg_t *a, int64_t now_wall_s)
{
    aqm_avg_init(a);
    a->seed_now = now_wall_s;
    a->seed_min = -1;
    a->seed_live_from = now_wall_s - AQM_AVG_RING;   /* last 20 min: live path */
}

void aqm_avg_seed_row(aqm_avg_t *a, const aqm_sample_t *s)
{
    uint8_t ok;
    int m;

    if (s->t_wall_s < a->seed_now - 86400 || s->t_wall_s > a->seed_now)
        return;
    if (s->t_wall_s >= a->seed_live_from) {
        /* close the minute buckets once, then behave like live data */
        if (a->seed_min >= 0) {
            seed_flush(a);
            a->seed_min = -2;               /* -2: bucket path finished */
        }
        aqm_avg_push(a, s);
        return;
    }
    if (a->seed_min == -2)
        return;                             /* out of order: ignore */
    seed_advance(a, s->t_wall_s / 60);
    ok = valid_mask(s);
    {
        float v[AQM_M_COUNT] = { s->pm25_cal, s->pm10_cal, (float)s->tvoc_ppb,
                                 s->temp_c, s->rh_pct };
        for (m = 0; m < AQM_M_COUNT; m++)
            if (ok & (1u << m)) {
                a->ssum[m] += v[m];
                a->scnt[m]++;
            }
    }
}

int aqm_avg_seed_end(aqm_avg_t *a)
{
    int m, best = 0;

    if (a->seed_min >= 0) {
        /* no recent rows: account for the time since the last stored minute */
        seed_advance(a, a->seed_now / 60);
        a->seed_min = -2;
    }
    for (m = 0; m < AQM_M_COUNT; m++)
        if (a->dcnt[m] > best)
            best = a->dcnt[m];
    a->seeded_min = best;
    return best;
}
