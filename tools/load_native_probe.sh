#!/bin/sh
set -eu
[ "$(id -u)" -eq 0 ] || exit 1
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
src="$root/build/native-probe/RTW89NativeProbe.kext"
codesign --verify --strict "$src"
stage=$(mktemp -d /private/var/tmp/rtw89-native-probe.XXXXXX)
cp -R "$src" "$stage/"
chown -R root:wheel "$stage"
chmod -R go-w "$stage"
sync
kmutil load -p "$stage/RTW89NativeProbe.kext"
for attempt in 1 2 3 4 5 6 7 8 9 10; do
    state=$(ioreg -r -c RTW89NativeProbe -l -w 0 -d 1)
    if printf '%s' "$state" | grep -q 'metadata-complete'; then
        printf '%s\n' "$state"
        exit 0
    fi
    sleep 1
done
echo 'Probe did not register within 10 seconds.' >&2
exit 1
