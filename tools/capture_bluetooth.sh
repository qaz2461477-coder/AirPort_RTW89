#!/bin/bash
set -euo pipefail
[ "$(id -u)" -eq 0 ] || exit 1
root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/build/hardware-test/bt-hci-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$out"
chmod 700 "$out"
pref=/Library/Preferences/com.apple.MobileBluetooth.debug.plist
had=0
if [ -f "$pref" ]; then cp -p "$pref" "$out/debug-preferences.before.plist"; had=1; fi
cleanup() {
    if [ "$had" -eq 1 ]; then cp -p "$out/debug-preferences.before.plist" "$pref";
    else /usr/bin/defaults delete /Library/Preferences/com.apple.MobileBluetooth.debug HCITraces >/dev/null 2>&1 || true; fi
    chown -R "${SUDO_USER:-root}" "$out"
    chmod -R go-rwx "$out"
}
trap cleanup EXIT
/usr/bin/defaults write /Library/Preferences/com.apple.MobileBluetooth.debug HCITraces '<dict><key>HCIFileTraces</key><true/><key>StackDebugEnabled</key><true/><key>UnlimitedHCIFileSize</key><false/></dict>'
pkill -x bluetoothd
printf '\nBluetooth trace active. Open Bluetooth settings and connect the device.\nWait for failure, about 30 seconds, then press Return here.\n'
read -r reply
sleep 2
for logfile in /private/var/root/Library/Logs/Bluetooth/*.pklg; do
    [ -f "$logfile" ] && cp "$logfile" "$out/"
done
/usr/bin/log show --last 3m --style compact --predicate 'process == "bluetoothd"' > "$out/bluetoothd.log" 2>&1
system_profiler SPBluetoothDataType > "$out/controller.txt"
printf 'Saved trace to %s\n' "$out"
