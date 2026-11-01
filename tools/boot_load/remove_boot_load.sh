#!/bin/sh
# Run by hand. Stops the Wi-Fi driver loading at boot. A driver already loaded stays
# until the next restart.
set -eu
[ "$(id -u)" -eq 0 ] || { echo 'Run with sudo.' >&2; exit 1; }
launchctl bootout system/com.rtw89.nativebridge.load 2>/dev/null || true
rm -f /Library/LaunchDaemons/com.rtw89.nativebridge.load.plist
rm -rf '/Library/Application Support/RTW89'
sync
echo 'Removed. Wi-Fi will not load at the next boot.'
