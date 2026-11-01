#!/usr/bin/env python3
"""Summarise a bluetoothd HCI trace (.pklg): what a connection attempt did.

Usage: python3 tools/parse_pklg.py trace.pklg

Records are big-endian length, seconds, microseconds, then a type byte (0 command,
1 event, 2 ACL out, 3 ACL in) and the payload; the next record starts at
start + length + 4. Connection attempts are listed with the LE link-layer
completions that followed, since those are what this controller has been missing.
"""
import collections
import struct
import sys

LE_EVENTS = {0x01: 'LE Connection Complete', 0x03: 'LE Connection Update Complete',
             0x04: 'LE Read Remote Features Complete', 0x05: 'LE LTK Request',
             0x07: 'LE Data Length Change', 0x0a: 'LE Enhanced Connection Complete',
             0x0c: 'LE PHY Update Complete'}
COMMANDS = {0x2031: 'LE Set Default PHY', 0x2032: 'LE Set PHY', 0x2022: 'LE Set Data Length',
            0x200d: 'LE Create Connection', 0x2043: 'LE Extended Create Connection',
            0x2016: 'LE Read Remote Features', 0x2013: 'LE Connection Update',
            0x2019: 'LE Enable Encryption', 0x041d: 'Read Remote Version', 0x0405: 'Create Connection',
            0x0406: 'Disconnect', 0x0411: 'Authentication Requested', 0x2030: 'LE Read PHY'}


def records(data):
    """Yield (seconds, type, payload) from a trace, stopping at a torn record."""
    offset = 0
    while offset + 13 <= len(data):
        length, seconds, micros = struct.unpack_from('>III', data, offset)
        end = offset + length + 4
        if length < 9 or end > len(data):
            return
        yield seconds + micros / 1e6, data[offset + 12], data[offset + 13:end]
        offset = end


def summarise(data):
    """Return the notable lines of a trace, and counts of what it holds."""
    lines = []
    counts = collections.Counter()
    start = None
    for when, kind, payload in records(data):
        start = when if start is None else start
        at = f'{when - start:8.3f}'
        counts[kind] += 1
        if kind == 0 and len(payload) >= 3:
            opcode = struct.unpack_from('<H', payload)[0]
            if opcode in COMMANDS:
                lines.append(f'{at} > {COMMANDS[opcode]} {payload[3:3 + payload[2]].hex()}')
        elif kind == 1 and payload:
            if payload[0] == 0x3e and len(payload) >= 3 and payload[2] in LE_EVENTS:
                lines.append(f'{at} < {LE_EVENTS[payload[2]]} {payload[3:].hex()}')
            elif payload[0] == 0x05 and len(payload) >= 6:
                lines.append(f'{at} < Disconnection Complete status {payload[2]:#x} reason {payload[5]:#x}')
            elif payload[0] == 0x13:
                lines.append(f'{at} < Number Of Completed Packets {payload[2:].hex()}')
            elif payload[0] == 0x03 and len(payload) >= 3:
                lines.append(f'{at} < Connection Complete status {payload[2]:#x}')
        elif kind in (2, 3):
            lines.append(f"{at} {'>' if kind == 2 else '<'} ACL {payload[:14].hex()}")
    return lines, counts


def main():
    with open(sys.argv[1], 'rb') as trace:
        lines, counts = summarise(trace.read())
    print('\n'.join(lines))
    print(f'commands {counts[0]}, events {counts[1]}, ACL out {counts[2]}, ACL in {counts[3]}')


if __name__ == '__main__':
    main()
