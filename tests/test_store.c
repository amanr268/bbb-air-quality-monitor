#include "aqm/store.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
    return 1; } } while (0)

#define DB "test_store.db"

static void cleanup(void)
{
    unlink(DB);
    unlink(DB "-wal");
    unlink(DB "-shm");
}

int main(void)
{
    aqm_sample_t s[10];
    aqm_store_t *st;
    int i;

    cleanup();
    memset(s, 0, sizeof(s));
    for (i = 0; i < 10; i++) {
        s[i].seq      = (uint32_t)i;
        s[i].t_wall_s = 1000 + i;            /* t = 1000 .. 1009 */
        s[i].pm25     = (uint16_t)(40 + i);
    }

    st = aqm_store_open(DB);
    CHECK(st != NULL);
    CHECK(aqm_store_count(st) == 0);

    /* one transaction, 10 rows */
    CHECK(aqm_store_add_batch(st, s, 10) == 0);
    CHECK(aqm_store_count(st) == 10);

    /* retention: delete t < 1005 -> 5 rows go */
    CHECK(aqm_store_prune(st, 1005) == 5);
    CHECK(aqm_store_count(st) == 5);
    aqm_store_close(st);

    /* persistence: data survives close + reopen (like a reboot) */
    st = aqm_store_open(DB);
    CHECK(st != NULL);
    CHECK(aqm_store_count(st) == 5);
    aqm_store_close(st);

    cleanup();
    printf("store ok\n");
    return 0;
}
