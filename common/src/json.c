#include "aqm/json.h"

#include "aqm/aqi.h"

#include <stdarg.h>
#include <stdio.h>

/* append to buf; on overflow mark the whole result invalid */
typedef struct { char *buf; size_t n, len; int err; } jbuf_t;

static void put(jbuf_t *j, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void put(jbuf_t *j, const char *fmt, ...)
{
    va_list ap;
    int r;

    if (j->err)
        return;
    va_start(ap, fmt);
    r = vsnprintf(j->buf + j->len, j->n - j->len, fmt, ap);
    va_end(ap);
    if (r < 0 || (size_t)r >= j->n - j->len)
        j->err = 1;
    else
        j->len += (size_t)r;
}

static int done(jbuf_t *j)
{
    return j->err ? -1 : (int)j->len;
}

static void num_or_null(jbuf_t *j, const char *key, int ok, const char *fmt, double v)
{
    put(j, "\"%s\":", key);
    if (ok)
        put(j, fmt, v);
    else
        put(j, "null");
}

int aqm_json_sample(char *buf, size_t n, const aqm_sample_t *s)
{
    jbuf_t j = { buf, n, 0, 0 };
    int pm = !(s->status & AQM_ST_PMS_FAULT);
    int ens = !(s->status & AQM_ST_ENS_FAULT);
    int aht = !(s->status & AQM_ST_AHT_FAULT);

    put(&j, "{\"seq\":%u,\"ts\":%lld,", s->seq, (long long)s->t_wall_s);
    num_or_null(&j, "pm1", pm, "%.0f", s->pm1);           put(&j, ",");
    num_or_null(&j, "pm25", pm, "%.1f", s->pm25_cal);     put(&j, ",");
    num_or_null(&j, "pm10", pm, "%.1f", s->pm10_cal);     put(&j, ",");
    num_or_null(&j, "tvoc", ens, "%.0f", s->tvoc_ppb);    put(&j, ",");
    num_or_null(&j, "eco2", ens && s->eco2_ppm, "%.0f", s->eco2_ppm); put(&j, ",");
    num_or_null(&j, "temp", aht, "%.1f", s->temp_c);      put(&j, ",");
    num_or_null(&j, "rh", aht, "%.1f", s->rh_pct);        put(&j, ",");
    num_or_null(&j, "aqi", pm, "%.0f", s->aqi);           put(&j, ",");
    if (pm)
        put(&j, "\"cat\":\"%s\",", aqm_aqi_cat_name((aqm_aqi_cat_t)s->aqi_cat));
    else
        put(&j, "\"cat\":null,");
    put(&j, "\"status\":%u}", s->status);
    return done(&j);
}

int aqm_json_avg(char *buf, size_t n, const aqm_avg_t *a, long long ts)
{
    static const int metric[] = { AQM_M_PM25, AQM_M_PM10, AQM_M_TVOC, AQM_M_TEMP, AQM_M_RH };
    static const char *name[] = { "pm25", "pm10", "tvoc", "temp", "rh" };
    jbuf_t j = { buf, n, 0, 0 };
    int mins, aqi24 = aqm_avg_aqi_24h(a, &mins);
    size_t i;
    int w;

    put(&j, "{\"ts\":%lld", ts);
    for (i = 0; i < sizeof(metric) / sizeof(metric[0]); i++) {
        put(&j, ",\"%s\":{", name[i]);
        for (w = 0; w < AQM_W_COUNT; w++) {
            float m = 0;
            int ok = aqm_avg_get(a, w, metric[i], &m) > 0;

            if (w)
                put(&j, ",");
            num_or_null(&j, aqm_avg_window_name(w), ok, "%.1f", m);
        }
        put(&j, "}");
    }
    put(&j, ",");
    num_or_null(&j, "aqi24h", aqi24 >= 0, "%.0f", aqi24);
    put(&j, ",\"minutes24h\":%d}", mins);
    return done(&j);
}

int aqm_json_alarm(char *buf, size_t n, const aqm_alarm_t *al)
{
    static const char *type[] = { "?", "aqi", "fault", "tvoc" };
    jbuf_t j = { buf, n, 0, 0 };
    const char *t = al->type <= AQM_ALM_TVOC ? type[al->type] : "?";
    const char *p;

    put(&j, "{\"ts\":%lld,\"seq\":%u,\"type\":\"%s\",\"prio\":%u,\"value\":%u,\"text\":\"",
        (long long)al->t_wall_s, al->seq, t, al->prio, al->value);
    /* alarm text is ours, but escape anyway so the JSON is always valid */
    for (p = al->text; *p && p < al->text + sizeof(al->text); p++) {
        if (*p == '"' || *p == '\\')
            put(&j, "\\%c", *p);
        else if ((unsigned char)*p < 0x20)
            put(&j, " ");
        else
            put(&j, "%c", *p);
    }
    put(&j, "\"}");
    return done(&j);
}
