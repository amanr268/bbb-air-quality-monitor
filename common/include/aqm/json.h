#ifndef AQM_JSON_H
#define AQM_JSON_H

#include <stddef.h>

#include "aqm/alarm.h"
#include "aqm/avg.h"
#include "aqm/types.h"

/*
 * JSON payloads published over MQTT. Values from a faulty sensor are `null`
 * (never 0), so a consumer can tell "clean air" from "no data".
 * Each returns the length written (snprintf semantics), or -1 if buf is too small.
 */
int aqm_json_sample(char *buf, size_t n, const aqm_sample_t *s);
int aqm_json_avg(char *buf, size_t n, const aqm_avg_t *a, long long ts);
int aqm_json_alarm(char *buf, size_t n, const aqm_alarm_t *al);

#endif
