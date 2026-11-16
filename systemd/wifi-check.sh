#!/bin/sh
# Wi-Fi watchdog, run by wifi-check.timer every 30 s (first run 30 s after boot).
#
#   OK      wlan0 has an IPv4 address AND the router answers a ping
#   not OK  restart iwd (this also resets the wl18xx chip; on the RT kernel it
#           sometimes comes up unable to scan: CMD_TRIGGER_SCAN EINVAL)
#   First 2 min after boot: only the known wl18xx boot bug is acted on (at once).
#   10 failed checks in a row (5 min): reboot the board, but at most once every 6 h,
#   so a router that is simply switched off cannot cause a reboot loop.
#   The sensing pipeline keeps running and buffering meanwhile.

FAILS=/run/wifi-check.fails             # tmpfs: starts at 0 after every boot
BOOTFIX=/run/wifi-check.bootfix         # boot-time scan bug already handled
STAMP=/var/lib/wifi-check/last-reboot   # survives reboots
MAX_FAILS=10                            # checks run every 30 s: 5 min
REBOOT_GAP=21600                        # 6 h
GRACE=120                               # s after boot: Wi-Fi may still be coming up

gw=$(ip -4 route show default dev wlan0 2>/dev/null | awk '{ print $3; exit }')

if ip -4 addr show wlan0 2>/dev/null | grep -q "inet " &&
   [ -n "$gw" ] && ping -c 2 -W 2 -I wlan0 "$gw" >/dev/null 2>&1; then
    if [ -f "$FAILS" ]; then
        echo "Wi-Fi OK again (router $gw answers)"
        rm -f "$FAILS"
    fi
    exit 0
fi

# first 2 minutes after boot: do not disturb a Wi-Fi that is still connecting,
# but act at once on the known wl18xx boot bug (scans rejected with EINVAL)
uptime_s=$(cut -d. -f1 /proc/uptime)
if [ "$uptime_s" -lt "$GRACE" ]; then
    if [ ! -f "$BOOTFIX" ] &&
       journalctl -b -u iwd --no-pager -q 2>/dev/null | grep -q "CMD_TRIGGER_SCAN: Invalid argument"; then
        : > "$BOOTFIX"
        echo "wl18xx scan bug at boot (iwd: CMD_TRIGGER_SCAN EINVAL) - restarting iwd now"
        systemctl restart iwd
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
