#include "aqm/button.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

int aqm_button_open(const char *name)
{
    int i, err = ENODEV;

    for (i = 0; i < 32; i++) {
        char path[32], dev_name[64] = "";
        int fd;

        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            if (errno == EACCES)
                err = EACCES;           /* exists but not ours (yet): report it */
            continue;
        }
        if (ioctl(fd, EVIOCGNAME(sizeof(dev_name) - 1), dev_name) >= 0 &&
            strcmp(dev_name, name) == 0)
            return fd;
        close(fd);
    }
    return -err;
}

int aqm_button_presses(int fd, unsigned code)
{
    struct input_event ev[16];
    int presses = 0;

    for (;;) {
        ssize_t n = read(fd, ev, sizeof(ev));
        size_t i;

        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR)
                return presses;
            return -errno;              /* e.g. ENODEV: device removed */
        }
        if (n == 0)
            return presses;             /* EOF (pipe in tests) */
        for (i = 0; i < (size_t)n / sizeof(ev[0]); i++)
            if (ev[i].type == EV_KEY && ev[i].code == code && ev[i].value == 1)
                presses++;              /* 1 = down, 0 = up, 2 = autorepeat */
    }
}

void aqm_button_close(int fd)
{
    if (fd >= 0)
        close(fd);
}
