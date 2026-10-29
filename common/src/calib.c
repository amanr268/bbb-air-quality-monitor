#include "aqm/calib.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

void aqm_calib_defaults(aqm_calib_t *c)
{
    c->pm25_gain   = 1.0f;
    c->pm25_offset = 0.0f;
    c->pm10_gain   = 1.0f;
    c->pm10_offset = 0.0f;
    c->temp_offset = 0.0f;
    c->rh_offset   = 0.0f;
}

int aqm_calib_load(aqm_calib_t *c, const char *path)
{
    aqm_calib_t t = *c;
    char line[128], key[64];
    float v;
    FILE *f = fopen(path, "r");

    if (!f)
        return -errno;

    while (fgets(line, sizeof(line), f)) {
        /* comment and blank lines fail this match and are skipped */
        if (sscanf(line, " %63[a-z0-9_] = %f", key, &v) != 2)
            continue;
        if      (!strcmp(key, "pm25_gain"))   t.pm25_gain   = v;
        else if (!strcmp(key, "pm25_offset")) t.pm25_offset = v;
        else if (!strcmp(key, "pm10_gain"))   t.pm10_gain   = v;
        else if (!strcmp(key, "pm10_offset")) t.pm10_offset = v;
        else if (!strcmp(key, "temp_offset")) t.temp_offset = v;
        else if (!strcmp(key, "rh_offset"))   t.rh_offset   = v;
    }
    fclose(f);
    *c = t;
    return 0;
}
