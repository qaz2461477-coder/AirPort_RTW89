import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location(
    'native_scan', Path(__file__).parents[1] / 'test_native_scan.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

GOOD = {
    'RTW89 Scan Requests': 3,
    'RTW89 Scan Status': 0,
    'RTW89 Scan Done Post Status': 0,
    'RTW89 Scan Posted': 2,
    'RTW89 Scan Results': [{'ssid': b'Other'}, {'ssid': module.TARGET_SSID}],
}
CW = [{'networks': 2}]


class Signed(unittest.TestCase):
    def test_dbm_from_the_registry(self):
        self.assertEqual(module.signed(2 ** 32 - 48), -48)
        self.assertEqual(module.signed(0), 0)


class Judge(unittest.TestCase):
    def problems(self, corewlan=CW, **changes):
        return module.judge({**GOOD, **changes}, corewlan)

    def test_a_good_scan(self):
        self.assertEqual(self.problems(), [])

    def test_macos_never_asked(self):
        self.assertTrue(any('never asked' in p for p in module.judge({}, [])))

    def test_aborted_scan(self):
        self.assertIn('ended with', self.problems(**{'RTW89 Scan Status': 0xe00002eb})[0])

    def test_rejected_completion(self):
        self.assertIn('ScanDone', self.problems(**{'RTW89 Scan Done Post Status': 1})[0])

    def test_target_network_missing(self):
        found = self.problems(**{'RTW89 Scan Results': [{'ssid': b'Other'}]})
        self.assertIn(module.TARGET_SSID.decode(), found[0])

    def test_a_similar_name_is_not_the_network(self):
        found = self.problems(**{'RTW89 Scan Results': [{'ssid': module.TARGET_SSID + b'-5G'}]})
        self.assertIn(module.TARGET_SSID.decode(), found[0])

    def test_driver_saw_them_but_macos_refused(self):
        self.assertIn('accepted', self.problems(**{'RTW89 Scan Posted': 0})[0])

    def test_corewlan_saw_nothing(self):
        self.assertIn('CoreWLAN', self.problems(corewlan=[{'error': 'x'}])[0])


if __name__ == '__main__':
    unittest.main()
