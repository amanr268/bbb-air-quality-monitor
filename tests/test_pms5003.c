/* PMS5003 parser test using two real frames captured from the board. */
#include "aqm/pms5003.h"

#include <stdio.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
    return 1; } } while (0)

static const uint8_t CAPTURE[64] = {
    0x42, 0x4d, 0x00, 0x1c, 0x00, 0x3b, 0x00, 0x66, 0x00, 0x70, 0x00, 0x27, 0x00, 0x43, 0x00, 0x4c,
    0x0e, 0x12, 0x0c, 0x0b, 0x03, 0x21, 0x00, 0x5a, 0x00, 0x0c, 0x00, 0x07, 0x97, 0x00, 0x03, 0xd1,
    0x42, 0x4d, 0x00, 0x1c, 0x00, 0x3b, 0x00, 0x66, 0x00, 0x70, 0x00, 0x27, 0x00, 0x43, 0x00, 0x4c,
    0x0d, 0xcc, 0x0b, 0xc2, 0x03, 0x04, 0x00, 0x59, 0x00, 0x09, 0x00, 0x06, 0x97, 0x00, 0x05, 0x1e,
};

int main(void)
{
    pms5003_t p;
    uint8_t buf[200];
    size_t i;

    memset(&p, 0, sizeof(p));
    p.fd = -1;

    /* two clean frames */
    pms5003_reset(&p);
    CHECK(pms5003_feed(&p, CAPTURE, sizeof(CAPTURE)) == 2);
    CHECK(p.frames_ok == 2 && p.bad_checksum == 0);
    CHECK(p.last.pm1 == 39 && p.last.pm25 == 67 && p.last.pm10 == 76);
    CHECK(p.last.pm1_cf == 59 && p.last.pm25_cf == 102 && p.last.pm10_cf == 112);
    CHECK(p.last.n03 == 3532 && p.last.version == 0x97 && p.last.error == 0);

    /* byte-at-a-time (split reads) gives the same result */
    pms5003_reset(&p);
    for (i = 0; i < sizeof(CAPTURE); i++)
        pms5003_feed(&p, &CAPTURE[i], 1);
    CHECK(p.frames_ok == 2 && p.last.pm25 == 67);

    /* garbage before, a stray 0x42 and a half frame: resyncs and finds both */
    pms5003_reset(&p);
    memset(buf, 0x55, 10);
    buf[10] = 0x42;                                  /* stray start byte   */
    memcpy(&buf[11], CAPTURE, 20);                   /* truncated frame    */
    memcpy(&buf[31], CAPTURE, sizeof(CAPTURE));
    CHECK(pms5003_feed(&p, buf, 31 + sizeof(CAPTURE)) >= 1);
    CHECK(p.last.pm25 == 67);
    CHECK(p.resync_bytes >= 10);

    /* corrupted checksum is rejected */
    pms5003_reset(&p);
    memcpy(buf, CAPTURE, 32);
    buf[13] ^= 0x01;                                 /* flip a PM2.5 bit   */
    CHECK(pms5003_feed(&p, buf, 32) == 0);
    CHECK(p.bad_checksum == 1 && !p.have_frame);

    /* wrong length field is rejected without eating the next frame */
    pms5003_reset(&p);
    memcpy(buf, CAPTURE, 32);
    buf[3] = 0x14;                                   /* len 20, not 28     */
    memcpy(&buf[32], CAPTURE, 32);
    CHECK(pms5003_feed(&p, buf, 64) == 1);
    CHECK(p.bad_length == 1 && p.last.pm25 == 67);

    printf("pms5003 ok\n");
    return 0;
}
