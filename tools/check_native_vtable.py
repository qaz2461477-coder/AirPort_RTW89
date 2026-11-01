#!/usr/bin/env python3
"""Compare a compiled subclass's virtual slots with this host's Boot KC.

Checks symbol placement, not object layouts or complete private API behavior.
Pure parent slots cannot reveal which abstract method belongs in that slot.
"""
import argparse
import json
from pathlib import Path
import re
import struct
import subprocess


class MachO:
    def __init__(self, path):
        self.data = Path(path).read_bytes()
        if struct.unpack_from('<I', self.data)[0] != 0xfeedfacf:
            raise ValueError('Expected a little-endian Mach-O 64 image')
        self.segments, self.sections, self.symbols = [], [], []
        symtab = None
        offset = 32
        for _ in range(struct.unpack_from('<I', self.data, 16)[0]):
            command, size = struct.unpack_from('<II', self.data, offset)
            if command == 0x19:
                self.segments.append(struct.unpack_from('<QQQQ', self.data, offset + 24))
                count = struct.unpack_from('<I', self.data, offset + 64)[0]
                for i in range(count):
                    values = struct.unpack_from('<16s16sQQIIIIIIII', self.data, offset + 72 + 80 * i)
                    self.sections.append(dict(address=values[2], size=values[3],
                                              relocations=values[6], count=values[7]))
            elif command == 2:
                symtab = struct.unpack_from('<IIII', self.data, offset + 8)
            offset += size
        if symtab:
            symoff, count, stroff, _ = symtab
            for i in range(count):
                string, kind, section, _, value = struct.unpack_from('<IBBHQ', self.data, symoff + 16 * i)
                end = self.data.index(b'\0', stroff + string)
                name = self.data[stroff + string:end].decode()
                self.symbols.append(dict(name=name, section=section, value=value))

    def read_pointer(self, address):
        for vm, _, offset, length in self.segments:
            if vm <= address and address + 8 <= vm + length:
                return struct.unpack_from('<Q', self.data, offset + address - vm)[0]
        raise ValueError(f'Unmapped virtual address {address:#x}')


def method_suffix(symbol):
    match = re.match(r'^__ZNK?(\d+)', symbol)
    if match:
        return symbol[match.end() + int(match[1]):]
    return None


def demangled_method(symbol):
    # A parent method may encode its own class as S_ in a parameter, while a
    # subclass must encode that parent name explicitly. Compare expanded types.
    value = subprocess.check_output(['xcrun', 'llvm-cxxfilt', symbol[1:]], text=True).strip()
    return value.partition('::')[2] or None


def compare(object_path, subclass, parent, kernel_path, local_bases=()):
    obj, kc = MachO(object_path), MachO(kernel_path)
    named, addressed = {}, {}
    output = subprocess.check_output(['xcrun', 'nm', '-n', str(kernel_path)], text=True)
    for line in output.splitlines():
        fields = line.split()
        if len(fields) == 3:
            try:
                address = int(fields[0], 16)
            except ValueError:
                continue
            named[fields[2]] = address
            addressed.setdefault(address, set()).add(fields[2])
    parent_vtable = named[f'__ZTV{len(parent)}{parent}']
    symbol = next(s for s in obj.symbols if s['name'] == f'__ZTV{len(subclass)}{subclass}')
    section = obj.sections[symbol['section'] - 1]
    end = min((s['value'] for s in obj.symbols
               if s['section'] == symbol['section'] and s['value'] > symbol['value']),
              default=section['address'] + section['size'])
    base = min(s[0] for s in kc.segments)
    rows = []
    for i in range(section['count']):
        relative, info = struct.unpack_from('<iI', obj.data, section['relocations'] + i * 8)
        address = section['address'] + relative
        if not symbol['value'] + 16 <= address < end:
            continue
        if not info & (1 << 27) or info >> 28 != 0:
            raise ValueError('Unexpected vtable relocation type')
        compiled = obj.symbols[info & 0xffffff]['name']
        slot = address - symbol['value'] - 16
        raw = kc.read_pointer(parent_vtable + 16 + slot)
        target = raw if raw >> 48 == 0xffff else base + (raw & 0x3fffffff)
        names = addressed.get(target, set())
        own = any(re.match(r'^__ZNK?' + str(len(name)) + re.escape(name), compiled)
                  for name in (subclass, *local_bases))
        if compiled in names:
            result = 'inherited-match'
        elif '_RESERVED' in compiled:
            result = 'reserved-not-validated'
        elif own and any(method_suffix(compiled) == method_suffix(s) for s in names):
            result = 'override-match'
        elif own and '___cxa_pure_virtual' in names:
            result = 'abstract-slot-override-not-identifiable'
        elif own and any(demangled_method(compiled) == demangled_method(s) for s in names):
            result = 'override-match'
        else:
            result = 'MISMATCH'
        rows.append(dict(slot=hex(slot), compiled=compiled, kernel=sorted(names), result=result))
    if not rows:
        raise ValueError('No subclass vtable relocations found')
    return sorted(rows, key=lambda r: int(r['slot'], 16))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('object', type=Path)
    parser.add_argument('subclass')
    parser.add_argument('parent')
    parser.add_argument('--kernel', type=Path,
                        default=Path('/System/Library/KernelCollections/BootKernelExtensions.kc'))
    parser.add_argument('--output', type=Path)
    parser.add_argument('--local-base', action='append', default=[],
                        help='Additional locally defined superclass overriding Apple slots')
    args = parser.parse_args()
    rows = compare(args.object, args.subclass, args.parent, args.kernel, args.local_base)
    if args.output:
        args.output.write_text(json.dumps(rows, indent=2) + '\n')
    counts = {result: sum(row['result'] == result for row in rows)
              for result in sorted({row['result'] for row in rows})}
    print(args.subclass, json.dumps(counts, sort_keys=True))
    for row in rows:
        if row['result'] == 'MISMATCH':
            print(json.dumps(row))
    return int(any(row['result'] == 'MISMATCH' for row in rows))


if __name__ == '__main__':
    raise SystemExit(main())
