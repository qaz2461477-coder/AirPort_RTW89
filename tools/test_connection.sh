#!/bin/sh
set -eu
[ "$(id -u)" -eq 0 ] || exit 1
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
ctl="$root/build/diagnostic/rtw89ctl"
trap '"$ctl" status; "$ctl" log' EXIT
check_http() {
    curl -4 --interface en2 --connect-timeout 5 --max-time 15 --fail --silent --show-error --output /dev/null --write-out 'HTTP %{http_code}, %{size_download} bytes, %{time_total}s\n' https://example.com
}
echo 'TEST connected scan'
"$ctl" scan
check_http
echo 'TEST access point probe'
"$ctl" probe
check_http
echo 'TEST forced disconnect and automatic rejoin'
"$ctl" drop
check_http
echo 'TEST six repeated HTTPS transfers'
for attempt in 1 2 3 4 5 6; do
    printf 'sample %s: ' "$attempt"
    check_http
    sleep 5
done
echo 'ALL CONNECTION CHECKS PASSED'
