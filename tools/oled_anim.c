/*
 * oled_anim: animations on the SSD1306 OLED.
 *
 *   oled_anim cube                 rotating 3D wireframe cube
 *   oled_anim play FILE.bin [fps]  play frames made by tools/gif2oled.py
 *
 * Options: -d /dev/i2c-2  -a 0x3c  -p DIR (write PBM images instead of using the OLED)
 * Ctrl+C clears the screen and turns it off.
 */
#include "aqm/ssd1306.h"
#include "aqm/sig.h"
#include "aqm/timeutil.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char *pbm_dir;      /* if set: dump frames to files, no hardware */
static int frame_no;

/* write the framebuffer as a PBM image (for testing without the display) */
static int dump_pbm(const ssd1306_t *d)
{
    char path[512];
    FILE *f;
    int x, y;

    snprintf(path, sizeof(path), "%s/frame_%04d.pbm", pbm_dir, frame_no);
    f = fopen(path, "w");
    if (!f)
        return -errno;
    fprintf(f, "P1\n%d %d\n", SSD1306_W, SSD1306_H);
    for (y = 0; y < SSD1306_H; y++) {
        for (x = 0; x < SSD1306_W; x++)
            fputc((d->fb[(y / 8) * SSD1306_W + x] >> (y % 8)) & 1 ? '1' : '0', f);
        fputc('\n', f);
    }
    fclose(f);
    return 0;
}

static int show(ssd1306_t *d)
{
    frame_no++;
    return pbm_dir ? dump_pbm(d) : ssd1306_flush(d);
}

/* ------------------------------- cube ------------------------------------ */

static void draw_cube(ssd1306_t *d, double ax, double ay, double az)
{
    static const double V[8][3] = {
        {-1,-1,-1}, { 1,-1,-1}, { 1, 1,-1}, {-1, 1,-1},
        {-1,-1, 1}, { 1,-1, 1}, { 1, 1, 1}, {-1, 1, 1},
    };
    static const int E[12][2] = {
        {0,1},{1,2},{2,3},{3,0}, {4,5},{5,6},{6,7},{7,4}, {0,4},{1,5},{2,6},{3,7},
    };
    int px[8], py[8], i;

    for (i = 0; i < 8; i++) {
        double x = V[i][0], y = V[i][1], z = V[i][2], t;

        t = y * cos(ax) - z * sin(ax); z = y * sin(ax) + z * cos(ax); y = t;   /* X */
        t = x * cos(ay) + z * sin(ay); z = -x * sin(ay) + z * cos(ay); x = t;  /* Y */
        t = x * cos(az) - y * sin(az); y = x * sin(az) + y * cos(az); x = t;   /* Z */

        z += 5.0;                                   /* push away from camera */
        px[i] = (int)lround(64.0 + x * 56.0 / z);   /* perspective projection */
        py[i] = (int)lround(32.0 + y * 56.0 / z);
    }
    for (i = 0; i < 12; i++)
        ssd1306_line(d, px[E[i][0]], py[E[i][0]], px[E[i][1]], py[E[i][1]]);
}

/* ------------------------------ helpers ---------------------------------- */

static void pace(struct timespec *next, int fps)
{
    ts_add_ns(next, 1000000000LL / fps);
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, next, NULL);
}

static void report_fps(int64_t *t_last, int *frames)
{
    int64_t now = mono_ns();

    if (now - *t_last >= 2 * NSEC_PER_SEC) {
        fprintf(stderr, "%.1f fps\n", *frames * 1e9 / (double)(now - *t_last));
        *t_last = now;
        *frames = 0;
    }
}

static int run_cube(ssd1306_t *d, int max_frames)
{
    struct timespec next;
    double a = 0.0;
    int64_t t_last = mono_ns();
    int frames = 0, rc;

    clock_gettime(CLOCK_MONOTONIC, &next);
    while (!aqm_should_stop() && (max_frames <= 0 || frame_no < max_frames)) {
        ssd1306_clear(d);
        draw_cube(d, a * 0.9, a * 1.3, a * 0.5);
        if ((rc = show(d)) != 0)
            return rc;
        a += 0.06;
        frames++;
        report_fps(&t_last, &frames);
        if (!pbm_dir)
            pace(&next, 30);                 /* target 30 fps; I2C may cap it lower */
    }
    return 0;
}

static int run_play(ssd1306_t *d, const char *file, int fps, int max_frames)
{
    struct timespec next;
    int64_t t_last = mono_ns();
    int frames = 0, rc;
    FILE *f = fopen(file, "rb");

    if (!f)
        return -errno;
    clock_gettime(CLOCK_MONOTONIC, &next);
    while (!aqm_should_stop() && (max_frames <= 0 || frame_no < max_frames)) {
        if (fread(d->fb, 1, SSD1306_FB, f) != SSD1306_FB) {   /* end: loop */
            if (ftell(f) < SSD1306_FB) {
                fclose(f);
                return -EINVAL;                                /* empty file */
            }
            rewind(f);
            continue;
        }
        if ((rc = show(d)) != 0) {
            fclose(f);
            return rc;
        }
        frames++;
        report_fps(&t_last, &frames);
        if (!pbm_dir)
            pace(&next, fps);
    }
    fclose(f);
    return 0;
}

/* ----------------------------- text screens ------------------------------ */

/* word-wrapped message in the 6x10 font */
static void draw_message(ssd1306_t *d, const char *msg)
{
    const aqm_font_t *f = &font_6_10;
    int cols = ssd1306_width(d) / f->w, x = 0, y = 0;
    char word[64];

    while (*msg && y + f->h <= ssd1306_height(d)) {
        int n = 0;

        while (*msg == ' ')
            msg++;
        while (*msg && *msg != ' ' && n < (int)sizeof(word) - 1)
            word[n++] = *msg++;
        word[n] = '\0';
        if (n == 0)
            break;
        if (x > 0 && x / f->w + n > cols) {      /* wrap */
            x = 0;
            y += f->h + 1;
        }
        x = ssd1306_text(d, x, y, word, f) + f->w;
    }
}

