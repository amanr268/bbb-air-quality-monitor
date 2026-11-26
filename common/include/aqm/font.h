#ifndef AQM_FONT_H
#define AQM_FONT_H

#include <stdint.h>

/* Fixed-width bitmap font: one row per entry, MSB = leftmost pixel. */
typedef struct {
    uint8_t     w, h;          /* glyph cell size in pixels       */
    uint8_t     first, last;   /* character range (ASCII)         */
    uint8_t     row_bytes;     /* 1 (w <= 8) or 2 (w <= 16)       */
    const void *bits;          /* [last-first+1][h] rows          */
} aqm_font_t;

extern const aqm_font_t font_6_10;    /* body text: 21 chars x 6 lines; "\x7f" = ohm sign */
extern const aqm_font_t font_10_20;   /* big numbers                    */

#endif
