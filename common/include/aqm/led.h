#ifndef AQM_LED_H
#define AQM_LED_H

/* LED via the kernel gpio-leds driver: /sys/class/leds/<name>/brightness */

int  aqm_led_open(const char *name);    /* fd, or -errno                 */
int  aqm_led_set(int fd, int on);       /* 0 or -errno                   */
void aqm_led_close(int fd);             /* switches the LED off first    */

#endif
