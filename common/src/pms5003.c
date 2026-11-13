#include "aqm/pms5003.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define START1  0x42
#define START2  0x4D
#define BODY_LEN  28                 /* value of the length field */

static uint16_t be16(const uint8_t *b)
{
    return (uint16_t)((b[0] << 8) | b[1]);
}

void pms5003_reset(pms5003_t *p)
{
    int fd = p->fd;

    memset(p, 0, sizeof(*p));
    p->fd = fd;
}

int pms5003_open(pms5003_t *p, const char *dev)
{
    struct termios t;
    int rc;

    p->fd = -1;
    pms5003_reset(p);
    p->fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (p->fd < 0)
        return -errno;

    if (tcgetattr(p->fd, &t) != 0)
        goto fail;
    cfmakeraw(&t);                       /* 8N1, no echo, no line editing */
    cfsetispeed(&t, B9600);
    cfsetospeed(&t, B9600);
    t.c_cflag |= CLOCAL | CREAD;
    t.c_cflag &= ~(CSTOPB | CRTSCTS);
    t.c_cc[VMIN]  = 0;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(p->fd, TCSANOW, &t) != 0)
        goto fail;
    tcflush(p->fd, TCIFLUSH);            /* drop stale bytes */
    return 0;

fail:
    rc = -errno;
    close(p->fd);
    p->fd = -1;
    return rc;
}

void pms5003_close(pms5003_t *p)
{
    if (p->fd >= 0) {
        close(p->fd);
        p->fd = -1;
    }
}

static int frame_done(pms5003_t *p)
{
    const uint8_t *b = p->buf;
    uint16_t sum = 0;
    int i;

    for (i = 0; i < PMS5003_FRAME_LEN - 2; i++)
        sum = (uint16_t)(sum + b[i]);
    if (sum != be16(&b[30])) {
        p->bad_checksum++;
        return 0;
    }

    p->last.pm1_cf  = be16(&b[4]);
    p->last.pm25_cf = be16(&b[6]);
    p->last.pm10_cf = be16(&b[8]);
    p->last.pm1     = be16(&b[10]);
    p->last.pm25    = be16(&b[12]);
    p->last.pm10    = be16(&b[14]);
    p->last.n03     = be16(&b[16]);
    p->last.n05     = be16(&b[18]);
    p->last.n10     = be16(&b[20]);
    p->last.n25     = be16(&b[22]);
    p->last.n50     = be16(&b[24]);
    p->last.n100    = be16(&b[26]);
    p->last.version = b[28];
    p->last.error   = b[29];
    p->have_frame = 1;
    p->frames_ok++;
    return 1;
}

/* byte-at-a-time state machine: survives garbage, split reads, lost bytes */
int pms5003_feed(pms5003_t *p, const uint8_t *data, size_t n)
{
    int frames = 0;
    size_t i;

    for (i = 0; i < n; i++) {
        uint8_t c = data[i];

        if (p->len == 0) {                       /* waiting for 0x42 */
            if (c == START1)
                p->buf[p->len++] = c;
            else
                p->resync_bytes++;
            continue;
        }
        if (p->len == 1) {                       /* waiting for 0x4D */
            if (c == START2) {
                p->buf[p->len++] = c;
            } else {
                p->resync_bytes++;
                p->len = (c == START1) ? 1 : 0;  /* 42 42 4D still valid */
            }
            continue;
        }

        p->buf[p->len++] = c;
        if (p->len == 4 && be16(&p->buf[2]) != BODY_LEN) {
            p->bad_length++;                     /* not a real frame: resync */
            p->len = 0;
            continue;
        }
        if (p->len == PMS5003_FRAME_LEN) {
            frames += frame_done(p);
            p->len = 0;
        }
    }
    return frames;
}

int pms5003_poll(pms5003_t *p)
{
    uint8_t chunk[128];
    int frames = 0;
    ssize_t n;

    if (p->fd < 0)
        return -EBADF;
    while ((n = read(p->fd, chunk, sizeof(chunk))) > 0)
        frames += pms5003_feed(p, chunk, (size_t)n);
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        return -errno;
    return frames;
}
