#include "aqm/modbus.h"

#include "aqm/log.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------ protocol --------------------------------- */

uint16_t aqm_modbus_crc(const uint8_t *b, size_t n)
{
    uint16_t crc = 0xFFFF;
    size_t i;
    int k;

    for (i = 0; i < n; i++) {
        crc ^= b[i];
        for (k = 0; k < 8; k++)
            crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
    }
    return crc;
}

static uint16_t x10(float v)
{
    if (v < 0) v = 0;
    if (v > 6553.4f) v = 6553.4f;
    return (uint16_t)(v * 10.0f + 0.5f);
}

static uint16_t avg_x10(const aqm_avg_t *a, int w, int m)
{
    float v;

    return (a && aqm_avg_get(a, w, m, &v) > 0) ? x10(v) : AQM_MB_NA;
}

void aqm_modbus_fill(uint16_t r[AQM_MB_NREGS], const aqm_sample_t *s, const aqm_avg_t *avg)
{
    int pm = !(s->status & AQM_ST_PMS_FAULT);
    int ens = !(s->status & AQM_ST_ENS_FAULT);
    int aht = !(s->status & AQM_ST_AHT_FAULT);
    int w, mins = 0, aqi24 = avg ? aqm_avg_aqi_24h(avg, &mins) : -1;
    int16_t t10;

    r[0] = pm ? s->pm1 : AQM_MB_NA;
    r[1] = pm ? x10(s->pm25_cal) : AQM_MB_NA;
    r[2] = pm ? x10(s->pm10_cal) : AQM_MB_NA;
    r[3] = ens ? s->tvoc_ppb : AQM_MB_NA;
    r[4] = (ens && s->eco2_ppm) ? s->eco2_ppm : AQM_MB_NA;
    t10 = (int16_t)(s->temp_c * 10.0f + (s->temp_c < 0 ? -0.5f : 0.5f));
    r[5] = aht ? (uint16_t)t10 : AQM_MB_NA_S16;
    r[6] = aht ? x10(s->rh_pct) : AQM_MB_NA;
    r[7] = pm ? s->aqi : AQM_MB_NA;
    r[8] = pm ? s->aqi_cat : AQM_MB_NA;
    r[9] = (uint16_t)(s->status & 0xFFFF);
    r[10] = (uint16_t)(s->seq & 0xFFFF);
    r[11] = (uint16_t)(s->seq >> 16);
    for (w = 0; w < AQM_W_COUNT; w++) {
        r[12 + w] = avg_x10(avg, w, AQM_M_PM25);
        r[17 + w] = avg_x10(avg, w, AQM_M_PM10);
    }
    r[22] = aqi24 >= 0 ? (uint16_t)aqi24 : AQM_MB_NA;
    r[23] = (uint16_t)mins;
    r[24] = AQM_MB_MAP_VER;
}

static int exception(uint8_t id, uint8_t fc, uint8_t code, uint8_t *resp)
{
    uint16_t crc;

    resp[0] = id;
    resp[1] = (uint8_t)(fc | 0x80);
    resp[2] = code;
    crc = aqm_modbus_crc(resp, 3);
    resp[3] = (uint8_t)(crc & 0xFF);
    resp[4] = (uint8_t)(crc >> 8);
    return 5;
}

