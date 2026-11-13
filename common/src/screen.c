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

/* inverted bar across the top line with the alert text */
static void draw_banner(ssd1306_t *d, const aqm_banner_t *b, const aqm_font_t *f)
{
    int w = ssd1306_width(d), portrait = w < ssd1306_height(d);
    char buf[24];

    switch (b->kind) {
    case AQM_BANNER_AQI:
        if (portrait)
            snprintf(buf, sizeof(buf), "!%s", short_cat(b->aqi_cat));
        else
            snprintf(buf, sizeof(buf), "! AQI %s", aqm_aqi_cat_name((aqm_aqi_cat_t)b->aqi_cat));
        break;
    case AQM_BANNER_FAULT:
        snprintf(buf, sizeof(buf), portrait ? "! FAULT" : "! SENSOR FAULT");
        break;
    case AQM_BANNER_TVOC:
        snprintf(buf, sizeof(buf), portrait ? "! TVOC HI" : "! TVOC HIGH");
        break;
    default:
        snprintf(buf, sizeof(buf), "! ALARM");
        break;
    }
    ssd1306_rect(d, 0, 0, w, f->h, 1);
    ssd1306_text_color(d, (w - ssd1306_text_width(buf, f)) / 2, 0, buf, f, 0);
}

void aqm_screen_draw(ssd1306_t *d, const aqm_sample_t *s, const aqm_banner_t *banner)
{
    const aqm_font_t *sm = &font_6_10, *big = &font_10_20;
    int w = ssd1306_width(d);
    char t[8], aqi[8], buf[32], pm25[8], pm10[8];
    /* a missing PM sensor must never look like clean air */
    int pm_ok = !(s->status & AQM_ST_PMS_FAULT);

    if (banner && banner->kind == AQM_BANNER_NONE)
        banner = NULL;
    ssd1306_clear(d);
    hhmm(s->t_wall_s, t, sizeof(t));
    if (pm_ok) {
        snprintf(aqi, sizeof(aqi), "%u", s->aqi);
        snprintf(pm25, sizeof(pm25), "%3.0f", s->pm25_cal);
        snprintf(pm10, sizeof(pm10), "%3.0f", s->pm10_cal);
    } else {
        snprintf(aqi, sizeof(aqi), "--");
        snprintf(pm25, sizeof(pm25), " --");
        snprintf(pm10, sizeof(pm10), " --");
    }

    if (w > ssd1306_height(d)) {
        /* landscape 128 x 64 */
        if (banner) {
            draw_banner(d, banner, sm);
        } else {
            ssd1306_text(d, 0, 0, "AQI", sm);
            ssd1306_text(d, w - ssd1306_text_width(t, sm), 0, t, sm);
        }
        ssd1306_text(d, 0, 11, aqi, big);
        ssd1306_text(d, ssd1306_text_width(aqi, big) + 6, 17,
                     pm_ok ? aqm_aqi_cat_name((aqm_aqi_cat_t)s->aqi_cat) : "No PM data", sm);
        ssd1306_line(d, 0, 33, w - 1, 33);
        snprintf(buf, sizeof(buf), "PM2.5 %s PM10 %s", pm25, pm10);
        ssd1306_text(d, 0, 37, buf, sm);
        snprintf(buf, sizeof(buf), "TVOC %3u  %4.1fC %2.0f%%", s->tvoc_ppb, s->temp_c, s->rh_pct);
        ssd1306_text(d, 0, 51, buf, sm);
    } else {
        /* portrait 64 x 128 */
        if (banner)
            draw_banner(d, banner, sm);
        else
            centre(d, 0, "AQI", sm);
        centre(d, 12, aqi, big);
        centre(d, 34, pm_ok ? short_cat(s->aqi_cat) : "No PM", sm);
        ssd1306_line(d, 0, 46, w - 1, 46);
        snprintf(buf, sizeof(buf), "PM2.5 %s", pm25);  ssd1306_text(d, 0, 50, buf, sm);
        snprintf(buf, sizeof(buf), "PM10  %s", pm10);  ssd1306_text(d, 0, 61, buf, sm);
        snprintf(buf, sizeof(buf), "TVOC  %3u", s->tvoc_ppb);   ssd1306_text(d, 0, 72, buf, sm);
        snprintf(buf, sizeof(buf), "T  %4.1fC", s->temp_c);     ssd1306_text(d, 0, 83, buf, sm);
        snprintf(buf, sizeof(buf), "RH    %2.0f%%", s->rh_pct); ssd1306_text(d, 0, 94, buf, sm);
        ssd1306_line(d, 0, 107, w - 1, 107);
        centre(d, 112, t, sm);
    }
}
