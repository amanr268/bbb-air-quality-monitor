#include "aqm/orient.h"

#include <stdio.h>

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
    return 1; } } while (0)

int main(void)
{
    aqm_orient_t o;
    int i;

    /* classification */
    CHECK(aqm_orient_classify(0.0f, -1.0f, 0.0f) == 0);
    CHECK(aqm_orient_classify(1.0f, 0.0f, 0.0f) == 1);
    CHECK(aqm_orient_classify(0.0f, 1.0f, 0.0f) == 2);
    CHECK(aqm_orient_classify(-1.0f, 0.0f, 0.0f) == 3);
    CHECK(aqm_orient_classify(0.0f, 0.0f, 1.0f) == -1);      /* flat         */
    CHECK(aqm_orient_classify(0.70f, -0.70f, 0.0f) == -1);   /* 45 deg       */

    /* hold time: needs 5 consecutive samples to switch */
    aqm_orient_init(&o, 5, 0);
    CHECK(o.current == 0);
    for (i = 0; i < 4; i++)
        CHECK(aqm_orient_update(&o, 1.0f, 0.0f, 0.0f) == 0); /* not yet      */
    CHECK(aqm_orient_update(&o, 1.0f, 0.0f, 0.0f) == 1);     /* 5th: switch  */
    CHECK(o.current == 1);

    /* a short wobble does not switch back */
    for (i = 0; i < 3; i++)
        CHECK(aqm_orient_update(&o, 0.0f, -1.0f, 0.0f) == 0);
    CHECK(aqm_orient_update(&o, 1.0f, 0.0f, 0.0f) == 0);     /* back: reset  */
    CHECK(o.current == 1);

    /* lying flat keeps the last orientation */
    for (i = 0; i < 20; i++)
        CHECK(aqm_orient_update(&o, 0.0f, 0.0f, 1.0f) == 0);
    CHECK(o.current == 1);

    /* mounting offset rotates the result */
    aqm_orient_init(&o, 1, 1);
    CHECK(aqm_orient_update(&o, 0.0f, 1.0f, 0.0f) == 1);     /* raw 2 + 1    */
    CHECK(o.current == 3);

    printf("orient ok\n");
    return 0;
}