int aqm_modbus_handle(uint8_t id, const uint16_t *regs, int nregs,
                      const uint8_t *req, int len, uint8_t *resp, int resp_max)
{
    uint16_t crc, start, qty;
    uint8_t fc;
    int i, n;

    if (len < 4)
        return 0;
    crc = (uint16_t)(req[len - 2] | (req[len - 1] << 8));
    if (aqm_modbus_crc(req, (size_t)(len - 2)) != crc)
        return 0;                                   /* corrupt: stay silent */
    if (req[0] != id)
        return 0;                                   /* other slave or broadcast */
    fc = req[1];

    if (fc != 0x03 && fc != 0x04)
        return exception(id, fc, AQM_MB_EX_FUNC, resp);
    if (len != 8)
        return exception(id, fc, AQM_MB_EX_VALUE, resp);
    start = (uint16_t)((req[2] << 8) | req[3]);
    qty   = (uint16_t)((req[4] << 8) | req[5]);
    if (qty < 1 || qty > 125)
        return exception(id, fc, AQM_MB_EX_VALUE, resp);
    if ((int)start + qty > nregs)
        return exception(id, fc, AQM_MB_EX_ADDR, resp);

    n = 3 + 2 * qty + 2;
    if (n > resp_max)
        return exception(id, fc, AQM_MB_EX_VALUE, resp);
    resp[0] = id;
    resp[1] = fc;
    resp[2] = (uint8_t)(2 * qty);
    for (i = 0; i < qty; i++) {                     /* registers are big-endian */
        resp[3 + 2 * i] = (uint8_t)(regs[start + i] >> 8);
        resp[4 + 2 * i] = (uint8_t)(regs[start + i] & 0xFF);
    }
    crc = aqm_modbus_crc(resp, (size_t)(n - 2));
    resp[n - 2] = (uint8_t)(crc & 0xFF);            /* CRC is little-endian */
    resp[n - 1] = (uint8_t)(crc >> 8);
    return n;
}

/* --------------------------- serial server ------------------------------- */

struct aqm_modbus {
    int             fd;
    uint8_t         id;
    int             baud;
    int             gap_ms;          /* t3.5 silence that ends a frame */
    char            dev[128];
    pthread_t       th;
    pthread_mutex_t lock;            /* protects regs */
    uint16_t        regs[AQM_MB_NREGS];
    int             stop;
    unsigned long   frames, replies, bad;
};

static speed_t baud_const(int b)
{
    switch (b) {
    case 2400:   return B2400;
    case 4800:   return B4800;
    case 9600:   return B9600;
    case 19200:  return B19200;
    case 38400:  return B38400;
    case 57600:  return B57600;
    case 115200: return B115200;
    default:     return 0;
    }
}

static int open_tty(const char *dev, int baud, char parity)
{
    struct termios t;
    speed_t sp = baud_const(baud);
    int fd;

    if (!sp)
        return -EINVAL;
    fd = open(dev, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (fd < 0)
        return -errno;
    if (tcgetattr(fd, &t) < 0) {
        int e = errno;
        close(fd);
        return -e;
    }
    cfmakeraw(&t);
    cfsetispeed(&t, sp);
    cfsetospeed(&t, sp);
    t.c_cflag |= CLOCAL | CREAD;
    t.c_cflag &= ~(CSTOPB | PARENB | PARODD | CRTSCTS);
    if (parity == 'E')
        t.c_cflag |= PARENB;
    else if (parity == 'O')
        t.c_cflag |= PARENB | PARODD;
    else
        t.c_cflag |= CSTOPB;                         /* Modbus: no parity -> 2 stop bits */
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &t) < 0) {
        int e = errno;
        close(fd);
        return -e;
    }
    tcflush(fd, TCIOFLUSH);
    return fd;
}

static void *server(void *arg)
{
    aqm_modbus_t *m = arg;
    uint8_t req[256], resp[256];
    uint16_t regs[AQM_MB_NREGS];
    int len = 0;

    while (!__atomic_load_n(&m->stop, __ATOMIC_ACQUIRE)) {
        struct pollfd p = { m->fd, POLLIN, 0 };
        /* waiting for a frame: 200 ms slices (to notice stop); inside a frame:
         * the t3.5 gap decides where it ends */
        int rc = poll(&p, 1, len ? m->gap_ms : 200);

        if (rc < 0) {
            if (errno == EINTR)
                continue;
            aqm_log("report", "Modbus poll: %s", strerror(errno));
            break;
        }
        if (rc > 0) {
            ssize_t n = read(m->fd, req + len, sizeof(req) - (size_t)len);

            if (n > 0)
                len += (int)n;
            if (len >= (int)sizeof(req))
                len = 0;                             /* garbage: drop it */
            continue;
        }
        if (!len)
            continue;

        /* silence after bytes: one complete frame */
        m->frames++;
        pthread_mutex_lock(&m->lock);
        memcpy(regs, m->regs, sizeof(regs));
        pthread_mutex_unlock(&m->lock);
        if (len < 4 || aqm_modbus_crc(req, (size_t)len - 2) !=
                       (uint16_t)(req[len - 2] | (req[len - 1] << 8)))
            m->bad++;                                /* noise or a cut frame */
        rc = aqm_modbus_handle(m->id, regs, AQM_MB_NREGS, req, len, resp, sizeof(resp));
        if (rc > 0 && write(m->fd, resp, (size_t)rc) == rc) {
            tcdrain(m->fd);                          /* fully sent before listening again */
            m->replies++;
        }
        len = 0;
    }
    return NULL;
}

