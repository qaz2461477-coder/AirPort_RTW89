#!/bin/sh
# Run by launchd at boot. This is the same temporary load as tools/load_native_bridge.sh,
# so nothing goes to the EFI. The marker stays behind if a load panics, and the next
# boot then skips the load rather than repeating the panic.
set -u
dir=${RTW89_BOOT_DIR:-/Library/Application Support/RTW89}
settle=${RTW89_BOOT_SETTLE:-30}
retry_wait=${RTW89_BOOT_RETRY_WAIT:-10}
marker="$dir/load-in-progress"

if [ -e "$marker" ]; then
    echo "$(date): the last load did not finish, skipping. Remove $marker to try again."
    exit 0
fi
loaded=$(kmutil showloaded 2>/dev/null)
if printf '%s\n' "$loaded" | grep -F 'com.rtw89.' >/dev/null; then
    echo "$(date): the driver is already loaded."
    exit 0
fi
if printf '%s\n' "$loaded" | grep -Ei 'AirportRTW|Airport_RTW88|itlwm|IO80211FamilyLegacy' >/dev/null; then
    echo "$(date): another Wi-Fi driver is loaded, skipping."
    exit 0
fi

: > "$marker"
sync
result=1
for attempt in 1 2 3; do
    result=0
    kmutil load -p "$dir/RTW89NativeBridge.kext" || result=$?
    echo "$(date): load attempt $attempt returned $result"
    if [ "$result" -eq 0 ] || [ "$result" -eq 27 ]; then
        break
    fi
    sleep "$retry_wait"
done
if [ "$result" -eq 0 ]; then
    # The first power-up happens here, so a panic in it still leaves the marker.
    sleep "$settle"
fi
rm -f "$marker"
exit "$result"
