#include "aqm/sig.h"

#include <signal.h>
#include <string.h>

static volatile sig_atomic_t g_stop;
static volatile sig_atomic_t g_reload;

static void on_signal(int sig)
{
    /* only async-signal-safe work in a handler: set a flag */
    if (sig == SIGHUP)
        g_reload = 1;
    else
        g_stop = 1;
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
    sigaction(SIGHUP, &sa, NULL);
}

int aqm_should_stop(void)
{
    return g_stop;
}

int aqm_reload_requested(void)
{
    if (g_reload) {
        g_reload = 0;
        return 1;
    }
    return 0;
}
