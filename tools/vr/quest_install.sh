#!/usr/bin/env bash
# Install and launch DamnationX VR on a Meta Quest over adb (docs/design/vr.md, "Testing on a Quest 2").
#
#   tools/vr/quest_install.sh [path/to/app-debug.apk] [path/to/DIABDAT.MPQ] [path/to/hd-folder]
#
# The APK defaults to the -Pvr debug build. The data and HD arguments are optional: they are pushed into the app's
# data folder, which the game uses for --data-dir and --hd-dir. Without game data the app opens its 2D data screen.
# Ends by following the log (Ctrl+C to stop).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
APK="${1:-$ROOT/android-project/app/build/outputs/apk/debug/app-debug.apk}"
DATA="${2:-}"
HD="${3:-}"
PACKAGE="net.damnationx.vr.dev"
ACTIVITY="org.diasurgical.devilutionx.DevilutionXSDLActivity"
ADB="${ADB:-adb}"
if ! command -v "$ADB" >/dev/null 2>&1 && [ -x /opt/homebrew/share/android-commandlinetools/platform-tools/adb ]; then
	ADB=/opt/homebrew/share/android-commandlinetools/platform-tools/adb
fi

if ! "$ADB" get-state >/dev/null 2>&1; then
	echo "No device. Enable developer mode for the Quest (Meta Horizon phone app), plug in USB-C, put the headset on and"
	echo "accept 'Allow USB debugging', then run '$ADB devices'." >&2
	exit 1
fi
echo "Device: $("$ADB" shell getprop ro.product.model | tr -d '\r')"

if [[ "$APK" == *release* ]]; then PACKAGE="net.damnationx.vr"; fi
"$ADB" install -r -g "$APK"

FILES="/sdcard/Android/data/$PACKAGE/files"
if [ -n "$DATA" ]; then
	"$ADB" shell mkdir -p "$FILES"
	"$ADB" push "$DATA" "$FILES/diabdat.mpq"
fi
if [ -n "$HD" ]; then
	"$ADB" shell mkdir -p "$FILES/hd"
	"$ADB" push "$HD/." "$FILES/hd/"
fi

"$ADB" logcat -c
"$ADB" shell am start -n "$PACKAGE/$ACTIVITY"
echo "Launched. Put the headset on. Following the log:"
"$ADB" logcat -v brief | grep --line-buffered -E "OpenXR|VR|WebGPU|Dawn|DevilutionX|SDL|AndroidRuntime|FATAL|vulkan"
