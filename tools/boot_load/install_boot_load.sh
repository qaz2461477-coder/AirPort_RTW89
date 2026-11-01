#!/bin/sh
# Run by hand. Installs a built kext to load at each boot, starting with the next one.
# The EFI is not touched, and tools/boot_load/remove_boot_load.sh undoes this.
# Usage: sudo tools/boot_load/install_boot_load.sh [path/to/RTW89NativeBridge.kext]
set -eu
[ "$(id -u)" -eq 0 ] || { echo 'Run with sudo.' >&2; exit 1; }
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
src=${1:-$root/build/native-bridge/RTW89NativeBridge.kext}
dir='/Library/Application Support/RTW89'
plist=/Library/LaunchDaemons/com.rtw89.nativebridge.load.plist

[ "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$src/Contents/Info.plist")" = com.rtw89.nativebridge ] \
    || { echo "$src is not the native bridge." >&2; exit 1; }
codesign --verify --strict "$src"
plutil -lint "$root/tools/boot_load/com.rtw89.nativebridge.load.plist" >/dev/null

mkdir -p "$dir"
rm -f "$dir/load-in-progress"
rm -rf "$dir/RTW89NativeBridge.kext"
cp -R "$src" "$dir/"
cp "$root/tools/boot_load/load_at_boot.sh" "$dir/"
chown -R root:wheel "$dir"
chmod -R go-w "$dir"
cp "$root/tools/boot_load/com.rtw89.nativebridge.load.plist" "$plist"
chown root:wheel "$plist"
chmod 644 "$plist"
sync
echo 'Installed. Wi-Fi will load by itself at the next boot.'
