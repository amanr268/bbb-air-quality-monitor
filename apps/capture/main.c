#include "aqm/log.h"
#include "aqm/proc.h"
#include "aqm/shm.h"
#include "aqm/sig.h"
#include "aqm/store.h"
#include "aqm/timeutil.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define DB_BATCH        10            /* rows per transaction (~10 s of data) */
#define RETENTION_S     (48 * 3600)   /* keep 48 hours                         */
#define HOUSEKEEP_S     60            /* prune + stats every minute            */

/* capture (RT) -> writer (SCHED_OTHER): private in-process queue */
static aqm_ring_t   g_dbq;
static aqm_store_t *g_store;
static int          g_writer_stop;

static const char *db_path(void)
{
    const char *p = getenv("AQM_DB");

    if (p)
        return p;
    mkdir("data", 0775);              /* default location; EEXIST is fine */
    return "data/aqm.db";
}

/* Low-priority thread: all SD-card I/O happens here, never in the RT loop. */
static void *writer_fn(void *arg)
{
    aqm_sample_t batch[DB_BATCH];
    int64_t last_house = 0, last_write_us = 0, rows_written = 0;
    sigset_t block;
    int n = 0;

    (void)arg;
    /* signals go to the main thread only */
    sigemptyset(&block);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGTERM);
    sigaddset(&block, SIGHUP);
    pthread_sigmask(SIG_BLOCK, &block, NULL);

    for (;;) {
        aqm_sample_t s;
        int rc = aqm_ring_pop(&g_dbq, &s, 1000);
        int stop = __atomic_load_n(&g_writer_stop, __ATOMIC_ACQUIRE);
        int64_t now;

        if (rc == 0)
            batch[n++] = s;

        /* flush when the batch is full, the queue went idle, or we are stopping */
        if (n > 0 && (n == DB_BATCH || rc != 0 || stop)) {
            int64_t t0 = mono_ns();

            if (aqm_store_add_batch(g_store, batch, n) != 0) {
                aqm_log("capture", "db write failed (%d rows lost): %s",
                        n, aqm_store_errmsg(g_store));
            } else {
                last_write_us = (mono_ns() - t0) / 1000;
                rows_written += n;
            }
            n = 0;
        }

        now = (int64_t)time(NULL);
        if (now - last_house >= HOUSEKEEP_S) {
            int del = aqm_store_prune(g_store, now - RETENTION_S);

            aqm_log("capture", "db: %lld rows stored, %lld written this run, "
                    "last batch %lld us, pruned %d, queue drops %llu",
                    (long long)aqm_store_count(g_store), (long long)rows_written,
                    (long long)last_write_us, del,
                    (unsigned long long)g_dbq.dropped);
            last_house = now;
        }

        if (stop && rc != 0)          /* stop requested and queue drained */
            break;
    }
    return NULL;
}

static int start_writer(pthread_t *t)
{
    struct sched_param sp = { .sched_priority = 0 };
    pthread_attr_t at;
    int rc;

    if (aqm_ring_init(&g_dbq) != 0)
        return -1;

    /* explicitly SCHED_OTHER: do not inherit capture's SCHED_FIFO 70 */
    pthread_attr_init(&at);
    pthread_attr_setinheritsched(&at, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&at, SCHED_OTHER);
    pthread_attr_setschedparam(&at, &sp);
    rc = pthread_create(t, &at, writer_fn, NULL);
    pthread_attr_destroy(&at);
    return rc ? -1 : 0;
}

int main(int argc, char **argv)
{
    int prio = aqm_parse_prio(argc, argv, 70);
    pthread_t writer;
    int have_writer = 0;
    aqm_shm_t *shm;

    aqm_signals_init();
    shm = aqm_attach_wait("capture");
    if (!shm)
        return 0;
    aqm_rt_setup("capture", prio);

    /* storage is optional: if it fails, the live pipeline still runs */
    g_store = aqm_store_open(db_path());
    if (!g_store) {
        aqm_log("capture", "db open failed (%s): running WITHOUT offline buffer", db_path());
    } else if (start_writer(&writer) != 0) {
        aqm_log("capture", "writer thread failed: running WITHOUT offline buffer");
        aqm_store_close(g_store);
        g_store = NULL;
    } else {
        have_writer = 1;
        aqm_log("capture", "offline buffer: %s (batch %d, retention %d h)",
                db_path(), DB_BATCH, RETENTION_S / 3600);
    }

    while (!aqm_should_stop() && aqm_running(shm)) {
        aqm_sample_t s;
        int rc = aqm_ring_pop(&shm->ring[AQM_RING_SENSE_CAPTURE], &s, 500);

        if (rc == -ETIMEDOUT || rc == -EINTR)
            continue;                 /* re-check stop flags */
        if (rc) {
            aqm_log("capture", "pop failed: %s", strerror(-rc));
            break;
        }

        aqm_log("capture", "seq %u pm2.5 %u tvoc %u status 0x%x | ipc latency %lld us",
                s.seq, s.pm25, s.tvoc_ppb, s.status,
                (long long)((mono_ns() - s.t_mono_ns) / 1000));

        /* live path first, storage second: the pipeline never waits for the disk */
        if (aqm_ring_push(&shm->ring[AQM_RING_CAPTURE_CONVERT], &s) == -EAGAIN)
            aqm_log("capture", "ring full, dropped seq %u", s.seq);
        if (have_writer && aqm_ring_push(&g_dbq, &s) == -EAGAIN)
            aqm_log("capture", "db queue full, seq %u not stored", s.seq);
    }

    aqm_log("capture", "stopping");
    if (have_writer) {
        __atomic_store_n(&g_writer_stop, 1, __ATOMIC_RELEASE);
        pthread_join(writer, NULL);   /* flushes the last partial batch */
        aqm_log("capture", "db closed: %lld rows stored",
                (long long)aqm_store_count(g_store));
        aqm_store_close(g_store);
    }
    aqm_shm_detach(shm);
    return 0;
}
