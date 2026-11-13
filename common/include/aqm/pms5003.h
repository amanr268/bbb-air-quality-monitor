#ifndef AQM_PMS5003_H
#define AQM_PMS5003_H

#include <stddef.h>
#include <stdint.h>

/* Plantower PMS5003 laser particle sensor, UART 9600 8N1, active mode:
 * one 32-byte frame about every second:
 *   42 4D | len(28) | 13 x uint16 (big-endian) | checksum(uint16)
 * The checksum is the 16-bit sum of the first 30 bytes. */

#define PMS5003_FRAME_LEN  32

typedef struct {
    uint16_t pm1_cf, pm25_cf, pm10_cf;   /* ug/m3, factory "CF=1" (lab)      */
    uint16_t pm1, pm25, pm10;            /* ug/m3, atmospheric: use these    */
    uint16_t n03, n05, n10, n25, n50, n100;   /* particles > x um per 0.1 L  */
    uint8_t  version, error;
} pms5003_frame_t;

typedef struct {
    int      fd;
    uint8_t  buf[PMS5003_FRAME_LEN];
    int      len;                /* bytes collected of the current frame  */
    /* statistics */
    uint32_t frames_ok;
    uint32_t bad_checksum;
    uint32_t bad_length;
    uint32_t resync_bytes;       /* bytes skipped while looking for 42 4D */
    /* latest good frame */
    pms5003_frame_t last;
    int      have_frame;
} pms5003_t;

/* open and configure the UART (9600 8N1 raw, non-blocking); 0 or -errno */
int  pms5003_open(pms5003_t *p, const char *dev);
void pms5003_close(pms5003_t *p);

/* read whatever is waiting in the UART and parse it (never blocks);
 * returns the number of new valid frames, or -errno */
int  pms5003_poll(pms5003_t *p);

/* parser only (no I/O): feed raw bytes; returns new valid frames.
 * Used by pms5003_poll and by the unit test. */
int  pms5003_feed(pms5003_t *p, const uint8_t *data, size_t n);

/* reset parser state and statistics (fd untouched) */
void pms5003_reset(pms5003_t *p);

#endif
