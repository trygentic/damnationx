#!/usr/bin/env bash
set -euo pipefail
apk="${1:?APK path}"
adb install -r "$apk"
adb shell settings put global device_provisioned 1
adb shell settings put secure user_setup_complete 1
sleep 20
for attempt in 1 2 3; do
  adb shell am force-stop net.damnationx.dev
  adb logcat -c
  adb shell am start -W -n net.damnationx.dev/org.diasurgical.devilutionx.DevilutionXSDLActivity --ez check_lobby true
  for second in $(seq 1 30); do
    if adb logcat -d | grep 'Lobby GET verified: https://lobby.damnationx.net'; then exit 0; fi
    sleep 1
  done
  adb logcat -d > /tmp/android-lobby-attempt-$attempt.log
  grep -E 'SDL|Lobby|lobby|damnationx|FATAL EXCEPTION' /tmp/android-lobby-attempt-$attempt.log | tail -100 || true
done
exit 1
