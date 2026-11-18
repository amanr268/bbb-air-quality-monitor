#!/bin/bash
# Builds app/aqm-monitor.apk with the plain Android SDK tools (no Gradle):
# aapt (resources + manifest) -> javac (Java 8 bytecode) -> dx (dex) ->
# zipalign -> apksigner.  Ubuntu/Debian: apt install android-sdk-platform-23
# aapt dalvik-exchange apksigner zipalign
set -euo pipefail
cd "$(dirname "$0")"
SDK=/usr/lib/android-sdk
JAR=$SDK/platforms/android-23/android.jar
BT=$SDK/build-tools/debian
KS=${KEYSTORE:-$HOME/.aqm-release.jks}

rm -rf build && mkdir -p build/gen build/classes build/apk
aapt package -f -m -J build/gen -M AndroidManifest.xml -S res -I "$JAR" \
     -F build/apk/unsigned.apk
javac --release 8 -nowarn -Xlint:-options -classpath "$JAR" -d build/classes \
     $(find src build/gen -name '*.java')
java -jar "$BT/lib/dx.jar" --dex --min-sdk-version=26 \
     --output=build/apk/classes.dex build/classes
(cd build/apk && aapt add unsigned.apk classes.dex >/dev/null)
zipalign -f -p 4 build/apk/unsigned.apk build/apk/aligned.apk

if [ ! -f "$KS" ]; then
    keytool -genkeypair -keystore "$KS" -alias aqm -keyalg RSA -keysize 2048 \
        -validity 10000 -storepass aqmaqm -keypass aqmaqm \
        -dname "CN=AQM Monitor, O=Amandeep Singh" >/dev/null 2>&1
fi
mkdir -p app
apksigner sign --ks "$KS" --ks-pass pass:aqmaqm --key-pass pass:aqmaqm \
     --out app/aqm-monitor.apk build/apk/aligned.apk
apksigner verify --print-certs app/aqm-monitor.apk | head -2
ls -l app/aqm-monitor.apk
