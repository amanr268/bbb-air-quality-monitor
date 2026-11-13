#ifndef AQM_SSD1306_H
#define AQM_SSD1306_H

#include <stdint.h>

/* Minimal SSD1306 128x64 I2C driver (Linux i2c-dev). */

#define SSD1306_W     128
#define SSD1306_H     64
#define SSD1306_FB    (SSD1306_W * SSD1306_H / 8)   /* 1024 bytes */

#include "aqm/font.h"

/* Software rotation of everything drawn (used for portrait/landscape):
 * 0 = landscape 128x64, 1 = portrait 64x128 (90 deg CW),
 * 2 = landscape upside down, 3 = portrait (270 deg) */
typedef struct {
    int     fd;
    int     rot;
    uint8_t fb[SSD1306_FB];      /* page layout: byte = 8 vertical pixels, bit0 = top */
    uint8_t shadow[SSD1306_FB];  /* what the panel currently shows             */
    int     shadow_valid;        /* 0 = force a full refresh on next flush     */
} ssd1306_t;

int  ssd1306_open(ssd1306_t *d, const char *i2c_dev, uint8_t addr);  /* 0 or -errno */
void ssd1306_close(ssd1306_t *d);        /* blanks the panel and turns it off */

int  ssd1306_cmd(ssd1306_t *d, const uint8_t *cmds, int n);
int  ssd1306_flush(ssd1306_t *d);        /* send changed pages, one I2C write per page */
int  ssd1306_contrast(ssd1306_t *d, uint8_t level);
int  ssd1306_flip180(ssd1306_t *d, int flipped);   /* hardware 180 deg rotation */

/* drawing into the framebuffer (call ssd1306_flush to show it);
 * coordinates are logical, i.e. after rotation */
void ssd1306_set_rotation(ssd1306_t *d, int rot);
int  ssd1306_width(const ssd1306_t *d);
int  ssd1306_height(const ssd1306_t *d);
void ssd1306_clear(ssd1306_t *d);
void ssd1306_pixel(ssd1306_t *d, int x, int y, int on);
void ssd1306_line(ssd1306_t *d, int x0, int y0, int x1, int y1);
void ssd1306_rect(ssd1306_t *d, int x, int y, int w, int h, int fill);

/* text: returns x after the last character */
int  ssd1306_text(ssd1306_t *d, int x, int y, const char *s, const aqm_font_t *f);
/* on = 0 clears the glyph pixels (dark text on a filled rectangle) */
int  ssd1306_text_color(ssd1306_t *d, int x, int y, const char *s,
                        const aqm_font_t *f, int on);
int  ssd1306_text_width(const char *s, const aqm_font_t *f);

#endif
