#ifndef AQM_NOTIFY_H
#define AQM_NOTIFY_H

/*
 * Minimal sd_notify(): sends "READY=1", "WATCHDOG=1", "STATUS=..." to systemd
 * over $NOTIFY_SOCKET, without linking libsystemd.
 * Returns 1 if sent, 0 if not running under systemd, -errno on error.
 */
int aqm_notify(const char *msg);

#endif
