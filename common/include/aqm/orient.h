#ifndef AQM_ORIENT_H
#define AQM_ORIENT_H

/* Phone-style screen orientation from a gravity vector, with
 *  - a margin: one axis must clearly dominate (no flip-flop at 45 deg)
 *  - a hold time: the new orientation must persist N samples
 *  - flat detection: lying flat keeps the last orientation
 *
 * Orientation values match ssd1306 rotation: 0..3 (90 deg steps). */

#define AQM_ORIENT_MARGIN_G   0.30f   /* |a| must exceed |b| by this     */
#define AQM_ORIENT_FLAT_G     0.80f   /* |z| above this = lying flat     */

typedef struct {
    int current;      /* orientation in use (0..3)          */
    int candidate;    /* orientation being confirmed (-1)   */
    int count;        /* consecutive samples of candidate   */
    int hold;         /* samples needed to switch           */
    int offset;       /* mounting correction, added mod 4   */
} aqm_orient_t;

void aqm_orient_init(aqm_orient_t *o, int hold_samples, int offset);

/* raw orientation for one sample: 0..3, or -1 if flat/ambiguous */
int  aqm_orient_classify(float gx, float gy, float gz);

/* feed one sample; returns 1 if o->current changed */
int  aqm_orient_update(aqm_orient_t *o, float gx, float gy, float gz);

#endif
