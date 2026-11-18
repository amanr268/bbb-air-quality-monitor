#!/bin/bash
# Publishes the board's network/system info every 10 s, retained, on
# aqm/bbb-aqi/net for the phone app: Wi-Fi SSID, signal, frequency, link
# rate, IPs (home + Tailscale), router, uptime, load, free RAM.
# Talks to the local broker's anonymous localhost listener (port 1884).
TOPIC=${AQM_NET_TOPIC:-aqm/bbb-aqi/net}
IFACE=${AQM_NET_IFACE:-wlan0}

num_or_null() { [ -n "$1" ] && echo "$1" || echo null; }
jstr() { local s=${1//\\/\\\\}; s=${s//\"/\\\"}; printf '"%s"' "$s"; }

while :; do
    L=$(iw dev "$IFACE" link 2>/dev/null)
    ssid=$(sed -n 's/^[[:space:]]*SSID: //p' <<<"$L")
    sig=$(sed -n 's/^[[:space:]]*signal: \(-\?[0-9]\+\).*/\1/p' <<<"$L")
    freq=$(sed -n 's/^[[:space:]]*freq: \([0-9]\+\).*/\1/p' <<<"$L")
    rate=$(sed -n 's/^[[:space:]]*tx bitrate: \([0-9.]\+\).*/\1/p' <<<"$L")
    ip=$(ip -4 -o addr show "$IFACE" 2>/dev/null | awk '{split($4,a,"/"); print a[1]; exit}')
    tsip=$(ip -4 -o addr show tailscale0 2>/dev/null | awk '{split($4,a,"/"); print a[1]; exit}')
    gw=$(ip -4 route show default dev "$IFACE" 2>/dev/null | awk '{print $3; exit}')
    up=$(cut -d. -f1 /proc/uptime)
    load=$(cut -d' ' -f1-3 /proc/loadavg)
    mem=$(awk '/^MemAvailable:/{print $2}' /proc/meminfo)

    printf '{"ts":%s,"host":%s,"iface":%s,"ssid":%s,"signal":%s,"freq":%s,"rate":%s,"ip":%s,"gw":%s,"ts_ip":%s,"uptime":%s,"load":%s,"mem_avail_kb":%s}' \
        "$(date +%s)" "$(jstr "$(hostname)")" "$(jstr "$IFACE")" "$(jstr "$ssid")" \
        "$(num_or_null "$sig")" "$(num_or_null "$freq")" "$(num_or_null "$rate")" \
        "$(jstr "$ip")" "$(jstr "$gw")" "$(jstr "$tsip")" "$up" "$(jstr "$load")" \
        "$(num_or_null "$mem")" |
        mosquitto_pub -h 127.0.0.1 -p 1884 -t "$TOPIC" -r -q 1 -s || true
    sleep 10
done
