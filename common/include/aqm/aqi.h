#ifndef AQM_AQI_H
#define AQM_AQI_H

/* Indian National AQI (CPCB) sub-indices for PM2.5 and PM10.
 * CPCB defines AQI on 24-h averages; here it is applied to the current
 * (calibrated) value and later to rolling averages. */

typedef enum {
    AQI_GOOD = 0,          /*   0 -  50 */
    AQI_SATISFACTORY,      /*  51 - 100 */
    AQI_MODERATE,          /* 101 - 200 */
    AQI_POOR,              /* 201 - 300 */
    AQI_VERY_POOR,         /* 301 - 400 */
    AQI_SEVERE             /* 401 - 500 */
} aqm_aqi_cat_t;

int           aqm_aqi_pm25(float ug_m3);
int           aqm_aqi_pm10(float ug_m3);
aqm_aqi_cat_t aqm_aqi_category(int aqi);
const char   *aqm_aqi_cat_name(aqm_aqi_cat_t c);

#endif
