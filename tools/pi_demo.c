/*
 * Priority-inversion demo: the same 3-thread scenario run twice,
 * first with a plain mutex, then with a priority-inheritance (PI) mutex.
 *
 *   LOW    (FIFO 10): locks the mutex, needs 200 ms of CPU to finish
 *   HIGH   (FIFO 30): wakes at 20 ms, needs the same mutex
 *   MEDIUM (FIFO 20): wakes at 40 ms, burns 500 ms of CPU, never touches the mutex
 *
 * HIGH and MEDIUM start their timers only after LOW signals that it holds the
 * mutex, so the scenario does not depend on how long thread creation takes.
 * All threads are pinned to CPU 0 (the BeagleBone has only one core anyway).
 * Without PI, MEDIUM preempts LOW, so HIGH ends up waiting for MEDIUM.
 * With PI, LOW runs at HIGH's priority while holding the lock, so MEDIUM can't cut in.
 */
#include "aqm/timeutil.h"

#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#define PRIO_MAIN   50
#define PRIO_HIGH   30
#define PRIO_MEDIUM 20
#define PRIO_LOW    10

static pthread_mutex_t lock;
static sem_t lock_taken;   /* LOW posts once it holds the mutex */
static int64_t t_start;
static int64_t high_wait_ns;

static double now_ms(void)
{
    return (double)(mono_ns() - t_start) / 1e6;
}

#define LOG(who, ...) do {                         \
        printf("%7.1f ms  %-6s  ", now_ms(), who); \
        printf(__VA_ARGS__);                       \
        putchar('\n');                             \
        fflush(stdout);                            \
    } while (0)

static int64_t thread_cpu_ns(void)
{
    struct timespec t;

    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return ts_to_ns(&t);
}

/* consume ms of THIS thread's CPU time: progress only happens while it runs */
static void burn_cpu_ms(int ms)
{
    int64_t end = thread_cpu_ns() + (int64_t)ms * 1000000LL;

    while (thread_cpu_ns() < end)
        ;
}

static void sleep_until_ms(int ms)
{
    struct timespec ts;
    int64_t ns = t_start + (int64_t)ms * 1000000LL;

    ts.tv_sec  = ns / NSEC_PER_SEC;
    ts.tv_nsec = ns % NSEC_PER_SEC;
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
}

static void *low_fn(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&lock);
    t_start = mono_ns();              /* timeline starts when LOW holds the lock */
    LOG("LOW", "locked mutex, starting 200 ms of work");
    sem_post(&lock_taken);            /* release HIGH ...   */
    sem_post(&lock_taken);            /* ... and MEDIUM     */
    burn_cpu_ms(200);
    LOG("LOW", "work done, unlocking");
    pthread_mutex_unlock(&lock);
    return NULL;
}

static void *high_fn(void *arg)
{
    int64_t t0;

    (void)arg;
    sem_wait(&lock_taken);
    sleep_until_ms(20);
    LOG("HIGH", "wants the mutex");
    t0 = mono_ns();
    pthread_mutex_lock(&lock);
    high_wait_ns = mono_ns() - t0;
    LOG("HIGH", "got the mutex after waiting %.1f ms", (double)high_wait_ns / 1e6);
    pthread_mutex_unlock(&lock);
    return NULL;
}

static void *medium_fn(void *arg)
{
    (void)arg;
    sem_wait(&lock_taken);
    sleep_until_ms(40);
    LOG("MEDIUM", "starts 500 ms of CPU work (never uses the mutex)");
    burn_cpu_ms(500);
    LOG("MEDIUM", "done");
    return NULL;
}

static pthread_t spawn(void *(*fn)(void *), int prio)
{
    struct sched_param sp = { .sched_priority = prio };
    pthread_attr_t at;
    cpu_set_t cpus;
    pthread_t t;
    int rc;

    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 64 * 1024);   /* small stack: fast to lock in RAM */
    pthread_attr_setinheritsched(&at, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&at, SCHED_FIFO);
    pthread_attr_setschedparam(&at, &sp);
    CPU_ZERO(&cpus);
    CPU_SET(0, &cpus);
    pthread_attr_setaffinity_np(&at, sizeof(cpus), &cpus);

    rc = pthread_create(&t, &at, fn, NULL);
    pthread_attr_destroy(&at);
    if (rc) {
        fprintf(stderr, "pthread_create (prio %d): %s\n", prio, strerror(rc));
        exit(1);
    }
    return t;
}

static double run(int use_pi)
{
    pthread_mutexattr_t ma;
    pthread_t lo, me, hi;

    pthread_mutexattr_init(&ma);
    pthread_mutexattr_setprotocol(&ma, use_pi ? PTHREAD_PRIO_INHERIT : PTHREAD_PRIO_NONE);
    pthread_mutex_init(&lock, &ma);
    pthread_mutexattr_destroy(&ma);
    sem_init(&lock_taken, 0, 0);
    high_wait_ns = 0;

    printf("\n=== %s ===\n", use_pi ? "WITH priority inheritance (PTHREAD_PRIO_INHERIT)"
                                    : "WITHOUT priority inheritance (PTHREAD_PRIO_NONE)");

    /* main runs at a higher priority on the same CPU, so all three threads
     * are created before any of them starts */
    lo = spawn(low_fn, PRIO_LOW);
    me = spawn(medium_fn, PRIO_MEDIUM);
    hi = spawn(high_fn, PRIO_HIGH);

    pthread_join(hi, NULL);
    pthread_join(me, NULL);
    pthread_join(lo, NULL);
    pthread_mutex_destroy(&lock);
    sem_destroy(&lock_taken);

    return (double)high_wait_ns / 1e6;
}

int main(void)
{
    struct sched_param sp = { .sched_priority = PRIO_MAIN };
    struct timespec gap = { 0, 500000000L };
    cpu_set_t cpus;
    double no_pi, with_pi;

    CPU_ZERO(&cpus);
    CPU_SET(0, &cpus);
    if (sched_setaffinity(0, sizeof(cpus), &cpus) != 0 ||
        sched_setscheduler(0, SCHED_FIFO, &sp) != 0) {
        perror("SCHED_FIFO setup (check: ulimit -r should be >= 50)");
        return 1;
    }
    mlockall(MCL_CURRENT | MCL_FUTURE);

    printf("Priority-inversion demo: LOW=%d MEDIUM=%d HIGH=%d (SCHED_FIFO, CPU 0)\n",
           PRIO_LOW, PRIO_MEDIUM, PRIO_HIGH);
    printf("Note: the board may feel frozen for ~1 s during each run.\n");

    no_pi = run(0);
    nanosleep(&gap, NULL);
    with_pi = run(1);

    if (no_pi < 100.0) {
        printf("\nScenario not reproduced (HIGH waited only %.1f ms without PI).\n"
               "Run again; if it repeats, report it.\n", no_pi);
        return 1;
    }

    printf("\n=== RESULT: how long HIGH (prio %d) was blocked ===\n", PRIO_HIGH);
    printf("  without PI : %6.1f ms  (LOW's 200 ms work + MEDIUM's 500 ms that cut in)\n", no_pi);
    printf("  with PI    : %6.1f ms  (only LOW's remaining work)\n", with_pi);
    printf("  improvement: %.1fx faster\n", no_pi / (with_pi > 0.0 ? with_pi : 1.0));
    return 0;
}
