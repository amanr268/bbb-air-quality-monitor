#ifndef AQM_CALIB_H
#define AQM_CALIB_H

/* corrected = raw * gain + offset */
typedef struct {
    float pm25_gain, pm25_offset;
    float pm10_gain, pm10_offset;
    float temp_offset;     /* deg C */
    float rh_offset;       /* %RH   */
} aqm_calib_t;

void aqm_calib_defaults(aqm_calib_t *c);
/* "key = value" lines, '#' comments; returns 0 or -errno (c unchanged on error) */
int  aqm_calib_load(aqm_calib_t *c, const char *path);

#endif
