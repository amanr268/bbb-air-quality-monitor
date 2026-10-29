#include "aqm/sig.h"

#include <signal.h>
#include <string.h>

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;            /* only async-signal-safe work in a handler */
}

void aqm_signals_init(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    /* no SA_RESTART: blocking calls return EINTR so loops notice the stop */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

int aqm_should_stop(void)
{
    return g_stop;
}
