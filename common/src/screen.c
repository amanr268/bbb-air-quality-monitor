#include "aqm/screen.h"

#include "aqm/aqi.h"

#include <stdio.h>
#include <string.h>
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
    /* 12-hour clock: "4:05 PM" (drop the leading zero of %I) */
    strftime(buf, n, "%I:%M %p", &tm);
    if (buf[0] == '0')
        memmove(buf, buf + 1, strlen(buf));
}

static void centre(ssd1306_t *d, int y, const char *s, const aqm_font_t *f)
{
    ssd1306_text(d, (ssd1306_width(d) - ssd1306_text_width(s, f)) / 2, y, s, f);
}

/* alert wording for the header (21 characters landscape, 10 portrait) */
static void banner_text(const aqm_banner_t *b, int portrait, char *buf, size_t n)
{
    switch (b->kind) {
    case AQM_BANNER_AQI:
        if (portrait)
            snprintf(buf, n, "!%s", short_cat(b->aqi_cat));
        else
            snprintf(buf, n, "! %s", aqm_aqi_cat_name((aqm_aqi_cat_t)b->aqi_cat));
        break;
    case AQM_BANNER_FAULT:
        snprintf(buf, n, portrait ? "! FAULT" : "! SENSOR FAULT");
        break;
    case AQM_BANNER_TVOC:
        snprintf(buf, n, portrait ? "! TVOC HI" : "! TVOC HIGH");
        break;
    default:
        snprintf(buf, n, "! ALARM");
        break;
    }
}

int aqm_screen_ticker_width(const char *ticker)
{
    return ssd1306_text_width(ticker, &font_6_10);
}

/* text centred in [x0, x0 + w); on = 0 draws dark text on a lit area */
static void centre_in(ssd1306_t *d, int x0, int w, int y, const char *s,
                      const aqm_font_t *f, int on)
{
    ssd1306_text_color(d, x0 + (w - ssd1306_text_width(s, f)) / 2, y, s, f, on);
}

/* one grid cell: label on top, value below, both centred */
static void tile(ssd1306_t *d, int x, int y, int w, const char *label, const char *val,
                 const aqm_font_t *f)
{
    centre_in(d, x, w, y, label, f, 1);
    centre_in(d, x, w, y + 11, val, f, 1);
}

/*
 * Main page: AQI in an inverted (lit) header, then a grid of tiles separated
 * by lines - landscape 3 x 2, portrait 2 x 3. An active alert replaces the
 * category text in the header. A faulty sensor shows "--", never 0.
 */
