#!/bin/sh
set -eu
[ "$(id -u)" -eq 0 ] || exit 1
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
src="$root/build/native-registration/RTW89NativeRegistration.kext"
[ "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$src/Contents/Info.plist")" = '0.2.0' ] || exit 1
codesign --verify --strict "$src"
stage=$(mktemp -d /private/var/tmp/rtw89-native-registration.XXXXXX)
cp -R "$src" "$stage/"
chown -R root:wheel "$stage"
chmod -R go-w "$stage"
sync
python3 "$root/tools/load_modern_journal.py" "$stage/RTW89NativeRegistration.kext" registration
python3 - <<'PY'
import json, plistlib, subprocess
entries = plistlib.loads(subprocess.check_output(['ioreg', '-a', '-r', '-c', 'RTW89RegistrationController', '-d', '1']))
keys = ('RTW89 Registration Stage', 'RTW89 Registration Version',
        'RTW89 BSD Publication Complete', 'RTW89 BSD Name', 'RTW89 BSD Prepare Error',
        'RTW89 Diagnostic Radio Off', 'RTW89 Shared Provider WorkLoop')
print(json.dumps([{k:e[k] for k in keys if k in e} for e in entries], indent=2))
if len(entries) != 1 or entries[0].get('RTW89 Registration Version') != '0.2.0' or entries[0].get('RTW89 BSD Publication Complete') is not True or entries[0].get('RTW89 Registration Stage') != 'native-interface-published-radio-off':
    raise SystemExit('Native BSD publication did not complete')
PY
