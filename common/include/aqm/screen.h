#ifndef AQM_SCREEN_H
#define AQM_SCREEN_H

#include "aqm/avg.h"
#include "aqm/ssd1306.h"
#include "aqm/types.h"

/* optional alert shown inverted in the top line; the wording is chosen to
 * fit the layout (21 characters landscape, 10 portrait) */
enum { AQM_BANNER_NONE = 0, AQM_BANNER_AQI, AQM_BANNER_FAULT, AQM_BANNER_TVOC };

typedef struct {
    int kind;       /* AQM_BANNER_*                    */
    int aqi_cat;    /* category for AQM_BANNER_AQI     */
    /* sensor-fault ticker: scrolls across the top line of the header for as
     * long as a sensor is not working (NULL / "" = none). ticker_x is the x
     * of its first character; the caller decreases it to scroll. */
    const char *ticker;
    int ticker_x;
} aqm_banner_t;

/* width in pixels of the ticker text (the caller wraps ticker_x with it) */
int aqm_screen_ticker_width(const char *ticker);

/* Draw the air-quality screen for one (converted + compared) sample.
 * The layout adapts to the display rotation (landscape / portrait).
 * banner: NULL or kind NONE = no alert line.
 * Clears the framebuffer first; call ssd1306_flush() afterwards. */
void aqm_screen_draw(ssd1306_t *d, const aqm_sample_t *s, const aqm_banner_t *banner);

/* extra pages, cycled with the button; page MAIN is aqm_screen_draw() */
/* FAULTS exists only while something is wrong: then there are 6 pages,
 * otherwise 5 and the button skips it */
enum { AQM_PAGE_MAIN = 0, AQM_PAGE_FAULTS, AQM_PAGE_PARTICLES, AQM_PAGE_AVERAGES,
       AQM_PAGE_CLIMATE, AQM_PAGE_SYSTEM, AQM_PAGE_COUNT };

typedef struct {
    char ip[16];        /* "" = no network */
    long uptime_s;
} aqm_sysinfo_t;

/* Draw info page `page` (PARTICLES..SYSTEM) in either orientation.
 * si: SYSTEM page; avg: PARTICLES (24 h AQI) and AVERAGES pages (may be NULL).
 * Call ssd1306_flush() afterwards. */
void aqm_screen_draw_page(ssd1306_t *d, const aqm_sample_t *s, int page,
                          const aqm_sysinfo_t *si, const aqm_avg_t *avg, int have_faults);

/* Fault page (page 2 while something is wrong): fault k of n, its title in an
 * inverted bar and the explanation (what to check, which pin) word-wrapped
 * below. A text too long for the screen is split into parts; `part` picks
 * one (the caller cycles it). Returns the number of parts. */
int aqm_screen_draw_fault(ssd1306_t *d, int k, int n, const char *title,
                          const char *body, int part);

#endif
