/*
 * pms_read: print decoded PMS5003 frames live (sensor check without the pipeline).
 *
 *   pms_read [/dev/ttyS1]
 */
#include "aqm/aqi.h"
#include "aqm/pms5003.h"
#include "aqm/sig.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    const char *dev = argc > 1 ? argv[1] : "/dev/ttyS1";
    time_t last_frame = time(NULL);
    pms5003_t p;
    int rc;

    aqm_signals_init();
    if ((rc = pms5003_open(&p, dev)) != 0) {
        fprintf(stderr, "%s: %s\n", dev, strerror(-rc));
        return 1;
    }
    printf("reading PMS5003 on %s (Ctrl+C to stop)\n", dev);
    printf("  time     PM1.0 PM2.5  PM10 ug/m3 | >0.3um >2.5um /0.1L | AQI(PM)\n");

    while (!aqm_should_stop()) {
        rc = pms5003_poll(&p);
        if (rc < 0) {
            fprintf(stderr, "read error: %s\n", strerror(-rc));
            break;
        }
        if (rc > 0) {
            const pms5003_frame_t *f = &p.last;
            int a25 = aqm_aqi_pm25(f->pm25), a10 = aqm_aqi_pm10(f->pm10);
            int aqi = a25 > a10 ? a25 : a10;
            char t[16];
            time_t now = time(NULL);

            strftime(t, sizeof(t), "%H:%M:%S", localtime(&now));
            printf("  %s %5u %5u %5u       | %6u %6u      | %3d %s%s\n",
                   t, f->pm1, f->pm25, f->pm10, f->n03, f->n25, aqi,
                   aqm_aqi_cat_name(aqm_aqi_category(aqi)),
                   f->error ? "  (sensor error flag!)" : "");
            fflush(stdout);
            last_frame = now;
        } else if (time(NULL) - last_frame >= 3) {
            printf("  no frame for %ld s - check wiring / fan\n", (long)(time(NULL) - last_frame));
            last_frame = time(NULL);
        }
        usleep(100000);
    }

    printf("frames ok %u, bad checksum %u, bad length %u, resync bytes %u\n",
           p.frames_ok, p.bad_checksum, p.bad_length, p.resync_bytes);
    pms5003_close(&p);
    return 0;
}
