#include "aqm/orient.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define PI_F 3.14159265f

static float dot(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static float norm(const float a[3])
{
    return sqrtf(dot(a, a));
}

/* component of v perpendicular to unit vector n */
static void in_plane(const float v[3], const float n[3], float out[3])
{
    float d = dot(v, n);
    int i;

    for (i = 0; i < 3; i++)
        out[i] = v[i] - d * n[i];
}

static int unit(float v[3])
{
    float m = norm(v);
    int i;

    if (m < 1e-6f)
        return -1;
    for (i = 0; i < 3; i++)
        v[i] /= m;
    return 0;
}

int aqm_orient_cal_compute(aqm_orient_cal_t *c, const float flat[3],
                           const float land[3], const float port[3],
                           char *err, size_t errlen)
{
    float lp[3], pp[3], ang;

    memset(c, 0, sizeof(*c));
    memcpy(c->normal, flat, sizeof(c->normal));
    if (norm(flat) < 0.7f || unit(c->normal)) {
        snprintf(err, errlen, "flat reading too small (|g| %.2f) - sensor problem?", norm(flat));
        return -1;
    }

    in_plane(land, c->normal, lp);
    in_plane(port, c->normal, pp);
    if (norm(lp) < AQM_ORIENT_MIN_TILT_G) {
        snprintf(err, errlen, "landscape pose not tilted enough - hold it more upright");
        return -1;
    }
    if (norm(pp) < AQM_ORIENT_MIN_TILT_G) {
        snprintf(err, errlen, "portrait pose not tilted enough - hold it more upright");
        return -1;
    }
    unit(lp);
    unit(pp);

    /* the two upright poses must be roughly a quarter turn apart */
    ang = acosf(fmaxf(-1.0f, fminf(1.0f, dot(lp, pp)))) * 180.0f / PI_F;
    if (ang < 60.0f || ang > 120.0f) {
        snprintf(err, errlen, "landscape and portrait poses are %.0f deg apart "
                 "(expected ~90) - redo both poses", ang);
        return -1;
    }

    memcpy(c->land, lp, sizeof(lp));
    memcpy(c->port, pp, sizeof(pp));
    c->landscape_rot = 0;
    c->portrait_rot  = 1;
    c->portrait2_rot = 3;
    return 0;
}

int aqm_orient_cal_save(const aqm_orient_cal_t *c, const char *path)
{
    FILE *f = fopen(path, "w");

    if (!f)
        return -errno;
    fprintf(f, "# ADXL345 orientation calibration (written by accel_cal)\n");
    fprintf(f, "normal = %.5f %.5f %.5f\n", c->normal[0], c->normal[1], c->normal[2]);
    fprintf(f, "landscape = %.5f %.5f %.5f\n", c->land[0], c->land[1], c->land[2]);
    fprintf(f, "portrait = %.5f %.5f %.5f\n", c->port[0], c->port[1], c->port[2]);
    fprintf(f, "# display rotation per state (0..3, 90 deg steps)\n");
    fprintf(f, "landscape_rot = %d\n", c->landscape_rot);
    fprintf(f, "portrait_rot = %d\n", c->portrait_rot);
    fprintf(f, "portrait2_rot = %d\n", c->portrait2_rot);
    fclose(f);
    return 0;
}

int aqm_orient_cal_load(aqm_orient_cal_t *c, const char *path)
{
    char line[160], key[32];
    float v[3];
    int got = 0, r;
    FILE *f = fopen(path, "r");

    if (!f)
        return -errno;
    memset(c, 0, sizeof(*c));
    c->portrait_rot  = 1;
    c->portrait2_rot = -1;                 /* default: portrait_rot + 2 */
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, " %31[a-z_] = %f %f %f", key, &v[0], &v[1], &v[2]) == 4) {
            if      (!strcmp(key, "normal"))    { memcpy(c->normal, v, sizeof(v)); got |= 1; }
            else if (!strcmp(key, "landscape")) { memcpy(c->land, v, sizeof(v));   got |= 2; }
            else if (!strcmp(key, "portrait"))  { memcpy(c->port, v, sizeof(v));   got |= 4; }
        } else if (sscanf(line, " %31[a-z_] = %d", key, &r) == 2) {
            if      (!strcmp(key, "landscape_rot")) c->landscape_rot = r & 3;
            else if (!strcmp(key, "portrait_rot"))  c->portrait_rot  = r & 3;
            else if (!strcmp(key, "portrait2_rot")) c->portrait2_rot = r & 3;
        }
    }
    fclose(f);
    if (c->portrait2_rot < 0)
        c->portrait2_rot = (c->portrait_rot + 2) & 3;
    return got == 7 ? 0 : -EINVAL;
}