/* sample air-quality screen; layout adapts to landscape / portrait */
static void draw_aqi(ssd1306_t *d)
{
    const aqm_font_t *s = &font_6_10, *big = &font_10_20;
    int w = ssd1306_width(d);
    char buf[32];

    if (w > ssd1306_height(d)) {
        /* landscape 128 x 64 */
        ssd1306_text(d, 0, 0, "AQI", s);
        ssd1306_text(d, w - ssd1306_text_width("12:45", s), 0, "12:45", s);
        ssd1306_text(d, 0, 11, "87", big);
        ssd1306_text(d, 30, 17, "Satisfactory", s);
        ssd1306_line(d, 0, 33, w - 1, 33);
        snprintf(buf, sizeof(buf), "PM2.5 %3d PM10 %3d", 45, 72);
        ssd1306_text(d, 0, 37, buf, s);
        snprintf(buf, sizeof(buf), "TVOC %3d  %4.1fC %2d%%", 150, 28.1, 55);
        ssd1306_text(d, 0, 51, buf, s);
    } else {
        /* portrait 64 x 128 */
        ssd1306_text(d, (w - ssd1306_text_width("AQI", s)) / 2, 0, "AQI", s);
        ssd1306_text(d, (w - ssd1306_text_width("87", big)) / 2, 12, "87", big);
        ssd1306_text(d, (w - ssd1306_text_width("Satisf.", s)) / 2, 34, "Satisf.", s);
        ssd1306_line(d, 0, 46, w - 1, 46);
        snprintf(buf, sizeof(buf), "PM2.5 %3d", 45);  ssd1306_text(d, 0, 50, buf, s);
        snprintf(buf, sizeof(buf), "PM10  %3d", 72);  ssd1306_text(d, 0, 61, buf, s);
        snprintf(buf, sizeof(buf), "TVOC  %3d", 150); ssd1306_text(d, 0, 72, buf, s);
        snprintf(buf, sizeof(buf), "T  %4.1fC", 28.1); ssd1306_text(d, 0, 83, buf, s);
        snprintf(buf, sizeof(buf), "RH    %2d%%", 55); ssd1306_text(d, 0, 94, buf, s);
        ssd1306_line(d, 0, 107, w - 1, 107);
        ssd1306_text(d, (w - ssd1306_text_width("12:45", s)) / 2, 112, "12:45", s);
    }
}

/* show one static screen; on hardware keep it until Ctrl+C */
static int run_static(ssd1306_t *d)
{
    int rc = show(d);

    if (rc || pbm_dir)
        return rc;
    fprintf(stderr, "showing; Ctrl+C to clear and exit\n");
    while (!aqm_should_stop())
        pause();
    return 0;
}

/* -------------------------------- main ----------------------------------- */

static void usage(const char *p)
{
    fprintf(stderr,
            "usage: %s [-d /dev/i2c-2] [-a 0x3c] [-r rot 0-3] [-p pbm_dir] [-n frames] cube\n"
            "       %s [options] play FILE.bin [fps]\n"
            "       %s [options] text \"message\"\n"
            "       %s [options] aqi        (sample AQI screen; -r 1 for portrait)\n",
            p, p, p, p);
}

int main(int argc, char **argv)
{
    const char *dev = "/dev/i2c-2";
    unsigned addr = 0x3c;
    int opt, rc = 0, max_frames = 0, rot = 0;
    ssd1306_t d;

    while ((opt = getopt(argc, argv, "d:a:p:n:r:")) != -1) {
        switch (opt) {
        case 'd': dev = optarg; break;
        case 'a': addr = (unsigned)strtoul(optarg, NULL, 0); break;
        case 'r': rot = atoi(optarg); break;
        case 'p': pbm_dir = optarg; break;
        case 'n': max_frames = atoi(optarg); break;
        default: usage(argv[0]); return 2;
        }
    }
    if (optind >= argc) {
        usage(argv[0]);
        return 2;
    }

    aqm_signals_init();

    if (pbm_dir) {
        memset(&d, 0, sizeof(d));
        d.fd = -1;
    } else if ((rc = ssd1306_open(&d, dev, (uint8_t)addr)) != 0) {
        fprintf(stderr, "ssd1306_open(%s, 0x%02x): %s\n", dev, addr, strerror(-rc));
        return 1;
    }

    ssd1306_set_rotation(&d, rot);

    if (!strcmp(argv[optind], "cube")) {
        rc = run_cube(&d, max_frames);
    } else if (!strcmp(argv[optind], "text") && optind + 1 < argc) {
        ssd1306_clear(&d);
        draw_message(&d, argv[optind + 1]);
        rc = run_static(&d);
    } else if (!strcmp(argv[optind], "aqi")) {
        ssd1306_clear(&d);
        draw_aqi(&d);
        rc = run_static(&d);
    } else if (!strcmp(argv[optind], "play") && optind + 1 < argc) {
        int fps = optind + 2 < argc ? atoi(argv[optind + 2]) : 10;
        rc = run_play(&d, argv[optind + 1], fps > 0 ? fps : 10, max_frames);
    } else {
        usage(argv[0]);
        rc = -EINVAL;
    }

    if (rc)
        fprintf(stderr, "error: %s\n", strerror(-rc));
    if (!pbm_dir)
        ssd1306_close(&d);
    return rc ? 1 : 0;
}
