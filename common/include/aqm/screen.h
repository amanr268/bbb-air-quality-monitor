#ifndef AQM_SCREEN_H
#define AQM_SCREEN_H

#include "aqm/ssd1306.h"
#include "aqm/types.h"

/* optional alert shown inverted in the top line; the wording is chosen to
 * fit the layout (21 characters landscape, 10 portrait) */
enum { AQM_BANNER_NONE = 0, AQM_BANNER_AQI, AQM_BANNER_FAULT, AQM_BANNER_TVOC };

typedef struct {
    int kind;       /* AQM_BANNER_*                    */
    int aqi_cat;    /* category for AQM_BANNER_AQI     */
} aqm_banner_t;

/* Draw the air-quality screen for one (converted + compared) sample.
 * The layout adapts to the display rotation (landscape / portrait).
 * banner: NULL or kind NONE = no alert line.
 * Clears the framebuffer first; call ssd1306_flush() afterwards. */
void aqm_screen_draw(ssd1306_t *d, const aqm_sample_t *s, const aqm_banner_t *banner);

/* extra pages, cycled with the button; page MAIN is aqm_screen_draw() */
enum { AQM_PAGE_MAIN = 0, AQM_PAGE_PARTICLES, AQM_PAGE_CLIMATE, AQM_PAGE_SYSTEM,
       AQM_PAGE_COUNT };

typedef struct {
    char ip[16];        /* "" = no network */
    long uptime_s;
} aqm_sysinfo_t;

/* Draw info page `page` (PARTICLES..SYSTEM) in either orientation.
 * si is only used by the SYSTEM page. Call ssd1306_flush() afterwards. */
void aqm_screen_draw_page(ssd1306_t *d, const aqm_sample_t *s, int page,
                          const aqm_sysinfo_t *si);

#endif
