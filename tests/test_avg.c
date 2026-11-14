/* rolling averages: window means, fault exclusion, ring wrap, 24 h minutes */
#include "aqm/avg.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
    return 1; } } while (0)
#define NEAR(a, b) (fabsf((a) - (b)) < 0.01f)

static aqm_avg_t A;                         /* ~50 KB: keep it off the stack */

static void push(float pm25, uint32_t status)
{
    aqm_sample_t s;

    memset(&s, 0, sizeof(s));
    s.pm25_cal = pm25;
    s.pm10_cal = pm25 * 2;
    s.tvoc_ppb = 100;
    s.temp_c = 25;
    s.rh_pct = 50;
    s.status = status;
    aqm_avg_push(&A, &s);
}

int main(void)
{
    float m;
    int i, mins;

    aqm_avg_init(&A);
    CHECK(aqm_avg_get(&A, AQM_W_1M, AQM_M_PM25, &m) == 0);   /* no data yet */

    /* 60 s of 10 -> 1 min mean 10 */
    for (i = 0; i < 60; i++)
        push(10, 0);
    CHECK(aqm_avg_get(&A, AQM_W_1M, AQM_M_PM25, &m) == 60 && NEAR(m, 10));
    CHECK(aqm_avg_get(&A, AQM_W_1M, AQM_M_PM10, &m) == 60 && NEAR(m, 20));

    /* next 60 s of 30: 1 min window has slid fully, 5 min = mean(10,30) */
    for (i = 0; i < 60; i++)
        push(30, 0);
    CHECK(aqm_avg_get(&A, AQM_W_1M, AQM_M_PM25, &m) == 60 && NEAR(m, 30));
    CHECK(aqm_avg_get(&A, AQM_W_5M, AQM_M_PM25, &m) == 120 && NEAR(m, 20));

    /* PM sensor fault: PM samples skipped, TVOC still counted */
    for (i = 0; i < 30; i++)
        push(999, AQM_ST_PMS_FAULT);
    CHECK(aqm_avg_get(&A, AQM_W_1M, AQM_M_PM25, &m) == 30 && NEAR(m, 30));
    CHECK(aqm_avg_get(&A, AQM_W_1M, AQM_M_TVOC, &m) == 60 && NEAR(m, 100));

    /* run well past the 1200-slot ring: 20 min window holds exactly 1200 */
    for (i = 0; i < 3000; i++)
        push(50, 0);
    CHECK(aqm_avg_get(&A, AQM_W_20M, AQM_M_PM25, &m) == 1200 && NEAR(m, 50));
    CHECK(aqm_avg_get(&A, AQM_W_10M, AQM_M_PM25, &m) == 600 && NEAR(m, 50));

    /* 24 h: ~52 minutes so far, below the 16 h CPCB minimum */
    CHECK(aqm_avg_aqi_24h(&A, &mins) == -1 && mins > 50 && mins < 60);

    /* fill a full day + a bit at PM2.5 60 / PM10 120 -> 24 h means exact */
    aqm_avg_init(&A);
    for (i = 0; i < (24 * 60 + 10) * 60; i++)
        push(60, 0);
    CHECK(aqm_avg_get(&A, AQM_W_24H, AQM_M_PM25, &m) == AQM_AVG_DAY && NEAR(m, 60));
    CHECK(aqm_avg_get(&A, AQM_W_24H, AQM_M_PM10, &m) == AQM_AVG_DAY && NEAR(m, 120));
    /* CPCB: PM2.5 60 -> 100 (top of Satisfactory); PM10 120 -> 113 */
    CHECK(aqm_avg_aqi_24h(&A, &mins) == 113 && mins == AQM_AVG_DAY);

    /* a minute that is mostly faulty does not count for the 24 h mean */
    aqm_avg_init(&A);
    for (i = 0; i < 60; i++)
        push(40, i < 40 ? AQM_ST_PMS_FAULT : 0);    /* only 20/60 valid */
    CHECK(aqm_avg_get(&A, AQM_W_24H, AQM_M_PM25, &m) == 0);
    CHECK(aqm_avg_get(&A, AQM_W_24H, AQM_M_TEMP, &m) == 1 && NEAR(m, 25));

    printf("avg ok\n");
    return 0;
}
