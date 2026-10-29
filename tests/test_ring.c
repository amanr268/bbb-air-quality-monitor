#include "aqm/ring.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
    return 1; } } while (0)

int main(void)
{
    static aqm_ring_t r;
    aqm_sample_t s = {0}, out;
    uint32_t i;

    CHECK(aqm_ring_init(&r) == 0);

    /* FIFO order */
    for (i = 0; i < 3; i++) { s.seq = i; CHECK(aqm_ring_push(&r, &s) == 0); }
    for (i = 0; i < 3; i++) {
        CHECK(aqm_ring_pop(&r, &out, 100) == 0);
        CHECK(out.seq == i);
    }

    /* empty: pop times out */
    CHECK(aqm_ring_pop(&r, &out, 50) == -ETIMEDOUT);

    /* full: push is refused and counted, never blocks */
    for (i = 0; i < AQM_RING_LEN; i++) CHECK(aqm_ring_push(&r, &s) == 0);
    CHECK(aqm_ring_push(&r, &s) == -EAGAIN);
    CHECK(r.dropped == 1);

    printf("ring ok\n");
    return 0;
}
