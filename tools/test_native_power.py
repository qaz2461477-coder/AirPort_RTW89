#!/usr/bin/env python3
"""Turn the radio on and off through macOS and record what happens.

Run by tools/load_native_bridge.sh once the bridge has published. Every step goes
to power.jsonl with an fsync before the next one, so a freeze leaves the last
step reached. Ends with the radio off. No scan, no join, no network settings.
"""
import argparse
import json
import os
from pathlib import Path
import plistlib
import subprocess
import time

CYCLES = 2
SETTLE_SECONDS = 10
DWELL_SECONDS = 2


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


def operations(interface):
    """The unsupported requests macOS made, in the order it first made them."""
    found = {key[len('RTW89 Op '):]: value for key, value in interface.items()
             if key.startswith('RTW89 Op ')}
    return sorted(found, key=found.get)


def judge(steps):
    """Return the list of ways the recorded steps fall short."""
    problems = []
    expected = [step for step in steps if step.get('event') == 'settled']
    if len(expected) != CYCLES * 2:
        problems.append(f'{len(expected)} of {CYCLES * 2} power changes were reached')
    for step in expected:
        wanted = step['wanted']
        if step.get('radio_up') is not wanted:
            problems.append(f"the radio did not go {'on' if wanted else 'off'}: {step}")
        if step.get('corewlan_power') is not wanted:
            problems.append(f"CoreWLAN did not report {'on' if wanted else 'off'}: {step}")
    return problems


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--folder', required=True, type=Path)
    args = parser.parse_args()

    root = Path(__file__).resolve().parent.parent
    status_tool = root / 'build/native-registration/native_wifi_status'
    journal = (args.folder / 'power.jsonl').open('w', encoding='utf-8')
    steps = []

    def record(event, **data):
        step = {'time': time.strftime('%H:%M:%S'), 'event': event, **data}
        steps.append(step)
        journal.write(json.dumps(step, ensure_ascii=False) + '\n')
        journal.flush()
        os.fsync(journal.fileno())
        print(json.dumps(step, ensure_ascii=False), flush=True)

    def corewlan(bsd):
        result = run([str(status_tool)], 10)
        try:
            entries = json.loads(result['output'])
        except ValueError:
            return None
        return next((entry.get('powerOn') for entry in entries if entry.get('interface') == bsd), None)

    controller = registry('RTW89BridgeController')
    bsd = controller.get('RTW89 BSD Name')
    if not bsd:
        record('no-interface')
        return 1
    record('start', bsd=bsd, radio_up=controller.get('RTW89 Radio Up'), corewlan_power=corewlan(bsd))

    for cycle in range(CYCLES):
        for wanted in (True, False):
            record('request', cycle=cycle, wanted=wanted)
            result = run(['networksetup', '-setairportpower', bsd, 'on' if wanted else 'off'], 30)
            record('requested', cycle=cycle, wanted=wanted, **result)
            began = time.monotonic()
            while time.monotonic() - began < SETTLE_SECONDS:
                if registry('RTW89BridgeController').get('RTW89 Radio Up') is wanted:
                    break
                time.sleep(0.25)
            seconds = round(time.monotonic() - began, 2)
            time.sleep(1)
            controller = registry('RTW89BridgeController')
            record('settled', cycle=cycle, wanted=wanted, seconds=seconds,
                   radio_up=controller.get('RTW89 Radio Up'),
                   radio_error=controller.get('RTW89 Radio Error'),
                   post_status=controller.get('RTW89 Power Post Status'),
                   power_gets=controller.get('RTW89 Power Gets'),
                   power_sets=controller.get('RTW89 Power Sets'),
                   corewlan_power=corewlan(bsd))
            if wanted:
                time.sleep(DWELL_SECONDS)

    interface = registry('RTW89BridgeInterface')
    wanted_log = run(['log', 'show', '--last', '5m', '--style', 'compact', '--predicate',
                      'process == "wifid" OR process == "airportd" OR process == "kernel" AND '
                      'eventMessage CONTAINS "RTW89"'], 60)
    (args.folder / 'wifi-system-log.txt').write_text(wanted_log['output'], encoding='utf-8')
    ops = operations(interface)
    problems = judge(steps)
    summary = {'passed': not problems, 'problems': problems, 'bsd_name': bsd, 'operations': ops}
    (args.folder / 'power-verification.json').write_text(
        json.dumps(summary, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    journal.close()
    if os.geteuid() == 0:
        owner = root.stat()
        for path in args.folder.iterdir():
            os.chown(path, owner.st_uid, owner.st_gid)
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    return 1 if problems else 0


if __name__ == '__main__':
    raise SystemExit(main())
