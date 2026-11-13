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

#endif
