#!/bin/sh
# Run by hand after saving work. A temporary load: nothing goes to the EFI or
# /Library/Extensions, and it is gone after a restart. The card is probed once
# per boot, so the test itself needs a fresh boot. With the card already claimed
# by an earlier test this only asks for approval of the new extension: the load
# stops at kmutil (27), or if it is approved already, the provider refuses the
# marked card in start() before touching it. An older version of this bundle
# being loaded takes the same path: it cannot be replaced, so nothing new runs.
set -eu
[ "$(id -u)" -eq 0 ] || { echo 'Run with sudo.' >&2; exit 1; }
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
src="$root/build/native-bridge/RTW89NativeBridge.kext"
[ -d "$src" ] || { echo 'Run make nativebridge first.' >&2; exit 1; }
[ "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$src/Contents/Info.plist")" = '0.7.2' ] || exit 1
loaded=$(kmutil showloaded 2>/dev/null)
claimed=0
if ioreg -l -r -n pci10ec,8922 -d 1 | grep -F 'RTW89 Provider Version' >/dev/null; then
    claimed=1
fi
if printf '%s\n' "$loaded" | grep -F 'com.rtw89.nativebridge (0.7.2)' >/dev/null; then
    echo 'The bridge is already loaded in this boot: restart before testing it again.' >&2
    exit 1
fi
if printf '%s\n' "$loaded" | grep -Ei 'AirportRTW|Airport_RTW88|itlwm|IO80211FamilyLegacy' >/dev/null; then
    echo 'Another Wi-Fi driver is loaded: restart first, then run this before any other test.' >&2
    exit 1
fi
if [ "$claimed" -eq 0 ] && printf '%s\n' "$loaded" | grep -F 'com.rtw89.' >/dev/null; then
    echo 'Another Wi-Fi test driver is loaded: restart first, then run this before any other test.' >&2
    exit 1
fi
codesign --verify --strict "$src"
stage=$(mktemp -d /private/var/tmp/rtw89-native-bridge.XXXXXX)
cp -R "$src" "$stage/"
chown -R root:wheel "$stage"
chmod -R go-w "$stage"
if [ "$claimed" -eq 1 ]; then
    echo 'The card is claimed in this boot, so this run only asks for approval.'
    result=0
    kmutil load -p "$stage/RTW89NativeBridge.kext" || result=$?
    if [ "$result" -eq 27 ]; then
        echo 'Approve the extension in System Settings > Privacy & Security, restart, then run this again.'
    elif [ "$result" -eq 0 ]; then
        echo 'Already approved, and the card was left alone. Restart, then run this again.'
    else
        echo "kmutil returned $result without loading anything. Restart, then run this again."
    fi
    exit "$result"
fi
cp /Library/Preferences/SystemConfiguration/NetworkInterfaces.plist "$stage/NetworkInterfaces-before.plist"
sync
python3 "$root/tools/load_modern_journal.py" "$stage/RTW89NativeBridge.kext" bridge
folder=$(ls -d "$root"/build/hardware-test/bridge-journal-* | sort | tail -n 1)
python3 "$root/tools/verify_native_bridge.py" --kext "$src" --folder "$folder" \
    --interfaces-before "$stage/NetworkInterfaces-before.plist"
python3 "$root/tools/test_native_power.py" --folder "$folder"
python3 "$root/tools/test_native_scan.py" --folder "$folder" || echo 'The scan test failed: going on to the join test.'
python3 "$root/tools/test_native_join.py" --folder "$folder"
