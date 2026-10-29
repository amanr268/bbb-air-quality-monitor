#include "aqm/shm.h"

#include <stdio.h>

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
    return 1; } } while (0)

int main(void)
{
    aqm_shm_t *owner, *peer;
    aqm_sample_t s = { .seq = 42 }, out;

    owner = aqm_shm_create();
    CHECK(owner != NULL);
    peer = aqm_shm_attach();           /* second, independent mapping */
    CHECK(peer != NULL);
    CHECK(peer != owner);

    CHECK(aqm_ring_push(&owner->ring[AQM_RING_SENSE_CAPTURE], &s) == 0);
    CHECK(aqm_ring_pop(&peer->ring[AQM_RING_SENSE_CAPTURE], &out, 100) == 0);
    CHECK(out.seq == 42);

    CHECK(aqm_running(peer) == 1);
    aqm_stop(owner);
    CHECK(aqm_running(peer) == 0);

    aqm_shm_detach(peer);
    aqm_shm_detach(owner);
    CHECK(aqm_shm_destroy() == 0);

    printf("shm ok\n");
    return 0;
}
