#!/usr/bin/env python3
"""Wait for a join made through the Wi-Fi menu, then pass real traffic over it.

Run by tools/load_native_bridge.sh after the scan test. The person at the machine
picks the network and types the password in macOS's own window, so no secret
passes through here. Every step goes to join.jsonl with an fsync.
"""
import argparse
import json
import os
from pathlib import Path
import plistlib
import subprocess
import time

JOIN_SECONDS = 40
ADDRESS_SECONDS = 20
PAGE = 'https://www.apple.com/library/test/success.html'
DOWNLOAD = 'https://speed.cloudflare.com/__down?bytes=20000000'
UPLOAD = 'https://speed.cloudflare.com/__up'


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


def judge(steps):
    """Return the list of ways the run falls short, first failure first."""
    by_event = {step['event']: step for step in steps}
    joined = by_event.get('joined')
    if not joined:
        return ['the link never reached authorized']
    problems = []
    if joined.get('connect_status') != 0:
        problems.append(f"ConnectComplete said {joined.get('connect_status')}")
    if not by_event.get('address', {}).get('ipv4'):
        problems.append('no IPv4 address on the interface')
    page = by_event.get('page', {})
    if page.get('http') != '200':
        problems.append(f"HTTPS through the interface gave {page.get('http')}")
    if not by_event.get('download', {}).get('mbps'):
        problems.append('no download through the interface')
    if not by_event.get('upload', {}).get('mbps'):
        problems.append('no upload through the interface')
    return problems


def transfer(bsd, *arguments):
    """One curl bound to the interface: HTTP status and megabits per second."""
    result = run(['curl', '--interface', bsd, '-sS', '-o', '/dev/null', '--max-time', '15', '-w',
                  '%{http_code} %{size_download} %{size_upload} %{time_total}', *arguments], 20)
    parts = result['output'].split()
    if result['returncode'] != 0 or len(parts) != 4:
        return {'http': None, 'mbps': 0, 'detail': result['output'][-200:]}
    moved = max(int(parts[1]), int(parts[2]))
    return {'http': parts[0], 'bytes': moved,
            'mbps': round(moved * 8 / max(float(parts[3]), 0.001) / 1e6, 2)}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--folder', required=True, type=Path)
    args = parser.parse_args()

    root = Path(__file__).resolve().parent.parent
    journal = (args.folder / 'join.jsonl').open('w', encoding='utf-8')
    steps = []

    def record(event, **data):
        step = {'time': time.strftime('%H:%M:%S'), 'event': event, **data}
        steps.append(step)
        journal.write(json.dumps(step, ensure_ascii=False, default=repr) + '\n')
        journal.flush()
        os.fsync(journal.fileno())
        print(json.dumps(step, ensure_ascii=False, default=repr), flush=True)

    bsd = registry('RTW89BridgeController').get('RTW89 BSD Name')
    if not bsd:
        record('no-interface')
        return 1
    print('\n>>> Now pick your network in the Wi-Fi menu and type its password there.', flush=True)
    print(f'>>> Waiting up to {JOIN_SECONDS} seconds.\n', flush=True)
    record('waiting', bsd=bsd)

    last = None
    began = time.monotonic()
    controller = {}
    while time.monotonic() - began < JOIN_SECONDS:
        controller = registry('RTW89BridgeController')
        state = (controller.get('RTW89 Join Requests'), controller.get('RTW89 Link State'),
                 controller.get('RTW89 Link Authorized'), controller.get('RTW89 Connect Status'),
                 controller.get('RTW89 Link Error'))
        if state != last:
            record('link', requests=state[0], state=state[1], authorized=state[2],
                   connect_status=state[3], error=state[4], rejected=controller.get('RTW89 Join Rejected'))
            last = state
        if controller.get('RTW89 Link Authorized') is True and controller.get('RTW89 Connect Status') is not None:
            break
        time.sleep(0.5)
    if controller.get('RTW89 Link Authorized') is True:
        record('joined', connect_status=controller.get('RTW89 Connect Status'),
               frequency=controller.get('RTW89 Link Channel Frequency'),
               beacons=controller.get('RTW89 Link Beacons'))
        ipv4 = ''
        began = time.monotonic()
        while time.monotonic() - began < ADDRESS_SECONDS and not ipv4:
            ipv4 = run(['ipconfig', 'getifaddr', bsd], 5)['output']
            if not ipv4 or not ipv4[0].isdigit():
                ipv4 = ''
                time.sleep(1)
        record('address', ipv4=ipv4, seconds=round(time.monotonic() - began, 1))
        if ipv4:
            record('page', **transfer(bsd, PAGE))
            record('download', **transfer(bsd, DOWNLOAD))
            payload = args.folder / 'upload.bin'
            payload.write_bytes(os.urandom(10_000_000))
            record('upload', **transfer(bsd, '-X', 'POST', '--data-binary', f'@{payload}', UPLOAD))
            payload.unlink()
            record('ping', **run(['ping', '-c', '3', '-b', bsd, '1.1.1.1'], 8))

    controller = registry('RTW89BridgeController')
    interface = registry('RTW89BridgeInterface')
    ops = {key[len('RTW89 Op '):]: value for key, value in interface.items()
           if key.startswith('RTW89 Op ')}
    problems = judge(steps)
    summary = {'passed': not problems, 'problems': problems, 'bsd_name': bsd,
               'tx_frames': controller.get('RTW89 TX Frames'),
               'rx_frames': controller.get('RTW89 RX Frames'),
               'operations': sorted(ops, key=ops.get)}
    (args.folder / 'join-verification.json').write_text(
        json.dumps(summary, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    log = run(['log', 'show', '--last', '3m', '--style', 'compact', '--predicate',
               'process == "airportd" AND (eventMessage CONTAINS[c] "join" OR eventMessage CONTAINS[c] "assoc" '
               'OR eventMessage CONTAINS[c] "driver" OR eventMessage CONTAINS[c] "private mac")'], 30)
    (args.folder / 'join-system-log.txt').write_text(log['output'], encoding='utf-8')
    # The family's own account of the join: what it refused and why.
    family = run(['log', 'show', '--last', '3m', '--style', 'compact', '--predicate',
                  'process == "kernel" AND eventMessage CONTAINS "rtw89-native"'], 30)
    (args.folder / 'wcl-log.txt').write_text(family['output'], encoding='utf-8')
    journal.close()
    if os.geteuid() == 0:
        owner = root.stat()
        for path in args.folder.iterdir():
            os.chown(path, owner.st_uid, owner.st_gid)
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    return 1 if problems else 0


if __name__ == '__main__':
    raise SystemExit(main())
