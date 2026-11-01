import importlib.util
from pathlib import Path
import struct
import unittest

spec = importlib.util.spec_from_file_location(
    'parse_pklg', Path(__file__).parents[1] / 'parse_pklg.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def record(seconds, kind, payload):
    return struct.pack('>III', len(payload) + 9, seconds, 0) + bytes([kind]) + payload


class Summarise(unittest.TestCase):
    def test_a_stalled_connection(self):
        data = (record(10, 0, bytes.fromhex('0d2001ff')) +
                record(10, 1, bytes.fromhex('3e0301001000')) +
                record(11, 0, bytes.fromhex('1620021000')) +
                record(16, 3, bytes.fromhex('102010000c00')) +
                record(40, 1, bytes.fromhex('0504001000' '16')))
        lines, counts = module.summarise(data)
        self.assertIn('LE Create Connection', lines[0])
        self.assertIn('LE Connection Complete', lines[1])
        self.assertIn('LE Read Remote Features', lines[2])
        self.assertIn('< ACL', lines[3])
        self.assertIn('reason 0x16', lines[4])
        self.assertEqual(counts[3], 1)

    def test_completions_are_found(self):
        data = record(1, 1, bytes.fromhex('3e0c04001000' '0000000000000000'))
        self.assertIn('Read Remote Features Complete', module.summarise(data)[0][0])

    def test_torn_final_record_is_ignored(self):
        data = record(1, 0, bytes.fromhex('0d2001ff')) + struct.pack('>III', 500, 2, 0) + b'\x00'
        self.assertEqual(len(module.summarise(data)[0]), 1)

    def test_empty_trace(self):
        self.assertEqual(module.summarise(b''), ([], {}))


if __name__ == '__main__':
    unittest.main()
