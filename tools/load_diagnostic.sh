#!/bin/sh
# Run manually after saving work. Temporary load; no EFI/system-volume install.
set -eu
[ "$(id -u)" -eq 0 ] || { echo 'Run with sudo.' >&2; exit 1; }
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
src="$root/build/diagnostic/AirPort_RTW89.kext"
[ -d "$src" ] || { echo 'Build and package the diagnostic first.' >&2; exit 1; }
loaded=$(kmutil showloaded 2>/dev/null)
if printf '%s\n' "$loaded" | grep -Ei 'com\.rtw89\.|AirportRTW|Airport_RTW88|itlwm|IO80211FamilyLegacy' >/dev/null; then
    echo 'Conflicting driver or legacy Wi-Fi stack loaded; use the original working EFI first.' >&2
    exit 1
fi
/usr/bin/codesign --verify --strict "$src"
stage=$(mktemp -d /private/var/tmp/rtl8922ae-diag.XXXXXX)
cp -R "$src" "$stage/"
chown -R root:wheel "$stage"
chmod -R go-w "$stage"
sync
printf 'Loading temporary diagnostic from %s\n' "$stage"
kmutil load -p "$stage/AirPort_RTW89.kext"
# kmutil may return before asynchronous IOService::start has completed.
ready=0
for attempt in $(seq 1 60); do
    if "$root/build/diagnostic/rtw89ctl" status > "$stage/status.txt" 2>&1; then
        ready=1
        break
    fi
    sleep 1
done
ioreg -r -c AirPort_RTW89 -l -w 0 -d 1
expected=$(/usr/libexec/PlistBuddy -c 'Print :RTW89ExpectedDiagnosticBuild' "$src/Contents/Info.plist")
if ! ioreg -r -c AirPort_RTW89 -l -w 0 -d 1 | grep -Fq "\"RTW89 Diagnostic Build\" = \"$expected\""; then
    echo 'Wrong cached diagnostic binary loaded; stop and reboot before testing.' >&2
    exit 1
fi
[ "$ready" -eq 1 ] || { cat "$stage/status.txt"; echo 'Driver did not register within 60 seconds.' >&2; exit 1; }
"$root/build/diagnostic/rtw89ctl" status
"$root/build/diagnostic/rtw89ctl" log
