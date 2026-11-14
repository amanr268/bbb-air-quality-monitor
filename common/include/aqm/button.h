#ifndef AQM_BUTTON_H
#define AQM_BUTTON_H

/*
 * Push button via the kernel gpio-keys driver (DT overlay BB-AQM-00A0):
 * the button is an evdev input device, debounced in the kernel.
 */

/* find the /dev/input/event* device whose name matches (e.g. "aqm-keys")
 * and open it non-blocking. Returns fd, or -errno (-ENODEV: not found). */
int aqm_button_open(const char *name);

/* drain pending events; returns the number of presses (key down) of `code`
 * (e.g. KEY_PROG1), 0 if none, -errno if the device went away. */
int aqm_button_presses(int fd, unsigned code);

void aqm_button_close(int fd);

#endif
