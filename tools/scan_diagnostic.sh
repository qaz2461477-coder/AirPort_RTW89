#!/bin/sh
set -eu
[ "$(id -u)" -eq 0 ] || { echo 'Run with sudo.' >&2; exit 1; }
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
ctl="$root/build/diagnostic/rtw89ctl"
"$ctl" status
"$ctl" log
set +e
"$ctl" scan
result=$?
set -e
"$ctl" status
"$ctl" log
exit "$result"
