/* JSON payloads: exact sample text, null on faults, escaping, overflow */
#include "aqm/json.h"

#include <stdio.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
    return 1; } } while (0)

static aqm_avg_t A;

int main(void)
{
    char buf[1024];
    aqm_sample_t s;
    aqm_alarm_t al;
    int i, n;

    memset(&s, 0, sizeof(s));
    s.seq = 7; s.t_wall_s = 1794682384; s.pm1 = 39; s.pm25_cal = 67; s.pm10_cal = 76;
    s.tvoc_ppb = 150; s.temp_c = 27.9f; s.rh_pct = 55; s.aqi = 123; s.aqi_cat = 2;

    n = aqm_json_sample(buf, sizeof(buf), &s);
    CHECK(n > 0 && (size_t)n == strlen(buf));
    CHECK(!strcmp(buf, "{\"seq\":7,\"ts\":1794682384,\"pm1\":39,\"pm25\":67.0,\"pm10\":76.0,"
                       "\"tvoc\":150,\"eco2\":null,\"temp\":27.9,\"rh\":55.0,\"aqi\":123,"
                       "\"cat\":\"Moderate\",\"status\":0}"));

    /* PM sensor fault: PM fields and AQI are null, never 0 */
    s.status = AQM_ST_PMS_FAULT;
    aqm_json_sample(buf, sizeof(buf), &s);
    CHECK(strstr(buf, "\"pm25\":null") && strstr(buf, "\"aqi\":null") &&
          strstr(buf, "\"cat\":null") && strstr(buf, "\"tvoc\":150"));

    /* too small a buffer is an error, not a truncated message */
    CHECK(aqm_json_sample(buf, 20, &s) == -1);

    /* averages */
    aqm_avg_init(&A);
    s.status = 0;
    for (i = 0; i < 120; i++)
        aqm_avg_push(&A, &s);
    n = aqm_json_avg(buf, sizeof(buf), &A, 1794682384LL);
    CHECK(n > 0);
    CHECK(strstr(buf, "\"pm25\":{\"1m\":67.0,\"5m\":67.0,\"10m\":67.0,\"20m\":67.0,\"24h\":67.0}"));
    CHECK(strstr(buf, "\"aqi24h\":null,\"minutes24h\":2}"));

    /* alarm text is escaped */
    memset(&al, 0, sizeof(al));
    al.seq = 10; al.type = AQM_ALM_FAULT; al.prio = AQM_PRIO_CRIT; al.value = 1;
    al.t_wall_s = 1794682384;
    snprintf(al.text, sizeof(al.text), "sensor \"PMS\" fault\\x");
    aqm_json_alarm(buf, sizeof(buf), &al);
    CHECK(!strcmp(buf, "{\"ts\":1794682384,\"seq\":10,\"type\":\"fault\",\"prio\":10,\"value\":1,"
                       "\"text\":\"sensor \\\"PMS\\\" fault\\\\x\"}"));

    printf("json ok\n");
    return 0;
}