void aqm_screen_draw(ssd1306_t *d, const aqm_sample_t *s, const aqm_banner_t *banner)
{
    const aqm_font_t *sm = &font_6_10, *big = &font_10_20;
    int w = ssd1306_width(d), portrait = w < ssd1306_height(d);
    char t[12], aqi[8], cat[24], pm25[8], pm10[8], tv[8], co2[8], tc[8], rh[8];
    const char *tick = (banner && banner->ticker && banner->ticker[0]) ? banner->ticker : NULL;
    /* a missing sensor must never look like clean air (or -50 C) */
    int pm_ok = !(s->status & AQM_ST_PMS_FAULT);
    int ens_ok = !(s->status & AQM_ST_ENS_FAULT);
    int aht_ok = !(s->status & AQM_ST_AHT_FAULT);

    ssd1306_clear(d);
    hhmm(s->t_wall_s, t, sizeof(t));
    if (pm_ok) {
        snprintf(aqi, sizeof(aqi), "%u", s->aqi);
        snprintf(pm25, sizeof(pm25), "%.0f", s->pm25_cal);
        snprintf(pm10, sizeof(pm10), "%.0f", s->pm10_cal);
        snprintf(cat, sizeof(cat), "%s", portrait ? short_cat(s->aqi_cat)
                 : aqm_aqi_cat_name((aqm_aqi_cat_t)s->aqi_cat));
    } else {
        snprintf(aqi, sizeof(aqi), "--");
        snprintf(pm25, sizeof(pm25), "--");
        snprintf(pm10, sizeof(pm10), "--");
        snprintf(cat, sizeof(cat), "%s", portrait ? "No PM" : "No PM data");
    }
    if (banner && banner->kind != AQM_BANNER_NONE)
        banner_text(banner, portrait, cat, sizeof(cat));
    if (ens_ok) snprintf(tv, sizeof(tv), "%u", s->tvoc_ppb);
    else        snprintf(tv, sizeof(tv), "--");
    if (ens_ok && s->eco2_ppm) snprintf(co2, sizeof(co2), "%u", (unsigned)s->eco2_ppm);
    else                       snprintf(co2, sizeof(co2), "--");
    if (aht_ok) {
        /* portrait tiles are 5 characters wide: no unit */
        snprintf(tc, sizeof(tc), portrait ? "%.1f" : "%.1fC", s->temp_c);
        snprintf(rh, sizeof(rh), "%.0f%%", s->rh_pct);
    } else {
        snprintf(tc, sizeof(tc), "--");
        snprintf(rh, sizeof(rh), "--");
    }

    if (!portrait && tick) {
        /* landscape with a sensor fault: the top line of the header becomes
         * the scrolling fault message; AQI, category and clock share line 2 */
        char line[40];

        ssd1306_rect(d, 0, 0, w, 21, 1);
        ssd1306_text_color(d, banner->ticker_x, 1, tick, sm, 0);
        snprintf(line, sizeof(line), "AQI %s %s", aqi, cat);
        ssd1306_text_color(d, 2, 11, line, sm, 0);
        if (2 + ssd1306_text_width(line, sm) + sm->w + ssd1306_text_width(t, sm) < w)
            ssd1306_text_color(d, w - 1 - ssd1306_text_width(t, sm), 11, t, sm, 0);   /* clock if it fits */
    }
    if (!portrait) {
        /* landscape 128 x 64: header 21 px, two rows of 3 tiles */
        int x = 2 + ssd1306_text_width(aqi, big) + 5;

        if (!tick) {
            ssd1306_rect(d, 0, 0, w, 21, 1);
            ssd1306_text_color(d, 2, 1, aqi, big, 0);
            ssd1306_text_color(d, x, 1, "AQI", sm, 0);
            ssd1306_text_color(d, w - 1 - ssd1306_text_width(t, sm), 1, t, sm, 0);
            ssd1306_text_color(d, x, 11, cat, sm, 0);
        }
        ssd1306_line(d, 42, 22, 42, 63);
        ssd1306_line(d, 85, 22, 85, 63);
        ssd1306_line(d, 0, 43, w - 1, 43);
        tile(d, 0, 22, 42, "PM2.5", pm25, sm);
        tile(d, 43, 22, 42, "PM10", pm10, sm);
        tile(d, 86, 22, 42, "TVOC", tv, sm);
        tile(d, 0, 44, 42, "eCO2", co2, sm);
        tile(d, 43, 44, 42, "Temp", tc, sm);
        tile(d, 86, 44, 42, "RH", rh, sm);
    } else {
        /* portrait 64 x 128: header 46 px, three rows of 2 tiles, clock */
        ssd1306_rect(d, 0, 0, w, 46, 1);
        if (tick)       /* sensor fault: scrolling message instead of the "AQI" label */
            ssd1306_text_color(d, banner->ticker_x, 1, tick, sm, 0);
        else
            centre_in(d, 0, w, 1, "AQI", sm, 0);
        centre_in(d, 0, w, 12, aqi, big, 0);
        centre_in(d, 0, w, 34, cat, sm, 0);
        ssd1306_line(d, 31, 47, 31, 115);
        ssd1306_line(d, 0, 70, w - 1, 70);
        ssd1306_line(d, 0, 93, w - 1, 93);
        ssd1306_line(d, 0, 116, w - 1, 116);
        tile(d, 0, 48, 31, "PM2.5", pm25, sm);
        tile(d, 32, 48, 32, "PM10", pm10, sm);
        tile(d, 0, 71, 31, "TVOC", tv, sm);
        tile(d, 32, 71, 32, "eCO2", co2, sm);
        tile(d, 0, 94, 31, "Temp", tc, sm);
        tile(d, 32, 94, 32, "RH", rh, sm);
        centre_in(d, 0, w, 118, t, sm, 1);
    }
}

