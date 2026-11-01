#!/usr/bin/env python3
"""Scan through macOS's own path and record what came back.

Run by tools/load_native_bridge.sh after the power test. Turns the radio on and
leaves it on, so the networks can be seen in the Wi-Fi menu. No join, and no
network settings are changed. Every step goes to scan.jsonl with an fsync.
"""
import argparse
import json
import os
from pathlib import Path
import plistlib
import subprocess
import time

TARGET_SSID = os.environ.get('RTW89_TEST_SSID', 'TestAP').encode()
SCANS = 2
SETTLE_SECONDS = 10


def registry(cls):
    try:
        output = subprocess.check_output(['ioreg', '-a', '-r', '-c', cls, '-d', '1'], timeout=10)
        entries = plistlib.loads(output) if output.strip() else []
    except (subprocess.SubprocessError, plistlib.InvalidFileException, ValueError):
        return {}
    return entries[0] if len(entries) == 1 else {}


def run(command, timeout):
    try:
        result = subprocess.run(command, capture_output=True, text=True, timeout=timeout,
                                encoding='utf-8', errors='replace')
        return {'returncode': result.returncode, 'output': (result.stdout + result.stderr).strip()}
    except (OSError, subprocess.TimeoutExpired) as error:
        return {'returncode': None, 'output': str(error)}


def signed(value):
    """The registry holds a 32-bit number; dBm is negative."""
    return value - 2 ** 32 if value >= 2 ** 31 else value


def judge(controller, corewlan):
    """Return the list of ways the scan falls short."""
    problems = []
    if not controller.get('RTW89 Scan Requests'):
        problems.append('macOS never asked the driver to scan')
    if controller.get('RTW89 Scan Status') != 0:
        problems.append(f"the last scan ended with {controller.get('RTW89 Scan Status')}")
    if controller.get('RTW89 Scan Done Post Status') != 0:
        problems.append(f"ScanDone posted {controller.get('RTW89 Scan Done Post Status')}")
    results = controller.get('RTW89 Scan Results') or []
    if not any(entry.get('ssid') == TARGET_SSID for entry in results):
        problems.append(f'{TARGET_SSID.decode()} is not in the {len(results)} networks found')
    if not controller.get('RTW89 Scan Posted'):
        problems.append('no result was accepted by macOS')
    if not any(step.get('networks') for step in corewlan):
        problems.append('CoreWLAN returned no network')
    return problems


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--folder', required=True, type=Path)
    args = parser.parse_args()

    root = Path(__file__).resolve().parent.parent
    scan_tool = root / 'build/native-bridge/native_wifi_scan'
    journal = (args.folder / 'scan.jsonl').open('w', encoding='utf-8')

    def record(event, **data):
        step = {'time': time.strftime('%H:%M:%S'), 'event': event, **data}
        journal.write(json.dumps(step, ensure_ascii=False, default=repr) + '\n')
        journal.flush()
        os.fsync(journal.fileno())
        print(json.dumps(step, ensure_ascii=False, default=repr), flush=True)
        return step

    controller = registry('RTW89BridgeController')
    bsd = controller.get('RTW89 BSD Name')
    if not bsd:
        record('no-interface')
        return 1
    record('radio-on', **run(['networksetup', '-setairportpower', bsd, 'on'], 30))
    began = time.monotonic()
    while time.monotonic() - began < SETTLE_SECONDS:
        if registry('RTW89BridgeController').get('RTW89 Radio Up') is True:
            break
        time.sleep(0.25)
    # macOS changes the private address right after power on.
    time.sleep(3)
    corewlan = []
    for attempt in range(SCANS):
        result = run([str(scan_tool)], 20)
        try:
            answer = json.loads(result['output'])
        except ValueError:
            answer = {'error': result['output'][:300]}
        corewlan.append(answer)
        controller = registry('RTW89BridgeController')
        record('scan', attempt=attempt, corewlan=answer,
               requests=controller.get('RTW89 Scan Requests'),
               rejected=controller.get('RTW89 Scan Rejected'),
               posted=controller.get('RTW89 Scan Posted'),
               status=controller.get('RTW89 Scan Status'),
               found=len(controller.get('RTW89 Scan Results') or []))
        time.sleep(1)

    controller = registry('RTW89BridgeController')
    interface = registry('RTW89BridgeInterface')
    found = [{'ssid': entry.get('ssid', b'').decode('utf-8', 'replace'), 'bssid': entry.get('bssid'),
              'channel': entry.get('channel'), 'rssi': signed(entry.get('rssi', 0))}
             for entry in controller.get('RTW89 Scan Results') or []]
    ops = {key[len('RTW89 Op '):]: value for key, value in interface.items()
           if key.startswith('RTW89 Op ')}
    problems = judge(controller, corewlan)
    summary = {'passed': not problems, 'problems': problems, 'corewlan': corewlan,
               'requests': controller.get('RTW89 Scan Requests'),
               'posted': controller.get('RTW89 Scan Posted'), 'found': found,
               'operations': sorted(ops, key=ops.get)}
    (args.folder / 'scan-verification.json').write_text(
        json.dumps(summary, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    log = run(['log', 'show', '--last', '2m', '--style', 'compact', '--predicate',
               'process == "airportd" AND eventMessage CONTAINS[c] "scan"'], 20)
    (args.folder / 'scan-system-log.txt').write_text(log['output'], encoding='utf-8')
    journal.close()
    if os.geteuid() == 0:
        owner = root.stat()
        for path in args.folder.iterdir():
            os.chown(path, owner.st_uid, owner.st_gid)
    print(json.dumps({k: v for k, v in summary.items() if k != 'found'}, ensure_ascii=False, indent=2))
    print(f'{len(found)} networks found')
    return 1 if problems else 0


if __name__ == '__main__':
    raise SystemExit(main())
