#include "aqm/aqi.h"

#include <stdio.h>

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
    return 1; } } while (0)

int main(void)
{
    /* PM2.5 breakpoints (CPCB) */
    CHECK(aqm_aqi_pm25(0)    == 0);
    CHECK(aqm_aqi_pm25(30)   == 50);
    CHECK(aqm_aqi_pm25(45)   == 75);     /* mid-segment interpolation */
    CHECK(aqm_aqi_pm25(60)   == 100);
    CHECK(aqm_aqi_pm25(90)   == 200);
    CHECK(aqm_aqi_pm25(250)  == 400);
    CHECK(aqm_aqi_pm25(1000) == 500);    /* capped */

    /* PM10 breakpoints */
    CHECK(aqm_aqi_pm10(100)  == 100);
    CHECK(aqm_aqi_pm10(175)  == 150);
    CHECK(aqm_aqi_pm10(350)  == 300);

    /* categories */
    CHECK(aqm_aqi_category(50)  == AQI_GOOD);
    CHECK(aqm_aqi_category(75)  == AQI_SATISFACTORY);
    CHECK(aqm_aqi_category(201) == AQI_POOR);
    CHECK(aqm_aqi_category(450) == AQI_SEVERE);

    printf("aqi ok\n");
    return 0;
}
