#ifndef AQM_SIG_H
#define AQM_SIG_H

void aqm_signals_init(void);    /* SIGINT/SIGTERM -> stop, SIGHUP -> reload */
int  aqm_should_stop(void);
int  aqm_reload_requested(void); /* returns 1 once per SIGHUP */

#endif
