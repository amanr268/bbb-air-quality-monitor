#include "aqm/aqi.h"

#include <stddef.h>

typedef struct {
    float c_lo, c_hi;   /* concentration range, ug/m3 */
    int   i_lo, i_hi;   /* index range                */
} seg_t;

/* CPCB breakpoints; top segment upper bound used only to cap at 500 */
static const seg_t PM25[] = {
    {   0,  30,   0,  50 },
    {  30,  60,  50, 100 },
    {  60,  90, 100, 200 },
    {  90, 120, 200, 300 },
    { 120, 250, 300, 400 },
    { 250, 380, 400, 500 },
};

static const seg_t PM10[] = {
    {   0,  50,   0,  50 },
    {  50, 100,  50, 100 },
    { 100, 250, 100, 200 },
    { 250, 350, 200, 300 },
    { 350, 430, 300, 400 },
    { 430, 510, 400, 500 },
};

/* linear interpolation inside the matching segment */
static int sub_index(const seg_t *t, size_t n, float c)
{
    size_t i;

    if (c <= 0.0f)
        return 0;
    for (i = 0; i < n; i++) {
        if (c <= t[i].c_hi) {
            float v = (float)t[i].i_lo +
                      (c - t[i].c_lo) * (float)(t[i].i_hi - t[i].i_lo) /
                      (t[i].c_hi - t[i].c_lo);
            return (int)(v + 0.5f);
        }
    }
    return 500;
}

int aqm_aqi_pm25(float ug_m3)
{
    return sub_index(PM25, sizeof(PM25) / sizeof(PM25[0]), ug_m3);
}

int aqm_aqi_pm10(float ug_m3)
{
    return sub_index(PM10, sizeof(PM10) / sizeof(PM10[0]), ug_m3);
}

aqm_aqi_cat_t aqm_aqi_category(int aqi)
{
    if (aqi <= 50)  return AQI_GOOD;
    if (aqi <= 100) return AQI_SATISFACTORY;
    if (aqi <= 200) return AQI_MODERATE;
    if (aqi <= 300) return AQI_POOR;
    if (aqi <= 400) return AQI_VERY_POOR;
    return AQI_SEVERE;
}

const char *aqm_aqi_cat_name(aqm_aqi_cat_t c)
{
    static const char *names[] = {
        "Good", "Satisfactory", "Moderate", "Poor", "Very Poor", "Severe"
    };

    return (c <= AQI_SEVERE) ? names[c] : "?";
}
