#!/usr/bin/env python3
"""Put the machine to sleep while joined, and check the network comes back.

Run by hand through Run-Native-Sleep.command after the join test passed. The
person at the machine wakes it with a key. macOS rejoins by itself: the backend's
own rejoin stays off. Every step goes to sleep.jsonl with an fsync.
"""
import argparse
import json
import os
from pathlib import Path
import plistlib
import re
import subprocess
import time

REJOIN_SECONDS = 40
SLEEP_SECONDS = 40
PAGE = 'https://www.apple.com/library/test/success.html'


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


def sleep_time(text):
    """The seconds in `sysctl kern.sleeptime`: when the kernel last slept, 0 if never."""
    match = re.search(r'sec = (\d+)', text)
    return int(match.group(1)) if match else 0


def judge(steps):
    """Return the list of ways the run falls short."""
    by_event = {step['event']: step for step in steps}
    if not by_event.get('before', {}).get('authorized'):
        return ['not joined before sleeping']
    if not by_event.get('woke', {}).get('slept'):
        return ['the machine did not sleep']
    if not by_event.get('rejoined'):
        return ['the link did not come back after wake']
    if by_event.get('page', {}).get('http') != '200':
        return [f"HTTPS after wake gave {by_event.get('page', {}).get('http')}"]
    return []


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--folder', required=True, type=Path)
    args = parser.parse_args()

    root = Path(__file__).resolve().parent.parent
    journal = (args.folder / 'sleep.jsonl').open('w', encoding='utf-8')
    steps = []

    def record(event, **data):
        step = {'time': time.strftime('%H:%M:%S'), 'event': event, **data}
        steps.append(step)
        journal.write(json.dumps(step, ensure_ascii=False, default=repr) + '\n')
        journal.flush()
        os.fsync(journal.fileno())
        print(json.dumps(step, ensure_ascii=False, default=repr), flush=True)

    controller = registry('RTW89BridgeController')
    bsd = controller.get('RTW89 BSD Name')
    authorized = controller.get('RTW89 Link Authorized') is True
    record('before', bsd=bsd, authorized=authorized, radio=controller.get('RTW89 Radio Up'))
    if authorized:
        print('\n>>> Sleeping in 5 seconds. Wake the machine with a key when it is dark.\n', flush=True)
        time.sleep(5)
        before = sleep_time(run(['sysctl', 'kern.sleeptime'], 5)['output'])
        wall = time.time()
        record('sleeping', pmset=run(['pmset', 'sleepnow'], 15)['output'])
        # Only the kernel's own record counts: time passing proves nothing, and
        # a power assertion can hold the sleep off.
        slept = False
        deadline = wall + SLEEP_SECONDS
        while not slept:
            # Checked after the wait too: the clock runs on through a real sleep,
            # so waking past the deadline must still count as having slept.
            over = time.time() >= deadline
            slept = sleep_time(run(['sysctl', 'kern.sleeptime'], 5)['output']) != before
            if over:
                break
            time.sleep(1)
        record('woke', slept=slept, seconds=round(time.time() - wall, 1))
        began = time.monotonic()
        while slept and time.monotonic() - began < REJOIN_SECONDS:
            controller = registry('RTW89BridgeController')
            ipv4 = run(['ipconfig', 'getifaddr', bsd], 5)['output']
            if controller.get('RTW89 Link Authorized') is True and ipv4[:1].isdigit():
                record('rejoined', seconds=round(time.monotonic() - began, 1), ipv4=ipv4)
                result = run(['curl', '--interface', bsd, '-sS', '-o', '/dev/null', '--max-time', '10',
                              '-w', '%{http_code}', PAGE], 12)
                record('page', http=result['output'][-3:])
                break
            time.sleep(1)

    problems = judge(steps)
    controller = registry('RTW89BridgeController')
    summary = {'passed': not problems, 'problems': problems,
               'radio_error': controller.get('RTW89 Radio Error'),
               'join_requests': controller.get('RTW89 Join Requests')}
    (args.folder / 'sleep-verification.json').write_text(
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
