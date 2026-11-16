#!/bin/sh
# Wi-Fi watchdog: on the RT kernel the wl18xx sometimes comes up unable to
# scan (iwd: CMD_TRIGGER_SCAN EINVAL); restarting iwd resets the chip.
ip -4 addr show wlan0 2>/dev/null | grep -q "inet " && exit 0
echo "wlan0 has no IPv4 address - restarting iwd"
systemctl restart iwd
