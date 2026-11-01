#!/usr/bin/env python3
"""Check what a loaded RTW89NativeBridge actually published, and keep the evidence.

Read-only: ioreg, kmutil showloaded, networksetup, ifconfig and a bounded
CoreWLAN status query. Run by tools/load_native_bridge.sh after the journal.
"""
import argparse
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess

EXPECTED_FIRMWARE = '0.35.80.3'
EXPECTED_VERSION = '0.7.2'
READY_STAGE = 'native-interface-published-hardware-probed'
INTERFACES = '/Library/Preferences/SystemConfiguration/NetworkInterfaces.plist'
CARD = 'pci10ec,8922'


def evaluate(controller, card, build_uuid, loaded_uuid, ifconfig):
    """Return the list of ways the run differs from the expected radio-off state."""
    problems = []
    if not controller:
        problems.append('no RTW89BridgeController in the registry')
    if not card:
        problems.append('no pci10ec,8922 in the registry')
    if problems:
        return problems
    wanted = (
        (controller, 'RTW89 Bridge Version', EXPECTED_VERSION),
        (controller, 'RTW89 Bridge Stage', READY_STAGE),
        (controller, 'RTW89 BSD Publication Complete', True),
        (controller, 'RTW89 Shared Provider WorkLoop', True),
        (card, 'RTW89 Firmware Version', EXPECTED_FIRMWARE),
    )
    for entry, key, value in wanted:
        if entry.get(key) != value:
            problems.append(f'{key} is {entry.get(key)!r}, expected {value!r}')
    mac = card.get('RTW89 Hardware MAC')
    if not mac or int(mac.split(':')[0], 16) & 2:
        problems.append(f'RTW89 Hardware MAC is {mac!r}, expected a factory address')
    if not build_uuid or build_uuid != loaded_uuid:
        problems.append(f'loaded UUID {loaded_uuid} is not the build {build_uuid}')
    match = re.search(r'ether\s+(\S+)', ifconfig)
    # macOS may already have given the interface a private address.
    if not match or (match.group(1).lower() != (mac or '').lower() and
                     not int(match.group(1).split(':')[0], 16) & 2):
        problems.append(f'BSD address is {match.group(1) if match else None}, expected {mac}')
    if re.search(r'status:\s+active', ifconfig):
        problems.append('the link is active, but nothing can join yet')
    return problems


def run(command, timeout=10):
    try:
        result = subprocess.run(command, capture_output=True, text=True, timeout=timeout,
                                encoding='utf-8', errors='replace')
        return result.stdout + result.stderr
    except (OSError, subprocess.TimeoutExpired) as error:
        return f'{command[0]} failed: {error}\n'


def registry(*match):
    try:
        output = subprocess.check_output(['ioreg', '-a', '-r', *match, '-d', '1'], timeout=10)
        entries = plistlib.loads(output) if output.strip() else []
    except (subprocess.SubprocessError, plistlib.InvalidFileException, ValueError):
        return None
    return entries[0] if len(entries) == 1 else None


def build_uuid(binary):
    match = re.search(r'UUID: (\S+)', run(['dwarfdump', '--uuid', str(binary)]))
    return match.group(1).upper() if match else None


def loaded_uuid(bundle_id):
    match = re.search(r'[0-9A-Fa-f]{8}(?:-[0-9A-Fa-f]{4}){3}-[0-9A-Fa-f]{12}',
                      run(['kmutil', 'showloaded', '--bundle-identifier', bundle_id]))
    return match.group(0).upper() if match else None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--kext', required=True, type=Path)
    parser.add_argument('--folder', required=True, type=Path)
    parser.add_argument('--interfaces-before', type=Path)
    args = parser.parse_args()

    root = Path(__file__).resolve().parent.parent
    controller, card = registry('-c', 'RTW89BridgeController'), registry('-n', CARD)
    bsd = (controller or {}).get('RTW89 BSD Name')
    ifconfig = run(['ifconfig', bsd]) if bsd else ''
    built = build_uuid(args.kext / 'Contents/MacOS/RTW89NativeBridge')
    loaded = loaded_uuid('com.rtw89.nativebridge')
    problems = evaluate(controller, card, built, loaded, ifconfig)

    keep = {
        'hardware-ports.txt': run(['networksetup', '-listallhardwareports']),
        'interface-ifconfig.txt': ifconfig,
        'kmutil-showloaded.txt': run(['kmutil', 'showloaded', '--bundle-identifier',
                                      'com.rtw89.nativebridge']),
    }
    status_tool = root / 'build/native-registration/native_wifi_status'
    if status_tool.exists():
        keep['corewlan-status.json'] = run([str(status_tool)])
    if card and card.get('RTW89 Log'):
        keep['driver-log.txt'] = card['RTW89 Log']
    for name, text in keep.items():
        (args.folder / name).write_text(text, encoding='utf-8')
    for name, match in (('RTW89BridgeController.plist', ('-c', 'RTW89BridgeController')),
                        ('RTW89BridgeProvider.plist', ('-c', 'RTW89BridgeProvider')),
                        ('RTW89BridgeBSD.plist', ('-c', 'RTW89BridgeBSD')),
                        ('RTW89BridgeInterface.plist', ('-c', 'RTW89BridgeInterface')),
                        ('card.plist', ('-n', CARD))):
        (args.folder / name).write_text(run(['ioreg', '-a', '-r', *match, '-l']), encoding='utf-8')
    if args.interfaces_before and args.interfaces_before.exists():
        shutil.copy2(args.interfaces_before, args.folder / 'NetworkInterfaces-before.plist')
    if Path(INTERFACES).exists():
        shutil.copy2(INTERFACES, args.folder / 'NetworkInterfaces-after.plist')

    summary = {
        'passed': not problems,
        'problems': problems,
        'bsd_name': bsd,
        'stage': (card or {}).get('RTW89 Bridge Stage'),
        'radio_up': (controller or {}).get('RTW89 Radio Up'),
        'hardware_error': (card or {}).get('RTW89 Hardware Error'),
        'mac': (card or {}).get('RTW89 Hardware MAC'),
        'firmware': (card or {}).get('RTW89 Firmware Version'),
        'chip_cut': (card or {}).get('RTW89 Chip Cut'),
        'rfe_type': (card or {}).get('RTW89 RFE Type'),
        'tx_streams': (card or {}).get('RTW89 TX Streams'),
        'rx_streams': (card or {}).get('RTW89 RX Streams'),
        'build_uuid': built,
        'loaded_uuid': loaded,
    }
    (args.folder / 'bridge-verification.json').write_text(
        json.dumps(summary, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    if os.geteuid() == 0:
        owner = root.stat()
        for path in args.folder.iterdir():
            os.chown(path, owner.st_uid, owner.st_gid)
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    return 1 if problems else 0


if __name__ == '__main__':
    raise SystemExit(main())