static int load_conf(const char *path, aqm_modbus_t *m, int *enabled, char *parity)
{
    char line[256], key[32], val[128];
    FILE *f = fopen(path, "r");

    if (!f)
        return -errno;
    while (fgets(line, sizeof(line), f)) {
        char *h = strchr(line, '#');

        if (h)
            *h = '\0';
        if (sscanf(line, " %31[^= ] = %127s", key, val) != 2)
            continue;
        if (!strcmp(key, "enabled"))     *enabled = atoi(val);
        else if (!strcmp(key, "device")) snprintf(m->dev, sizeof(m->dev), "%.127s", val);
        else if (!strcmp(key, "baud"))   m->baud = atoi(val);
        else if (!strcmp(key, "parity")) *parity = val[0];
        else if (!strcmp(key, "id"))     m->id = (uint8_t)atoi(val);
    }
    fclose(f);
    return 0;
}

aqm_modbus_t *aqm_modbus_start(const char *conf_path)
{
    aqm_modbus_t *m = calloc(1, sizeof(*m));
    int enabled = 1, rc, i;
    char parity = 'N';

    if (!m)
        return NULL;
    snprintf(m->dev, sizeof(m->dev), "/dev/ttyS4");
    m->baud = 9600;
    m->id = 1;
    if ((rc = load_conf(conf_path, m, &enabled, &parity)) != 0 || !enabled) {
        aqm_log("report", "Modbus off (%s)", rc ? strerror(-rc) : "enabled = 0");
        free(m);
        return NULL;
    }
    if (m->id < 1 || m->id > 247) {
        aqm_log("report", "Modbus off: slave id %u not in 1..247", m->id);
        free(m);
        return NULL;
    }
    /* t3.5 = 3.5 characters of 11 bits; the spec fixes 1.75 ms above 19200 baud.
     * Linux tty latency adds a few ms, so use a safe floor of 5 ms */
    m->gap_ms = (int)(3.5 * 11 * 1000 / m->baud + 1);
    if (m->gap_ms < 5)
        m->gap_ms = 5;
    for (i = 0; i < AQM_MB_NREGS; i++)
        m->regs[i] = AQM_MB_NA;
    m->regs[24] = AQM_MB_MAP_VER;

    m->fd = open_tty(m->dev, m->baud, parity);
    if (m->fd < 0) {
        aqm_log("report", "Modbus off: %s: %s", m->dev, strerror(-m->fd));
        free(m);
        return NULL;
    }
    pthread_mutex_init(&m->lock, NULL);
    if (pthread_create(&m->th, NULL, server, m) != 0) {
        aqm_log("report", "Modbus thread failed");
        close(m->fd);
        free(m);
        return NULL;
    }
    aqm_log("report", "Modbus RTU slave id %u on %s %d 8%c%c (gap %d ms, %d registers)",
            m->id, m->dev, m->baud, parity, parity == 'N' ? '2' : '1', m->gap_ms, AQM_MB_NREGS);
    return m;
}

void aqm_modbus_update(aqm_modbus_t *m, const aqm_sample_t *s, const aqm_avg_t *avg)
{
    uint16_t r[AQM_MB_NREGS];

    if (!m)
        return;
    aqm_modbus_fill(r, s, avg);                      /* outside the lock */
    pthread_mutex_lock(&m->lock);
    memcpy(m->regs, r, sizeof(r));
    pthread_mutex_unlock(&m->lock);
}

void aqm_modbus_stop(aqm_modbus_t *m)
{
    if (!m)
        return;
    __atomic_store_n(&m->stop, 1, __ATOMIC_RELEASE);
    pthread_join(m->th, NULL);
    close(m->fd);
    pthread_mutex_destroy(&m->lock);
    aqm_log("report", "Modbus: %lu frames, %lu replies, %lu bad CRC", m->frames, m->replies, m->bad);
    free(m);
}
