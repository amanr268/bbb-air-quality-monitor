#include "aqm/led.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int aqm_led_open(const char *name)
{
    char path[96];
    int fd;

    snprintf(path, sizeof(path), "/sys/class/leds/%s/brightness", name);
    fd = open(path, O_WRONLY | O_CLOEXEC);
    return fd < 0 ? -errno : fd;
}

int aqm_led_set(int fd, int on)
{
    /* sysfs attribute: one write from offset 0 sets the value */
    if (pwrite(fd, on ? "1" : "0", 1, 0) != 1)
        return -errno;
    return 0;
}

void aqm_led_close(int fd)
{
    if (fd >= 0) {
        aqm_led_set(fd, 0);
        close(fd);
    }
}
