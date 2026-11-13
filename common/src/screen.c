#include "aqm/screen.h"

#include "aqm/aqi.h"

#include <stdio.h>
#include <time.h>

/* portrait is 10 characters wide: short category names */
static const char *short_cat(int c)
{
    static const char *n[] = { "Good", "Satisf.", "Moderate", "Poor", "V.Poor", "Severe" };

    return (c >= 0 && c <= 5) ? n[c] : "?";
}

static void hhmm(int64_t t, char *buf, size_t n)
{
    time_t tt = (time_t)t;
    struct tm tm;

    localtime_r(&tt, &tm);
    strftime(buf, n, "%H:%M", &tm);
}

static void centre(ssd1306_t *d, int y, const char *s, const aqm_font_t *f)
{
    ssd1306_text(d, (ssd1306_width(d) - ssd1306_text_width(s, f)) / 2, y, s, f);
}

void aqm_screen_draw(ssd1306_t *d, const aqm_sample_t *s)
{
    const aqm_font_t *sm = &font_6_10, *big = &font_10_20;
    int w = ssd1306_width(d);
    char t[8], aqi[8], buf[32];

    ssd1306_clear(d);
    hhmm(s->t_wall_s, t, sizeof(t));
    snprintf(aqi, sizeof(aqi), "%u", s->aqi);

    if (w > ssd1306_height(d)) {
        /* landscape 128 x 64 */
        ssd1306_text(d, 0, 0, "AQI", sm);
        ssd1306_text(d, w - ssd1306_text_width(t, sm), 0, t, sm);
        ssd1306_text(d, 0, 11, aqi, big);
        ssd1306_text(d, ssd1306_text_width(aqi, big) + 6, 17,
                     aqm_aqi_cat_name((aqm_aqi_cat_t)s->aqi_cat), sm);
        ssd1306_line(d, 0, 33, w - 1, 33);
        snprintf(buf, sizeof(buf), "PM2.5 %3.0f PM10 %3.0f", s->pm25_cal, s->pm10_cal);
        ssd1306_text(d, 0, 37, buf, sm);
        snprintf(buf, sizeof(buf), "TVOC %3u  %4.1fC %2.0f%%", s->tvoc_ppb, s->temp_c, s->rh_pct);
        ssd1306_text(d, 0, 51, buf, sm);
    } else {
        /* portrait 64 x 128 */
        centre(d, 0, "AQI", sm);
        centre(d, 12, aqi, big);
        centre(d, 34, short_cat(s->aqi_cat), sm);
        ssd1306_line(d, 0, 46, w - 1, 46);
        snprintf(buf, sizeof(buf), "PM2.5 %3.0f", s->pm25_cal); ssd1306_text(d, 0, 50, buf, sm);
        snprintf(buf, sizeof(buf), "PM10  %3.0f", s->pm10_cal); ssd1306_text(d, 0, 61, buf, sm);
        snprintf(buf, sizeof(buf), "TVOC  %3u", s->tvoc_ppb);   ssd1306_text(d, 0, 72, buf, sm);
        snprintf(buf, sizeof(buf), "T  %4.1fC", s->temp_c);     ssd1306_text(d, 0, 83, buf, sm);
        snprintf(buf, sizeof(buf), "RH    %2.0f%%", s->rh_pct); ssd1306_text(d, 0, 94, buf, sm);
        ssd1306_line(d, 0, 107, w - 1, 107);
        centre(d, 112, t, sm);
    }
}
