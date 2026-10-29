#ifndef AQM_SIG_H
#define AQM_SIG_H

void aqm_signals_init(void);   /* SIGINT/SIGTERM -> stop flag */
int  aqm_should_stop(void);

#endif