void aqm_orient_init(aqm_orient_t *o, const aqm_orient_cal_t *c, int hold_samples)
{
    o->cal = *c;
    o->state = AQM_ORIENT_LANDSCAPE;   /* start in landscape */
    o->candidate = -1;
    o->count = 0;
    o->hold = hold_samples > 0 ? hold_samples : 1;
}

float aqm_orient_angle(const aqm_orient_t *o, float gx, float gy, float gz)
{
    float g[3] = { gx, gy, gz }, vp[3], m, c;

    in_plane(g, o->cal.normal, vp);
    m = norm(vp);
    if (m < AQM_ORIENT_MIN_TILT_G)
        return -1.0f;                  /* too flat: rotation not observable */
    c = dot(vp, o->cal.port) / m;
    return acosf(fmaxf(-1.0f, fminf(1.0f, c))) * 180.0f / PI_F;
}

/* which state this angle asks for, given the current state (hysteresis) */
static int wanted_state(int cur, float ang)
{
    const float enter = AQM_ORIENT_ENTER_DEG, exit_ = AQM_ORIENT_EXIT_DEG;

    if (cur == AQM_ORIENT_PORTRAIT && ang <= exit_)
        return AQM_ORIENT_PORTRAIT;                 /* stay: within 45 deg   */
    if (cur == AQM_ORIENT_PORTRAIT_FLIP && ang >= 180.0f - exit_)
        return AQM_ORIENT_PORTRAIT_FLIP;            /* stay: within 45 deg   */
    if (ang <= enter)
        return AQM_ORIENT_PORTRAIT;                 /* enter: within 30 deg  */
    if (ang >= 180.0f - enter)
        return AQM_ORIENT_PORTRAIT_FLIP;            /* enter: within 30 deg  */
    return AQM_ORIENT_LANDSCAPE;
}

int aqm_orient_update(aqm_orient_t *o, float gx, float gy, float gz)
{
    float ang = aqm_orient_angle(o, gx, gy, gz);
    int want;

    if (ang < 0.0f) {                  /* flat: keep current state */
        o->candidate = -1;
        o->count = 0;
        return 0;
    }
    want = wanted_state(o->state, ang);

    if (want == o->state) {
        o->candidate = -1;
        o->count = 0;
        return 0;
    }
    if (want != o->candidate) {        /* new candidate: start counting */
        o->candidate = want;
        o->count = 1;
    } else {
        o->count++;
    }
    if (o->count >= o->hold) {
        o->state = want;
        o->candidate = -1;
        o->count = 0;
        return 1;
    }
    return 0;
}

int aqm_orient_rotation(const aqm_orient_t *o)
{
    switch (o->state) {
    case AQM_ORIENT_PORTRAIT:      return o->cal.portrait_rot;
    case AQM_ORIENT_PORTRAIT_FLIP: return o->cal.portrait2_rot;
    default:                       return o->cal.landscape_rot;
    }
}

const char *aqm_orient_name(int state)
{
    switch (state) {
    case AQM_ORIENT_PORTRAIT:      return "PORTRAIT";
    case AQM_ORIENT_PORTRAIT_FLIP: return "PORTRAIT (180)";
    default:                       return "LANDSCAPE";
    }
}
