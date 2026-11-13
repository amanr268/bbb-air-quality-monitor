#include "aqm/orient.h"

#include <math.h>

void aqm_orient_init(aqm_orient_t *o, int hold_samples, int offset)
{
    o->current   = (offset & 3);
    o->candidate = -1;
    o->count     = 0;
    o->hold      = hold_samples > 0 ? hold_samples : 1;
    o->offset    = offset & 3;
}

int aqm_orient_classify(float gx, float gy, float gz)
{
    float ax = fabsf(gx), ay = fabsf(gy);

    if (fabsf(gz) > AQM_ORIENT_FLAT_G)
        return -1;                                   /* lying flat        */
    if (ay > ax + AQM_ORIENT_MARGIN_G)
        return gy < 0.0f ? 0 : 2;                    /* Y axis dominates  */
    if (ax > ay + AQM_ORIENT_MARGIN_G)
        return gx > 0.0f ? 1 : 3;                    /* X axis dominates  */
    return -1;                                       /* ~45 deg: ambiguous */
}

int aqm_orient_update(aqm_orient_t *o, float gx, float gy, float gz)
{
    int raw = aqm_orient_classify(gx, gy, gz);
    int want;

    if (raw < 0) {                       /* flat or ambiguous: keep current */
        o->candidate = -1;
        o->count = 0;
        return 0;
    }
    want = (raw + o->offset) & 3;

    if (want == o->current) {
        o->candidate = -1;
        o->count = 0;
        return 0;
    }
    if (want != o->candidate) {          /* new candidate: start counting */
        o->candidate = want;
        o->count = 1;
    } else {
        o->count++;
    }
    if (o->count >= o->hold) {
        o->current = want;
        o->candidate = -1;
        o->count = 0;
        return 1;
    }
    return 0;
}
