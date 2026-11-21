#!/bin/bash
# AQM board self-test: prints PASS / WARN / FAIL for each subsystem.
# Run on the board:  bash scripts/aqm_selftest.sh   (asks for sudo once)
# Also runs daily from aqm-selftest.timer: journalctl -u aqm-selftest -n 70
P=0; W=0; F=0
if [ -t 1 ]; then G='\e[32m' Y='\e[33m' R='\e[31m' B='\e[1m' N='\e[0m'; else G= Y= R= B= N=; fi
pass() { printf "  ${G}PASS${N}  %s\n" "$*"; P=$((P+1)); }
warn() { printf "  ${Y}WARN${N}  %s\n" "$*"; W=$((W+1)); }
fail() { printf "  ${R}FAIL${N}  %s\n" "$*"; F=$((F+1)); }
hdr()  { printf "\n${B}== %s${N}\n" "$*"; }
sudo -v || exit 1

hdr "System"
k=$(uname -r); [[ $k == *rt* ]] && pass "kernel $k (PREEMPT_RT)" || warn "kernel $k is not the RT kernel"
pass "uptime $(uptime -p | sed 's/^up //'), load $(cut -d' ' -f1-3 /proc/loadavg)"
mem=$(awk '/MemAvailable/{print int($2/1024)}' /proc/meminfo)
[ "$mem" -gt 100 ] && pass "free RAM ${mem} MB" || warn "free RAM only ${mem} MB"
use=$(df --output=pcent / | tail -1 | tr -dc 0-9)
[ "$use" -lt 85 ] && pass "rootfs ${use}% used" || warn "rootfs ${use}% used"
e=$(journalctl -b -p err --no-pager -q 2>/dev/null | wc -l)
[ "$e" -eq 0 ] && pass "no error-level log lines this boot" || warn "$e error-level log lines this boot (journalctl -b -p err)"

hdr "Services"
for s in aqm mosquitto aqm-netinfo tailscaled wifi-check.timer rtc-int32k; do
    st=$(systemctl is-active "$s" 2>/dev/null)
    [ "$st" = active ] && pass "$s active" || fail "$s is '$st'"
done
r=$(systemctl show -p NRestarts --value aqm)
[ "${r:-0}" -eq 0 ] && pass "aqm restarts since start: 0" || warn "aqm restarted $r times (journalctl -u aqm)"
pass "aqm status: $(systemctl show -p StatusText --value aqm)"

hdr "Watchdogs / clock"
[ "$(systemctl show -p WatchdogUSec --value aqm)" != 0 ] && pass "aqm systemd watchdog armed ($(systemctl show -p WatchdogUSec --value aqm))" || fail "aqm watchdog not armed"
rw=$(systemctl show -p RuntimeWatchdogUSec --value)
[ "$rw" != 0 ] && [ -n "$rw" ] && pass "hardware watchdog serviced by systemd ($rw)" || fail "hardware watchdog not configured"
a=$(cat /sys/class/rtc/rtc0/since_epoch); sleep 2; b=$(cat /sys/class/rtc/rtc0/since_epoch)
[ $((b-a)) -ge 1 ] && pass "RTC ticking (needed for one-tap power-off)" || fail "RTC frozen: rtc-int32k did not run"
[ "$(timedatectl show -p NTPSynchronized --value)" = yes ] && pass "clock NTP-synchronised: $(date '+%d %b %H:%M')" || warn "clock not NTP-synchronised yet"

hdr "Device tree / hardware"
ls /proc/device-tree/chosen/overlays 2>/dev/null | grep -q BB-AQM && pass "overlay BB-AQM-00A0 loaded" || warn "BB-AQM overlay not listed under /proc/device-tree/chosen/overlays"
for d in /dev/ttyS1 /dev/ttyS4 /dev/i2c-2; do [ -e $d ] && pass "$d present" || fail "$d missing"; done
grep -q 'aqm-keys' /proc/bus/input/devices && pass "button (gpio-keys aqm-keys) present" || fail "button input device missing"
[ -e '/sys/class/leds/aqm:alarm' ] && pass "alarm LED (aqm:alarm) present" || fail "alarm LED missing"
lsmod | grep -qE '^(hci_uart|bluetooth) ' && warn "Bluetooth modules loaded (should be blocked)" || pass "Bluetooth kernel modules blocked"

