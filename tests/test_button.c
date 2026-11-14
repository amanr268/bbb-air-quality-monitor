/* aqm_button_presses() counts key-down events only, using a pipe as a fake evdev */
#include "aqm/button.h"

#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
    return 1; } } while (0)

static void put(int fd, unsigned type, unsigned code, int value)
{
    struct input_event ev;

    memset(&ev, 0, sizeof(ev));
    ev.type = (unsigned short)type;
    ev.code = (unsigned short)code;
    ev.value = value;
    if (write(fd, &ev, sizeof(ev)) != (ssize_t)sizeof(ev))
        perror("write");
}

int main(void)
{
    int p[2];

    CHECK(pipe2(p, O_NONBLOCK) == 0);

    /* nothing pending */
    CHECK(aqm_button_presses(p[0], KEY_PROG1) == 0);

    /* two presses with releases and SYN reports, plus noise */
    put(p[1], EV_KEY, KEY_PROG1, 1); put(p[1], EV_SYN, SYN_REPORT, 0);
    put(p[1], EV_KEY, KEY_PROG1, 0); put(p[1], EV_SYN, SYN_REPORT, 0);
    put(p[1], EV_KEY, KEY_PROG1, 2);                 /* autorepeat: not a press */
    put(p[1], EV_KEY, KEY_A, 1);                     /* other key: ignored      */
    put(p[1], EV_KEY, KEY_PROG1, 1); put(p[1], EV_SYN, SYN_REPORT, 0);
    put(p[1], EV_KEY, KEY_PROG1, 0); put(p[1], EV_SYN, SYN_REPORT, 0);
    CHECK(aqm_button_presses(p[0], KEY_PROG1) == 2);

    /* drained */
    CHECK(aqm_button_presses(p[0], KEY_PROG1) == 0);

    /* more than one read() buffer (16 events) worth */
    for (int i = 0; i < 20; i++) {
        put(p[1], EV_KEY, KEY_PROG1, 1);
        put(p[1], EV_KEY, KEY_PROG1, 0);
    }
    CHECK(aqm_button_presses(p[0], KEY_PROG1) == 20);

    close(p[0]);
    close(p[1]);
    printf("button ok\n");
    return 0;
}
