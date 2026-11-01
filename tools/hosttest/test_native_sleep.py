import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location(
    'native_sleep', Path(__file__).parents[1] / 'test_native_sleep.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

GOOD = [
    {'event': 'before', 'authorized': True},
    {'event': 'woke', 'slept': True},
    {'event': 'rejoined', 'ipv4': '192.168.1.20'},
    {'event': 'page', 'http': '200'},
]


class SleepTime(unittest.TestCase):
    def test_kernel_record(self):
        self.assertEqual(module.sleep_time('kern.sleeptime: { sec = 1793563410, usec = 5 } Mon'), 1793563410)
        self.assertEqual(module.sleep_time('kern.sleeptime: { sec = 0, usec = 0 } Thu'), 0)

    def test_failed_query_is_not_a_sleep(self):
        self.assertEqual(module.sleep_time('sysctl failed'), 0)


class Judge(unittest.TestCase):
    def test_a_good_cycle(self):
        self.assertEqual(module.judge(GOOD), [])

    def test_not_joined_first(self):
        self.assertIn('not joined', module.judge([{'event': 'before', 'authorized': False}])[0])

    def test_machine_stayed_awake(self):
        found = module.judge([GOOD[0], {'event': 'woke', 'slept': False}])
        self.assertIn('did not sleep', found[0])

    def test_link_lost_after_wake(self):
        self.assertIn('did not come back', module.judge(GOOD[:2])[0])

    def test_rejoined_without_traffic(self):
        self.assertIn('HTTPS', module.judge(GOOD[:3] + [{'event': 'page', 'http': '000'}])[0])


if __name__ == '__main__':
    unittest.main()