/* one "label ... value" row; if both do not fit side by side (portrait IP),
 * the value goes right-aligned on the next row. Returns the next y. */
static int row(ssd1306_t *d, int y, const char *label, const char *value, const aqm_font_t *f)
{
    int w = ssd1306_width(d), vw = ssd1306_text_width(value, f);

    ssd1306_text(d, 0, y, label, f);
    if (ssd1306_text_width(label, f) + f->w + vw > w)
        y += f->h;
    if (vw > w) {
        /* still too wide (an IP in portrait): break after the last '.'
         * that fits, e.g. "192.168." / "1.14" */
        char head[24];
        int max = w / f->w, cut = 0, i;

        for (i = 0; value[i] && i < max; i++)
            if (value[i] == '.')
                cut = i + 1;
        if (cut == 0)
            cut = max;
        snprintf(head, sizeof(head), "%.*s", cut, value);
        ssd1306_text(d, 0, y, head, f);
        y += f->h;
        value += cut;
        vw = ssd1306_text_width(value, f);
    }
    ssd1306_text(d, w - vw, y, value, f);
    return y + f->h;
}

static void fmt_uptime(long s, char *buf, size_t n)
{
    if (s >= 86400)
        snprintf(buf, n, "%ldd%02ldh", s / 86400, (s % 86400) / 3600);
    else
        snprintf(buf, n, "%ldh%02ldm", s / 3600, (s % 3600) / 60);
}

/* "label  pm25  pm10" averages row: values right-aligned in two columns */
static int avg_row(ssd1306_t *d, int y, const aqm_avg_t *avg, int win, const aqm_font_t *f)
{
    int w = ssd1306_width(d);
    int col1 = w - (w < ssd1306_height(d) ? 4 : 7) * f->w;  /* right edge of PM2.5 column */
    char a[8] = "--", b[8] = "--";
    float m;

    if (avg && aqm_avg_get(avg, win, AQM_M_PM25, &m) > 0) snprintf(a, sizeof(a), "%.0f", m);
    if (avg && aqm_avg_get(avg, win, AQM_M_PM10, &m) > 0) snprintf(b, sizeof(b), "%.0f", m);
    ssd1306_text(d, 0, y, aqm_avg_window_name(win), f);
    ssd1306_text(d, col1 - ssd1306_text_width(a, f), y, a, f);
    ssd1306_text(d, w - ssd1306_text_width(b, f), y, b, f);
    return y + f->h;
}

