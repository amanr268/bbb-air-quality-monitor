/*
 * oled_off: switch the SSD1306 panel off (display off + charge pump off).
 * Run by systemd after aqm.service stops for ANY reason (clean stop, crash,
 * kill, watchdog restart), so a powered-down or stopped monitor never keeps
 * showing stale readings. The OLED keeps its supply after the board halts.
 *
 *   oled_off [/dev/i2c-2] [0x3c]
 */
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    const char *dev = argc > 1 ? argv[1] : "/dev/i2c-2";
    int addr = argc > 2 ? (int)strtol(argv[2], NULL, 0) : 0x3C;
    /* control byte 0x00 = command stream: display off, charge pump off */
    static const uint8_t cmd[] = { 0x00, 0xAE, 0x8D, 0x10 };
    int fd = open(dev, O_RDWR);

    if (fd < 0 || ioctl(fd, I2C_SLAVE, addr) < 0) {
        fprintf(stderr, "oled_off: %s: %s\n", dev, strerror(errno));
        return 1;
    }
    if (write(fd, cmd, sizeof(cmd)) != (ssize_t)sizeof(cmd)) {
        fprintf(stderr, "oled_off: no OLED at 0x%02x: %s\n", addr, strerror(errno));
        close(fd);
        return 1;
    }
    close(fd);
    printf("OLED at 0x%02x switched off\n", addr);
    return 0;
}
