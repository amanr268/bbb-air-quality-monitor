#ifndef AQM_SCREEN_H
#define AQM_SCREEN_H

#include "aqm/ssd1306.h"
#include "aqm/types.h"

/* Draw the air-quality screen for one (converted + compared) sample.
 * The layout adapts to the display rotation (landscape / portrait).
 * Clears the framebuffer first; call ssd1306_flush() afterwards. */
void aqm_screen_draw(ssd1306_t *d, const aqm_sample_t *s);

#endif
