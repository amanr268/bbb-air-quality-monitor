#include "aqm/orient.h"

#include <stdio.h>

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
    return 1; } } while (0)

/* feed the same reading n times; return how many state changes happened */
static int feed(aqm_orient_t *o, int n, float x, float y, float z)
{
    int i, changes = 0;

    for (i = 0; i < n; i++)
        changes += aqm_orient_update(o, x, y, z);
    return changes;
}

int main(void)
{
    /* synthetic sensor: flat = +Z, landscape upright = gravity on -Y,
     * portrait upright (quarter turn) = gravity on +X,
     * portrait turned 180 = gravity on -X, other quarter turn = gravity on +Y */
    const float flat[3] = { 0.02f, -0.01f, 1.00f };
    const float land[3] = { 0.00f, -0.90f, 0.40f };
    const float port[3] = { 0.90f,  0.00f, 0.40f };
    aqm_orient_cal_t cal, loaded;
    aqm_orient_t o;
    char err[128];

    CHECK(aqm_orient_cal_compute(&cal, flat, land, port, err, sizeof(err)) == 0);

    /* calibration rejects bad input */
    CHECK(aqm_orient_cal_compute(&loaded, flat, land, land, err, sizeof(err)) != 0);  /* same pose  */
    CHECK(aqm_orient_cal_compute(&loaded, flat, flat, port, err, sizeof(err)) != 0);  /* not tilted */

    /* save / load round trip */
    CHECK(aqm_orient_cal_save(&cal, "test_orient.conf") == 0);
    CHECK(aqm_orient_cal_load(&loaded, "test_orient.conf") == 0);
    CHECK(loaded.landscape_rot == 0 && loaded.portrait_rot == 1 && loaded.portrait2_rot == 3);
    remove("test_orient.conf");

    aqm_orient_init(&o, &loaded, 5);
    CHECK(o.state == AQM_ORIENT_LANDSCAPE && aqm_orient_rotation(&o) == 0);

    /* lying flat (any spin) and small tilts: never change */
    CHECK(feed(&o, 50, 0.0f, 0.0f, 1.0f) == 0);
    CHECK(feed(&o, 50, 0.30f, 0.0f, 0.95f) == 0);
    CHECK(feed(&o, 50, -0.30f, 0.0f, 0.95f) == 0);
    /* upright landscape and the other quarter turn: landscape */
    CHECK(feed(&o, 50, 0.0f, -0.9f, 0.4f) == 0);
    CHECK(feed(&o, 50, 0.0f, 0.9f, 0.4f) == 0);
    CHECK(o.state == AQM_ORIENT_LANDSCAPE);

    /* portrait (calibrated pose): switches after the 5-sample hold */
    CHECK(feed(&o, 4, 0.9f, 0.0f, 0.4f) == 0);
    CHECK(feed(&o, 1, 0.9f, 0.0f, 0.4f) == 1);
    CHECK(o.state == AQM_ORIENT_PORTRAIT && aqm_orient_rotation(&o) == 1);

    /* hysteresis: 40 deg away keeps portrait (exit is 45) */
    CHECK(feed(&o, 50, 0.69f, -0.58f, 0.4f) == 0);
    CHECK(o.state == AQM_ORIENT_PORTRAIT);
    /* lying flat keeps portrait */
    CHECK(feed(&o, 50, 0.0f, 0.0f, 1.0f) == 0);
    CHECK(o.state == AQM_ORIENT_PORTRAIT);

    /* portrait turned 180: goes straight to the flipped portrait */
    CHECK(feed(&o, 5, -0.9f, 0.0f, 0.4f) == 1);
    CHECK(o.state == AQM_ORIENT_PORTRAIT_FLIP && aqm_orient_rotation(&o) == 3);
    /* hysteresis on the flipped side: 40 deg away (140 deg) keeps it */
    CHECK(feed(&o, 50, -0.69f, -0.58f, 0.4f) == 0);
    CHECK(o.state == AQM_ORIENT_PORTRAIT_FLIP);

    /* back to landscape upright */
    CHECK(feed(&o, 5, 0.0f, -0.9f, 0.4f) == 1);
    CHECK(o.state == AQM_ORIENT_LANDSCAPE);

    /* entry needs <= 30 deg on either side: 40 deg does NOT enter */
    CHECK(feed(&o, 50, 0.69f, -0.58f, 0.4f) == 0);
    CHECK(feed(&o, 50, -0.69f, -0.58f, 0.4f) == 0);
    CHECK(o.state == AQM_ORIENT_LANDSCAPE);

    /* a short wobble toward portrait (3 samples) does not switch */
    CHECK(feed(&o, 3, 0.9f, 0.0f, 0.4f) == 0);
    CHECK(feed(&o, 1, 0.0f, -0.9f, 0.4f) == 0);
    CHECK(o.state == AQM_ORIENT_LANDSCAPE);

    printf("orient ok\n");
    return 0;
}