void aqm_screen_draw_page(ssd1306_t *d, const aqm_sample_t *s, int page,
                          const aqm_sysinfo_t *si, const aqm_avg_t *avg)
{
    static const char *title[AQM_PAGE_COUNT] = { "Air", "Particles", "Averages", "Climate", "System" };
    const aqm_font_t *f = &font_6_10;
    int w = ssd1306_width(d), portrait = w < ssd1306_height(d);
    int pm_ok = !(s->status & AQM_ST_PMS_FAULT), y;
    char a[24], b[24], n[8];

    if (page <= AQM_PAGE_MAIN || page >= AQM_PAGE_COUNT)
        return;
    ssd1306_clear(d);

    /* header: title + page number, then a rule */
    snprintf(n, sizeof(n), "%d/%d", page + 1, AQM_PAGE_COUNT);
    if (portrait) {
        centre(d, 0, title[page], f);
        centre(d, f->h, n, f);
        y = 2 * f->h + 1;
    } else {
        ssd1306_text(d, 0, 0, title[page], f);
        ssd1306_text(d, w - ssd1306_text_width(n, f), 0, n, f);
        y = f->h + 1;
    }
    ssd1306_line(d, 0, y, w - 1, y);
    y += 3;

    switch (page) {
    case AQM_PAGE_PARTICLES:
        if (pm_ok) snprintf(a, sizeof(a), "%u", s->pm1);
        else       snprintf(a, sizeof(a), "--");
        y = row(d, y, "PM1.0", a, f);
        if (pm_ok) snprintf(a, sizeof(a), "%.0f", s->pm25_cal);
        y = row(d, y, "PM2.5", a, f);
        if (pm_ok) snprintf(a, sizeof(a), "%.0f", s->pm10_cal);
        y = row(d, y, "PM10", a, f);
        if (pm_ok) snprintf(a, sizeof(a), "%u", s->aqi);
        y = row(d, y, "AQI", a, f);
        {
            /* CPCB AQI proper: 24 h means, needs >= 16 h of data */
            int mins = 0, aqi24 = avg ? aqm_avg_aqi_24h(avg, &mins) : -1;

            if (aqi24 >= 0) snprintf(a, sizeof(a), "%d", aqi24);
            else            snprintf(a, sizeof(a), "%d/16h", mins / 60);  /* hours collected */
            y = row(d, y, "AQI24h", a, f);
        }
        if (portrait)
            centre(d, y + 4, "ug/m3", f);
        break;
    case AQM_PAGE_AVERAGES:
        /* column heads, then one row per window (10 min is logged, not shown) */
        {
            int col1 = w - (portrait ? 4 : 7) * f->w;
            const char *h1 = portrait ? "2.5" : "PM2.5", *h2 = portrait ? "10" : "PM10";

            ssd1306_text(d, col1 - ssd1306_text_width(h1, f), y, h1, f);
            ssd1306_text(d, w - ssd1306_text_width(h2, f), y, h2, f);
            y += f->h;
        }
        y = avg_row(d, y, avg, AQM_W_1M, f);
        y = avg_row(d, y, avg, AQM_W_5M, f);
        if (portrait)                       /* room for all five windows */
            y = avg_row(d, y, avg, AQM_W_10M, f);
        y = avg_row(d, y, avg, AQM_W_20M, f);
        (void)avg_row(d, y, avg, AQM_W_24H, f);
        break;
    case AQM_PAGE_CLIMATE:
        {
            int ens = !(s->status & AQM_ST_ENS_FAULT), aht = !(s->status & AQM_ST_AHT_FAULT);

            if (ens) snprintf(a, sizeof(a), "%u", s->tvoc_ppb);
            else     snprintf(a, sizeof(a), "--");
            y = row(d, y, "TVOC", a, f);
            if (ens && s->eco2_ppm) snprintf(a, sizeof(a), "%u", s->eco2_ppm);
            else                    snprintf(a, sizeof(a), "--");
            y = row(d, y, "eCO2", a, f);
            if (aht) snprintf(a, sizeof(a), "%.1fC", s->temp_c);
            else     snprintf(a, sizeof(a), "--");
            y = row(d, y, "Temp", a, f);
            if (aht) snprintf(a, sizeof(a), "%.0f%%", s->rh_pct);
            else     snprintf(a, sizeof(a), "--");
            y = row(d, y, "RH", a, f);
        }
        break;
    case AQM_PAGE_SYSTEM:
        y = row(d, y, "IP", (si && si->ip[0]) ? si->ip : "none", f);
        fmt_uptime(si ? si->uptime_s : 0, b, sizeof(b));
        y = row(d, y, "Up", b, f);
        snprintf(a, sizeof(a), "%u", s->seq);
        y = row(d, y, "Seq", a, f);
        break;
    }
}
