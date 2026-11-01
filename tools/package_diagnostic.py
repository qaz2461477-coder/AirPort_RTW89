#!/usr/bin/env python3
"""Package a backend-only RTL8922AE diagnostic build after make kext passes."""
import hashlib
import json
from pathlib import Path
import plistlib
import shutil
import subprocess

root = Path(__file__).resolve().parent.parent
source = root / 'build/out/AirPort_RTW89.kext'
output = root / 'build/diagnostic'
output.mkdir(exist_ok=True)
bundle = output / source.name
if bundle.exists():
    shutil.rmtree(bundle)
shutil.copytree(source, bundle)
plist = bundle / 'Contents/Info.plist'
info = plistlib.loads(plist.read_bytes())
info['CFBundleVersion'] = '0.5.2'
info['CFBundleShortVersionString'] = '0.5.2'
info['RTW89ExpectedDiagnosticBuild'] = 'rtl8922ae-diag3'
personality = info['IOKitPersonalities']['RTW89']
personality['IOPCIMatch'] = '0x892210ec'
personality['RTW89 Diagnostic Ethernet Only'] = True
plist.write_bytes(plistlib.dumps(info, sort_keys=False))
resources = bundle / 'Contents/Resources'
resources.mkdir(exist_ok=True)
for name in ['LICENSE', 'CREDITS.md']:
    shutil.copy2(root / name, resources / name)
shutil.copy2(root / 'firmware/LICENCE.rtlwifi_firmware.txt', resources)
subprocess.run(['codesign', '--force', '--sign', '-', str(bundle)], check=True)
subprocess.run(['codesign', '--verify', '--strict', str(bundle)], check=True)
shutil.copy2(root / 'build/out/rtw89ctl', output)
manifest = {str(p.relative_to(output)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in output.rglob('*') if p.is_file() and p.name != 'SHA256.json'}
(output / 'SHA256.json').write_text(json.dumps(manifest, indent=2) + '\n')
print(output)
