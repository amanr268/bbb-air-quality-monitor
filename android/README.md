# AQM Monitor — Android app

One-page live view of the monitor for a portrait phone (laid out for a Galaxy S25+).
Plain Java, no libraries, no Gradle: a ~200-line MQTT 3.1.1 client (`Mqtt.java`)
and a programmatic UI (`MainActivity.java`). APK ≈ 30 KB.

Shows: AQI (CPCB colours), PM1/PM2.5/PM10, TVOC, eCO₂, temperature, humidity,
dew point, feels-like, 1/5/10/20 min + 24 h averages, 24 h AQI, sensor health,
last alarm, how the phone reaches the board (home Wi-Fi or Tailscale) and the
board's Wi-Fi (SSID, signal, band/channel, link rate, IPs, uptime, load, RAM).

Data: subscribes to `aqm/bbb-aqi/#` on the board's Mosquitto (port 1883, user
`phone`, read-only). Addresses are tried in order (default `192.168.1.14`, then
the Tailscale name `bbb-aqi`), so it works at home and away.

Build (Ubuntu/Debian): `sudo apt install android-sdk-platform-23 aapt dalvik-exchange apksigner zipalign`, then `./build.sh` → `app/aqm-monitor.apk`.
