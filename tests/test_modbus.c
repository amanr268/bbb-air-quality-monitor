/* Modbus RTU: CRC, register reads, exceptions, silence rules, fault values */
#include "aqm/modbus.h"

#include <stdio.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
    return 1; } } while (0)

static int req(uint8_t *b, uint8_t id, uint8_t fc, uint16_t start, uint16_t qty)
{
    uint16_t crc;

    b[0] = id; b[1] = fc;
    b[2] = (uint8_t)(start >> 8); b[3] = (uint8_t)start;
    b[4] = (uint8_t)(qty >> 8);   b[5] = (uint8_t)qty;
    crc = aqm_modbus_crc(b, 6);
    b[6] = (uint8_t)crc; b[7] = (uint8_t)(crc >> 8);
    return 8;
}

static uint16_t reg_at(const uint8_t *resp, int i)
{
    return (uint16_t)((resp[3 + 2 * i] << 8) | resp[4 + 2 * i]);
}

int main(void)
{
    static const uint8_t ex[] = { 0x01, 0x03, 0x00, 0x00, 0x00, 0x0A };
    uint16_t regs[AQM_MB_NREGS];
    uint8_t q[16], r[256];
    aqm_sample_t s;
    int n;

    /* textbook example: 01 03 00 00 00 0A -> CRC bytes C5 CD */
    CHECK(aqm_modbus_crc(ex, 6) == 0xCDC5);

    memset(&s, 0, sizeof(s));
    s.pm1 = 39; s.pm25_cal = 67.4f; s.pm10_cal = 76.0f; s.tvoc_ppb = 212; s.eco2_ppm = 690;
    s.temp_c = -3.25f; s.rh_pct = 48.0f; s.aqi = 123; s.aqi_cat = 2; s.seq = 0x12345;
    aqm_modbus_fill(regs, &s, NULL);
    CHECK(regs[0] == 39 && regs[1] == 674 && regs[2] == 760 && regs[3] == 212 && regs[4] == 690);
    CHECK((int16_t)regs[5] == -33 && regs[6] == 480 && regs[7] == 123 && regs[8] == 2);
    CHECK(regs[10] == 0x2345 && regs[11] == 0x0001 && regs[24] == AQM_MB_MAP_VER);
    CHECK(regs[12] == AQM_MB_NA && regs[22] == AQM_MB_NA);       /* no averages given */

    /* FC04 read 0..8 */
    n = aqm_modbus_handle(1, regs, AQM_MB_NREGS, q, req(q, 1, 0x04, 0, 9), r, sizeof(r));
    CHECK(n == 3 + 18 + 2 && r[0] == 1 && r[1] == 0x04 && r[2] == 18);
    CHECK(reg_at(r, 1) == 674 && reg_at(r, 7) == 123);
    CHECK(aqm_modbus_crc(r, (size_t)n - 2) == (uint16_t)(r[n - 2] | (r[n - 1] << 8)));

    /* FC03 gives the same map */
    n = aqm_modbus_handle(1, regs, AQM_MB_NREGS, q, req(q, 1, 0x03, 24, 1), r, sizeof(r));
    CHECK(n == 7 && reg_at(r, 0) == AQM_MB_MAP_VER);

    /* exceptions: past the end -> 02, qty 0 -> 03, write single -> 01 */
    n = aqm_modbus_handle(1, regs, AQM_MB_NREGS, q, req(q, 1, 0x04, 20, 10), r, sizeof(r));
    CHECK(n == 5 && r[1] == 0x84 && r[2] == AQM_MB_EX_ADDR);
    n = aqm_modbus_handle(1, regs, AQM_MB_NREGS, q, req(q, 1, 0x04, 0, 0), r, sizeof(r));
    CHECK(n == 5 && r[2] == AQM_MB_EX_VALUE);
    n = aqm_modbus_handle(1, regs, AQM_MB_NREGS, q, req(q, 1, 0x06, 0, 1), r, sizeof(r));
    CHECK(n == 5 && r[1] == 0x86 && r[2] == AQM_MB_EX_FUNC);

    /* silence: other slave, broadcast, corrupted CRC, runt frame */
    CHECK(aqm_modbus_handle(1, regs, AQM_MB_NREGS, q, req(q, 2, 0x04, 0, 1), r, sizeof(r)) == 0);
    CHECK(aqm_modbus_handle(1, regs, AQM_MB_NREGS, q, req(q, 0, 0x04, 0, 1), r, sizeof(r)) == 0);
    req(q, 1, 0x04, 0, 1); q[3] ^= 1;
    CHECK(aqm_modbus_handle(1, regs, AQM_MB_NREGS, q, 8, r, sizeof(r)) == 0);
    CHECK(aqm_modbus_handle(1, regs, AQM_MB_NREGS, q, 3, r, sizeof(r)) == 0);

    /* faulty sensors read as "no data", never 0 */
    s.status = AQM_ST_PMS_FAULT | AQM_ST_ENS_FAULT | AQM_ST_AHT_FAULT;
    aqm_modbus_fill(regs, &s, NULL);
    CHECK(regs[1] == AQM_MB_NA && regs[3] == AQM_MB_NA && regs[5] == AQM_MB_NA_S16 &&
          regs[7] == AQM_MB_NA && regs[9] == 7);

    printf("modbus ok\n");
    return 0;
}
