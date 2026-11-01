#!/bin/sh
set -eu
[ "$(id -u)" -eq 0 ] || exit 1
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
if ! kmutil showloaded 2>/dev/null | grep -q 'com.rtw89.driver'; then
    exec ./tools/load_diagnostic.sh
fi
echo 'Disconnect and unload old diagnostic; temporary replacement with diag2.'
./build/diagnostic/rtw89ctl leave
./build/diagnostic/rtw89ctl down
unloaded=0
for attempt in 1 2 3 4 5; do
    if kmutil unload -b com.rtw89.driver; then
        unloaded=1
        break
    fi
    sleep 2
done
[ "$unloaded" -eq 1 ] || { echo 'Unload failed; no replacement loaded.' >&2; exit 1; }
sleep 2
./tools/load_diagnostic.sh
