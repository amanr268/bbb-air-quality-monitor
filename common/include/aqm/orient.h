#ifndef AQM_ORIENT_H
#define AQM_ORIENT_H

#include <stddef.h>

/*
 * Screen orientation from the ADXL345, based on a one-time calibration of
 * three poses:
 *   flat      : device on the table, screen up
 *   landscape : held upright in the normal (landscape) position
 *   portrait  : held upright in the portrait position
 *
 * States:
 *   LANDSCAPE      every pose that is not one of the two below
 *   PORTRAIT       within ENTER_DEG of the calibrated portrait pose
 *   PORTRAIT_FLIP  within ENTER_DEG of the portrait pose turned 180 deg
 *
 * Rules:
 *   - a portrait state is entered within ENTER_DEG and left beyond EXIT_DEG
 *     (hysteresis, no flicker at the boundary)
 *   - tilted less than ~30 deg from flat: nothing changes
 *     (an accelerometer cannot sense rotation while lying flat)
 *   - a change must persist `hold` samples before it is applied
 */

#define AQM_ORIENT_ENTER_DEG     30.0f
#define AQM_ORIENT_EXIT_DEG      45.0f
#define AQM_ORIENT_MIN_TILT_G    0.50f    /* in-plane gravity, ~30 deg tilt */

enum {
    AQM_ORIENT_LANDSCAPE = 0,
    AQM_ORIENT_PORTRAIT,
    AQM_ORIENT_PORTRAIT_FLIP,
};

typedef struct {
    float normal[3];       /* unit gravity direction when lying flat          */
    float land[3];         /* unit in-plane gravity direction, landscape pose  */
    float port[3];         /* unit in-plane gravity direction, portrait pose   */
    int   landscape_rot;   /* ssd1306 rotation in landscape            (0/2)  */
    int   portrait_rot;    /* ssd1306 rotation in portrait             (1/3)  */
    int   portrait2_rot;   /* ssd1306 rotation in portrait turned 180  (3/1)  */
} aqm_orient_cal_t;

typedef struct {
    aqm_orient_cal_t cal;
    int state;             /* AQM_ORIENT_*                              */
    int candidate;         /* state being confirmed, -1 = none          */
    int count;             /* consecutive samples of candidate          */
    int hold;
} aqm_orient_t;

/* build a calibration from three averaged readings (in g);
 * returns 0, or -1 with a human-readable reason in err */
int  aqm_orient_cal_compute(aqm_orient_cal_t *c, const float flat[3],
                            const float land[3], const float port[3],
                            char *err, size_t errlen);
int  aqm_orient_cal_save(const aqm_orient_cal_t *c, const char *path);   /* 0 / -errno */
int  aqm_orient_cal_load(aqm_orient_cal_t *c, const char *path);         /* 0 / -errno */

void aqm_orient_init(aqm_orient_t *o, const aqm_orient_cal_t *c, int hold_samples);

/* angle (deg, 0..180) between the current pose and the portrait pose:
 * ~0 = portrait, ~180 = portrait turned 180, -1 = too flat to tell */
float aqm_orient_angle(const aqm_orient_t *o, float gx, float gy, float gz);

/* feed one sample; returns 1 if the state changed */
int  aqm_orient_update(aqm_orient_t *o, float gx, float gy, float gz);

/* ssd1306 rotation for the current state */
int  aqm_orient_rotation(const aqm_orient_t *o);

const char *aqm_orient_name(int state);

#endif
