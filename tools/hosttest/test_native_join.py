import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location(
    'native_join', Path(__file__).parents[1] / 'test_native_join.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

GOOD = [
    {'event': 'joined', 'connect_status': 0},
    {'event': 'address', 'ipv4': '192.168.1.20'},
    {'event': 'page', 'http': '200', 'mbps': 1},
    {'event': 'download', 'http': '200', 'mbps': 100.5},
    {'event': 'upload', 'http': '200', 'mbps': 80.2},
]


def without(event, **change):
    return [{**step, **change} if step['event'] == event else step for step in GOOD]


class Judge(unittest.TestCase):
    def test_a_good_run(self):
        self.assertEqual(module.judge(GOOD), [])

    def test_never_joined(self):
        self.assertIn('authorized', module.judge([])[0])

    def test_associated_is_not_enough(self):
        self.assertIn('authorized', module.judge([{'event': 'link', 'state': 2}])[0])

    def test_failed_completion(self):
        self.assertIn('ConnectComplete', module.judge(without('joined', connect_status=1))[0])

    def test_no_address(self):
        found = module.judge(GOOD[:1])
        self.assertIn('IPv4', found[0])

    def test_page_refused(self):
        self.assertIn('HTTPS', module.judge(without('page', http='000'))[0])

    def test_download_failed(self):
        self.assertIn('download', module.judge(without('download', mbps=0))[0])

    def test_upload_failed(self):
        self.assertIn('upload', module.judge(without('upload', mbps=0))[0])


if __name__ == '__main__':
    unittest.main()
