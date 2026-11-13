/* Checks the page-wise, changed-only flush by writing to a plain file
 * instead of the I2C bus and counting the bytes sent. */
#include "aqm/ssd1306.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
    return 1; } } while (0)

#define PAGE_BYTES  (7 + 129)   /* window command (1+6) + data (1+128) */

static long size_of(const char *p)
{
    struct stat st;

    return stat(p, &st) == 0 ? (long)st.st_size : -1;
}

int main(void)
{
    const char *path = "test_ssd1306.bin";
    ssd1306_t d;

    memset(&d, 0, sizeof(d));
    d.fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(d.fd >= 0);

    /* first flush: all 8 pages */
    CHECK(ssd1306_flush(&d) == 0);
    CHECK(size_of(path) == 8 * PAGE_BYTES);

    /* nothing changed: nothing sent */
    CHECK(ssd1306_flush(&d) == 0);
    CHECK(size_of(path) == 8 * PAGE_BYTES);

    /* one pixel in row 30 (page 3): exactly one page sent */
    ssd1306_pixel(&d, 10, 30, 1);
    CHECK(ssd1306_flush(&d) == 0);
    CHECK(size_of(path) == 9 * PAGE_BYTES);

    /* text spanning rows 0..9 touches pages 0 and 1 */
    ssd1306_text(&d, 0, 0, "AQI", &font_6_10);
    CHECK(ssd1306_flush(&d) == 0);
    CHECK(size_of(path) == 11 * PAGE_BYTES);

    /* inverted text clears glyph pixels inside a filled bar */
    ssd1306_clear(&d);
    ssd1306_rect(&d, 0, 0, 128, 10, 1);
    ssd1306_text_color(&d, 0, 0, "!", &font_6_10, 0);
    CHECK((d.fb[2] & 0x02) == 0);         /* '!' stroke at column 2, row 1 is dark */
    CHECK(d.fb[127] == 0xFF);             /* bar elsewhere fully lit (rows 0-7)    */

    close(d.fd);
    unlink(path);
    printf("ssd1306 ok\n");
    return 0;
}
