#!/bin/sh
set -eu
[ "$(id -u)" -eq 0 ] || exit 1
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
src="$root/build/modern-controller/RTW89ModernController.kext"
version=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$src/Contents/Info.plist")
[ "$version" = '0.1.3' ] || { echo 'Previous controller build is blocked after crash.' >&2; exit 1; }
codesign --verify --strict "$src"
stage=$(mktemp -d /private/var/tmp/rtw89-modern-controller.XXXXXX)
cp -R "$src" "$stage/"
chown -R root:wheel "$stage"
chmod -R go-w "$stage"
sync
python3 "$root/tools/load_modern_journal.py" "$stage/RTW89ModernController.kext"
for attempt in $(seq 1 60); do
    state=$(ioreg -r -c RTW89ModernController -l -w 0 -d 1)
    if printf '%s' "$state" | grep -q 'native-controller-ready-no-interface'; then
        printf '%s\n' "$state"
        exit 0
    fi
    sleep 1
done
echo 'Probe did not register within 60 seconds.' >&2
exit 1
