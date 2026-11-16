#!/bin/sh
# Wi-Fi watchdog, run by wifi-check.timer every 60 s (first run 75 s after boot).
#
#   OK      wlan0 has an IPv4 address AND the router answers a ping
#   not OK  restart iwd (this also resets the wl18xx chip; on the RT kernel it
#           sometimes comes up unable to scan: CMD_TRIGGER_SCAN EINVAL)
#   5 failed checks in a row: reboot the board, but at most once every 6 h,
#   so a router that is simply switched off cannot cause a reboot loop.
#   The sensing pipeline keeps running and buffering meanwhile.

FAILS=/run/wifi-check.fails             # tmpfs: starts at 0 after every boot
STAMP=/var/lib/wifi-check/last-reboot   # survives reboots
MAX_FAILS=5
REBOOT_GAP=21600                        # 6 h

gw=$(ip -4 route show default dev wlan0 2>/dev/null | awk '{ print $3; exit }')

if ip -4 addr show wlan0 2>/dev/null | grep -q "inet " &&
   [ -n "$gw" ] && ping -c 2 -W 2 -I wlan0 "$gw" >/dev/null 2>&1; then
    if [ -f "$FAILS" ]; then
        echo "Wi-Fi OK again (router $gw answers)"
        rm -f "$FAILS"
    fi
    exit 0
fi

n=$(( $(cat "$FAILS" 2>/dev/null || echo 0) + 1 ))
echo "$n" > "$FAILS"
addr=$(ip -4 -br addr show wlan0 2>/dev/null | awk '{ print $3 }')
echo "Wi-Fi check failed ($n/$MAX_FAILS): address=${addr:-none} router=${gw:-none}"

if [ "$n" -ge "$MAX_FAILS" ]; then
    now=$(date +%s)
    last=$(cat "$STAMP" 2>/dev/null || echo 0)
    if [ $(( now - last )) -ge "$REBOOT_GAP" ]; then
        mkdir -p "$(dirname "$STAMP")"
        echo "$now" > "$STAMP"
        echo "Wi-Fi still down after $n checks - rebooting the board"
        systemctl reboot
        exit 0
    fi
    echo "reboot skipped: last watchdog reboot was less than 6 h ago"
fi

echo "restarting iwd"
systemctl restart iwd
