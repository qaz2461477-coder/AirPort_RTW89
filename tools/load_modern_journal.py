#!/usr/bin/env python3
"""Persist provider checkpoints before each potentially failing startup call."""
import datetime
import json
import os
from pathlib import Path
import plistlib
import subprocess
import struct
import sys
import threading
import time

root = Path(__file__).resolve().parent.parent
mode = sys.argv[2] if len(sys.argv) > 2 else 'controller'
if mode not in ('controller', 'interface', 'registration', 'bridge'):
    sys.exit('Unknown journal mode')
targets = {
    'controller': ('RTW89 Native Stage', 'native-controller-ready-no-interface', 40),
    'interface': ('RTW89 Native Interface Stage', 'native-interface-initialized-attached-not-published', 40),
    'registration': ('RTW89 Registration Stage', 'native-interface-published-radio-off', 60),
    'bridge': ('RTW89 Bridge Stage', 'native-interface-published-hardware-probed', 30),
}
stage_key, ready_stage, sampling_seconds = targets[mode]
folder = root / 'build/hardware-test' / (mode + '-journal-' + datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
folder.mkdir(mode=0o700)
journal = (folder / 'stages.jsonl').open('w')
if os.geteuid() == 0:
    owner = root.stat()
    os.chown(folder, owner.st_uid, owner.st_gid)
    os.chown(folder / 'stages.jsonl', owner.st_uid, owner.st_gid)
stop = threading.Event()
complete = threading.Event()
lock = threading.Lock()
callback_names = ('createWorkLoop', 'createWorkQueue', 'getWorkLoop',
                  'getWorkQueue', 'getLogger', 'getFaultReporter', 'getLogPipes')

def record(event, **data):
    with lock:
        journal.write(json.dumps({'time': datetime.datetime.now().isoformat(), 'event': event, **data}) + '\n')
        journal.flush()
        os.fsync(journal.fileno())

def monitor():
    previous = None
    previous_callbacks = None
    previous_routing = None
    while not stop.is_set():
        try:
            result = subprocess.run(['ioreg', '-a', '-r', '-n', 'pci10ec,8922', '-d', '1'], capture_output=True, timeout=2)
            entries = plistlib.loads(result.stdout)
            routing = {key: entry[key] for entry in entries for key in (
                'RTW89 Provider Version', 'RTW89 Shared Provider WorkLoop',
                'RTW89 Native Queue Has Thread', 'RTW89 Interface Probe Version',
                'RTW89 Registration Version', 'RTW89 Bridge Version') if key in entry}
            if routing and routing != previous_routing:
                record('queue-routing', **routing)
                previous_routing = routing
            stage = next((e.get(stage_key) for e in entries if e.get(stage_key)), None)
            if stage and stage != previous:
                record('provider-stage', stage=stage)
                previous = stage
                if stage == ready_stage:
                    complete.set()
            raw = next((e.get('RTW89 Native Callbacks') for e in entries
                        if e.get('RTW89 Native Callbacks')), None)
            if isinstance(raw, bytes) and len(raw) == 4 * len(callback_names):
                callbacks = struct.unpack('<7i', raw)
                if callbacks != previous_callbacks:
                    record('callback-counts', **dict(zip(callback_names, callbacks)))
                    previous_callbacks = callbacks
        except (subprocess.TimeoutExpired, plistlib.InvalidFileException, ValueError):
            pass
        stop.wait(0.2)

record('armed', bundle=sys.argv[1], journal_version=3, mode=mode)
thread = threading.Thread(target=monitor, daemon=True)
thread.start()
print('Persistent stage journal:', folder, flush=True)
print(f'Kernel stacks will be sampled for {sampling_seconds} seconds. If the screen freezes,',
      'wait at least 90 seconds before a forced restart.', flush=True)
sample_log = (folder / 'spindump-progress.log').open('w')
sampler = subprocess.Popen(['/usr/sbin/spindump', '-noTarget', str(sampling_seconds), '100',
                            '-o', str(folder / 'kernel-stacks.spindump'),
                            '-timelimit', str(sampling_seconds + 25)], stdout=sample_log,
                           stderr=subprocess.STDOUT)
record('kernel-sampling-started', pid=sampler.pid)
try:
    time.sleep(1)
    early_exit = sampler.poll()
    if early_exit is not None:
        record('kernel-sampling-exited-before-load', returncode=early_exit)
        print('Stack sampling did not stay active; extension load cancelled.', flush=True)
        sys.exit(1)
    record('kmutil-load-requested')
    result = subprocess.run(['kmutil', 'load', '-p', sys.argv[1]])
    record('kmutil-returned', returncode=result.returncode)
    if result.returncode == 0:
        complete.wait(timeout={'registration': 90, 'bridge': 30}.get(mode, 60))
    else:
        time.sleep(0.3)
finally:
    # Do not kill the sampler when startup fails or stalls. Its independent
    # kernel/user stack capture is the evidence this experiment needs.
    try:
        record('kernel-sampling-finished', returncode=sampler.wait(timeout=sampling_seconds + 30))
    except subprocess.TimeoutExpired:
        record('kernel-sampling-timeout', pid=sampler.pid)
    sample_log.close()
    stop.set()
    thread.join(timeout=3)
    journal.close()
sys.exit(result.returncode)
