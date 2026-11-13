#include "aqm/ssd1306.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* every I2C transfer starts with a control byte */
#define CTRL_CMD   0x00
#define CTRL_DATA  0x40

static const uint8_t INIT_SEQ[] = {
    0xAE,             /* display off                         */
    0xD5, 0x80,       /* clock divide / oscillator           */
    0xA8, 0x3F,       /* multiplex ratio: 64 rows            */
    0xD3, 0x00,       /* display offset 0                    */
    0x40,             /* start line 0                        */
    0x8D, 0x14,       /* charge pump ON (needed at 3.3 V)    */
    0x20, 0x00,       /* horizontal addressing mode          */
    0xA1,             /* segment remap (column 127 = SEG0)   */
    0xC8,             /* COM scan descending                 */
    0xDA, 0x12,       /* COM pins: alternative, no remap     */
    0x81, 0xCF,       /* contrast                            */
    0xD9, 0xF1,       /* pre-charge period                   */
    0xDB, 0x40,       /* VCOMH deselect level                */
    0xA4,             /* display follows RAM                 */
    0xA6,             /* normal (not inverted)               */
    0xAF              /* display on                          */
};

int ssd1306_cmd(ssd1306_t *d, const uint8_t *cmds, int n)
{
    uint8_t buf[64];

    if (n < 1 || n > (int)sizeof(buf) - 1)
        return -EINVAL;
    buf[0] = CTRL_CMD;
    memcpy(&buf[1], cmds, (size_t)n);
    return write(d->fd, buf, (size_t)n + 1) == n + 1 ? 0 : -errno;
}

int ssd1306_open(ssd1306_t *d, const char *i2c_dev, uint8_t addr)
{
    int rc;

    memset(d, 0, sizeof(*d));
    d->fd = open(i2c_dev, O_RDWR);
    if (d->fd < 0)
        return -errno;
    if (ioctl(d->fd, I2C_SLAVE, addr) < 0) {
        rc = -errno;
        close(d->fd);
        return rc;
    }
    rc = ssd1306_cmd(d, INIT_SEQ, (int)sizeof(INIT_SEQ));
    if (rc) {
        close(d->fd);
        return rc;
    }
    ssd1306_clear(d);
    return ssd1306_flush(d);
}

void ssd1306_close(ssd1306_t *d)
{
    static const uint8_t off[] = { 0xAE };

    if (d->fd < 0)
        return;
    ssd1306_clear(d);
    ssd1306_flush(d);
    ssd1306_cmd(d, off, 1);
    close(d->fd);
    d->fd = -1;
}

/*
 * Send the framebuffer page by page (8 rows x 128 columns = 128 bytes each),
 * and only the pages that changed since the last flush.
 *  - each I2C transfer is ~12 ms at 100 kHz instead of ~95 ms for a whole
 *    frame, so other users of the bus (the real-time sensor reads) wait at
 *    most one page;
 *  - unchanged pages cost nothing.
 */
int ssd1306_flush(ssd1306_t *d)
{
    uint8_t buf[SSD1306_W + 1];
    int page;

    for (page = 0; page < SSD1306_H / 8; page++) {
        const uint8_t *src = &d->fb[page * SSD1306_W];
        uint8_t *shadow = &d->shadow[page * SSD1306_W];
        uint8_t window[] = { 0x21, 0, SSD1306_W - 1, 0x22, (uint8_t)page, (uint8_t)page };
        int rc;

        if (d->shadow_valid && !memcmp(src, shadow, SSD1306_W))
            continue;                                   /* page unchanged */
        if ((rc = ssd1306_cmd(d, window, (int)sizeof(window))) != 0)
            return rc;
        buf[0] = CTRL_DATA;
        memcpy(&buf[1], src, SSD1306_W);
        if (write(d->fd, buf, sizeof(buf)) != (ssize_t)sizeof(buf))
            return -errno;
        memcpy(shadow, src, SSD1306_W);
    }
    d->shadow_valid = 1;
    return 0;
}

int ssd1306_contrast(ssd1306_t *d, uint8_t level)
{
    uint8_t c[] = { 0x81, level };

    return ssd1306_cmd(d, c, 2);
}

int ssd1306_flip180(ssd1306_t *d, int flipped)
{
    /* segment remap + COM scan direction together rotate the image 180 deg */
    uint8_t c[] = { (uint8_t)(flipped ? 0xA0 : 0xA1), (uint8_t)(flipped ? 0xC0 : 0xC8) };

    return ssd1306_cmd(d, c, 2);
}

void ssd1306_clear(ssd1306_t *d)
{
    memset(d->fb, 0, sizeof(d->fb));
}

void ssd1306_set_rotation(ssd1306_t *d, int rot)
{
    d->rot = rot & 3;
}

int ssd1306_width(const ssd1306_t *d)
{
    return (d->rot & 1) ? SSD1306_H : SSD1306_W;
}

int ssd1306_height(const ssd1306_t *d)
{
    return (d->rot & 1) ? SSD1306_W : SSD1306_H;
}

void ssd1306_pixel(ssd1306_t *d, int x, int y, int on)
{
    uint8_t *b;
    int px, py;

    if (x < 0 || x >= ssd1306_width(d) || y < 0 || y >= ssd1306_height(d))
        return;

    /* map logical (rotated) coordinates to the physical 128x64 panel */
    switch (d->rot) {
    case 1:  px = SSD1306_W - 1 - y; py = x;                 break;
    case 2:  px = SSD1306_W - 1 - x; py = SSD1306_H - 1 - y; break;
    case 3:  px = y;                 py = SSD1306_H - 1 - x; break;
    default: px = x;                 py = y;                 break;
    }
    x = px;
    y = py;
    b = &d->fb[(y / 8) * SSD1306_W + x];
    if (on)
        *b |= (uint8_t)(1u << (y % 8));
    else
        *b &= (uint8_t)~(1u << (y % 8));
}

void ssd1306_line(ssd1306_t *d, int x0, int y0, int x1, int y1)
{
    /* Bresenham */
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    for (;;) {
        ssd1306_pixel(d, x0, y0, 1);
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void ssd1306_rect(ssd1306_t *d, int x, int y, int w, int h, int fill)
{
    int i, j;

    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            if (fill || j == 0 || j == h - 1 || i == 0 || i == w - 1)
                ssd1306_pixel(d, x + i, y + j, 1);
}

static unsigned glyph_row(const aqm_font_t *f, int c, int row)
{
    int idx = (c - f->first) * f->h + row;

    return f->row_bytes == 1 ? ((const uint8_t *)f->bits)[idx]
                             : ((const uint16_t *)f->bits)[idx];
}

int ssd1306_text(ssd1306_t *d, int x, int y, const char *s, const aqm_font_t *f)
{
    return ssd1306_text_color(d, x, y, s, f, 1);
}

int ssd1306_text_color(ssd1306_t *d, int x, int y, const char *s,
                       const aqm_font_t *f, int on)
{
    for (; *s; s++) {
        int c = (unsigned char)*s, row, col;

        if (c < f->first || c > f->last)
            c = '?';
        for (row = 0; row < f->h; row++) {
            unsigned bits = glyph_row(f, c, row);

            for (col = 0; col < f->w; col++)
                if (bits & (1u << (f->w - 1 - col)))
                    ssd1306_pixel(d, x + col, y + row, on);
        }
        x += f->w;
    }
    return x;
}

int ssd1306_text_width(const char *s, const aqm_font_t *f)
{
    return (int)strlen(s) * f->w;
}