hdr "I2C bus 2"
scan=$(sudo i2cdetect -y -r 2 2>/dev/null)
for a in "1d ADXL345" "38 AHT21" "3c OLED" "53 ENS160"; do
    set -- $a; grep -qE "(^|[[:space:]])$1([[:space:]]|$)" <<<"$scan" && pass "0x$1 $2 answers" || fail "0x$1 $2 not found"
done

hdr "Sensors (latest log lines)"
j=$(journalctl -u aqm --since "-60 s" --no-pager -q -o cat)
pms=$(grep 'PMS5003:' <<<"$j" | tail -1); aht=$(grep 'AHT21:' <<<"$j" | tail -1); ens=$(grep 'ENS160:' <<<"$j" | tail -1)
[ -n "$pms" ] && { grep -q ' 0 bad checksum' <<<"$pms" && pass "PMS5003 ${pms##*sense }" || warn "PMS5003 ${pms##*sense }"; } || fail "no PMS5003 log in the last 60 s"
[ -n "$aht" ] && { grep -q '0 bad CRC, 0 bus errors' <<<"$aht" && pass "AHT21 ${aht##*AHT21: }" || warn "AHT21 ${aht##*AHT21: }"; } || fail "no AHT21 log in the last 60 s"
if [ -n "$ens" ]; then
    grep -q 'valid' <<<"$ens" && pass "ENS160 ${ens##*ENS160: }" || warn "ENS160 not 'valid' yet (warm-up ~3 min): ${ens##*ENS160: }"
else fail "no ENS160 log in the last 60 s"; fi
grep -qi 'simulated' <<<"$j" && warn "a sensor reports SIMULATED data" || pass "no simulated sensor data"
st=$(journalctl -u aqm --since "-24 h" --no-pager -q -o cat | grep -c 'ENS160 stalled')
if [ "$st" -eq 0 ]; then pass "ENS160: no stalls in the last 24 h"
elif [ "$st" -le 5 ]; then warn "ENS160 stalled $st times in 24 h (each auto-reset; a 10 uF cap at the module would help)"
else fail "ENS160 stalled $st times in 24 h: check its 3.3 V supply / wiring"; fi
s=$(grep 'report   seq' <<<"$j" | tail -1)
[ -n "$s" ] && pass "pipeline end-to-end: ${s##*report   }" || fail "no samples reached 'report' in the last 60 s"

hdr "Network / outputs"
L=$(iw dev wlan0 link 2>/dev/null)
if grep -q Connected <<<"$L"; then
    sig=$(sed -n 's/^[[:space:]]*signal: \(-\?[0-9]\+\).*/\1/p' <<<"$L")
    [ "${sig:--100}" -ge -75 ] && pass "Wi-Fi $(sed -n 's/^[[:space:]]*SSID: //p' <<<"$L"), ${sig} dBm" || warn "Wi-Fi signal weak: ${sig} dBm"
else fail "wlan0 not connected"; fi
gw=$(ip -4 route show default | awk '{print $3; exit}')
ping -c2 -W2 "$gw" >/dev/null 2>&1 && pass "router $gw reachable" || fail "router $gw not reachable"
ping -c2 -W3 1.1.1.1 >/dev/null 2>&1 && pass "internet reachable" || warn "internet not reachable"
ts=$(tailscale ip -4 2>/dev/null | head -1)
[ -n "$ts" ] && pass "Tailscale up: $ts" || warn "Tailscale not connected"
m=$(timeout 6 mosquitto_sub -h 127.0.0.1 -p 1884 -t aqm/bbb-aqi/sample -C 1 2>/dev/null)
[ -n "$m" ] && pass "MQTT live sample: ${m:0:70}..." || fail "no MQTT sample within 6 s"
journalctl -u aqm -b --no-pager -q -o cat | grep -q 'Modbus RTU slave' && pass "Modbus RTU slave started on /dev/ttyS4" || warn "Modbus slave start not found in log"

hdr "Result"
printf '  %d PASS, %d WARN, %d FAIL\n' $P $W $F
[ $F -eq 0 ] && echo "  Board healthy." || echo "  Check the FAIL lines above."
echo "  Not testable from software: OLED picture, fans spinning, red LED -> look at the board."
exit $((F > 0))
