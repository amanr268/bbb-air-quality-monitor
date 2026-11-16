#ifndef AQM_MODBUS_H
#define AQM_MODBUS_H

#include <stddef.h>
#include <stdint.h>

#include "aqm/avg.h"
#include "aqm/types.h"

/*
 * Modbus RTU slave (RS485 via UART4 + an auto-direction transceiver).
 *
 * Function codes 03 (read holding) and 04 (read input) return the same
 * read-only register map below; anything else gets exception 01.
 * Values from a faulty sensor read as AQM_MB_NA (0xFFFF), or AQM_MB_NA_S16
 * (0x8000) for signed registers - never as 0.
 *
 *  reg  value                     unit / scale
 *    0  PM1.0                     ug/m3
 *    1  PM2.5                     ug/m3 x10
 *    2  PM10                      ug/m3 x10
 *    3  TVOC                      ppb
 *    4  eCO2 (estimate)           ppm
 *    5  temperature               degC x10, signed
 *    6  relative humidity         %RH x10
 *    7  AQI (CPCB, instant)
 *    8  AQI category              0 Good .. 5 Severe
 *    9  status bits               1 PM fault, 2 ENS160 fault, 4 AHT21 fault
 *   10  sample sequence           low 16 bits
 *   11  sample sequence           high 16 bits
 *   12  PM2.5 mean  1 min         ug/m3 x10
 *   13  PM2.5 mean  5 min
 *   14  PM2.5 mean 10 min
 *   15  PM2.5 mean 20 min
 *   16  PM2.5 mean 24 h
 *   17  PM10  mean  1 min         ug/m3 x10
 *   18  PM10  mean  5 min
 *   19  PM10  mean 10 min
 *   20  PM10  mean 20 min
 *   21  PM10  mean 24 h
 *   22  AQI 24 h (CPCB)           0xFFFF until 16 h of data
 *   23  minutes of 24 h data
 *   24  register map version      0x0100
 */

#define AQM_MB_NREGS     25
#define AQM_MB_NA        0xFFFFu
#define AQM_MB_NA_S16    0x8000u
#define AQM_MB_MAP_VER   0x0100u

enum { AQM_MB_EX_FUNC = 1, AQM_MB_EX_ADDR = 2, AQM_MB_EX_VALUE = 3 };

uint16_t aqm_modbus_crc(const uint8_t *b, size_t n);   /* CRC-16/MODBUS */

/* fill the register map from a sample and the rolling averages (avg may be NULL) */
void aqm_modbus_fill(uint16_t regs[AQM_MB_NREGS], const aqm_sample_t *s, const aqm_avg_t *avg);

/* process one received frame; returns the response length to send, or 0 for
 * no response (bad CRC, other slave, broadcast, too short) */
int aqm_modbus_handle(uint8_t slave_id, const uint16_t *regs, int nregs,
                      const uint8_t *req, int len, uint8_t *resp, int resp_max);

/* serial server: a thread that answers requests on a tty.
 * conf file keys: enabled, device, baud, parity (N/E/O), id */
typedef struct aqm_modbus aqm_modbus_t;

aqm_modbus_t *aqm_modbus_start(const char *conf_path);    /* NULL = off (logged) */
void          aqm_modbus_update(aqm_modbus_t *m, const aqm_sample_t *s, const aqm_avg_t *avg);
void          aqm_modbus_stop(aqm_modbus_t *m);

#endif
