#!/bin/sh
set -eu
[ "$(id -u)" -eq 0 ] || exit 1
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
src="$root/build/native-interface/RTW89NativeInterfaceProbe.kext"
[ "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$src/Contents/Info.plist")" = '0.1.0' ] || exit 1
codesign --verify --strict "$src"
python3 - "$root" <<'PY'
import hashlib, json, pathlib, sys
root = pathlib.Path(sys.argv[1])
saved = root / 'build/known-good-modern-controller-0.1.3'
manifest = json.loads((saved / 'SHA256.json').read_text())
for relative, expected in manifest.items():
    for folder in (saved, root / 'build/modern-controller'):
        if hashlib.sha256((folder / relative).read_bytes()).hexdigest() != expected:
            raise SystemExit('Controller package differs from preserved 0.1.3: ' + relative)
print('Verified controller package against known-good 0.1.3 hashes')
PY
# Reuse a running known-good controller. After an approval reboot, load the
# preserved controller first; never unload or replace a running controller.
if ! ioreg -r -c RTW89ModernController -l -w 0 -d 1 | grep -q 'native-controller-ready-no-interface'; then
    "$root/tools/load_modern_controller.sh"
fi
python3 - <<'PY'
import plistlib, subprocess
entries = plistlib.loads(subprocess.check_output(['ioreg', '-a', '-r', '-c', 'RTW89NativeProvider', '-d', '1']))
if len(entries) != 1 or entries[0].get('RTW89 Provider Version') != '0.1.3':
    raise SystemExit('Expected the verified 0.1.3 shared-queue provider')
PY
stage=$(mktemp -d /private/var/tmp/rtw89-native-interface.XXXXXX)
cp -R "$src" "$stage/"
chown -R root:wheel "$stage"
chmod -R go-w "$stage"
sync
python3 "$root/tools/load_modern_journal.py" "$stage/RTW89NativeInterfaceProbe.kext" interface
python3 - <<'PY'
import json, plistlib, subprocess
entries = plistlib.loads(subprocess.check_output(['ioreg', '-a', '-r', '-c', 'RTW89NativeInterfaceProbe', '-d', '1']))
keys = ('RTW89 Native Interface Stage', 'RTW89 Interface Probe Version',
        'RTW89 No Radio Access', 'RTW89 BSD Interface Published')
summary = [{k:e[k] for k in keys if k in e} for e in entries]
print(json.dumps(summary, indent=2))
if len(entries) != 1 or entries[0].get('RTW89 Interface Probe Version') != '0.1.0' or entries[0].get('RTW89 Native Interface Stage') != 'native-interface-initialized-attached-not-published':
    raise SystemExit('Interface initialization/attachment did not complete')
PY
