import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location(
    'native_power', Path(__file__).parents[1] / 'test_native_power.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def settled(wanted, radio=None, corewlan=None):
    return {'event': 'settled', 'wanted': wanted,
            'radio_up': wanted if radio is None else radio,
            'corewlan_power': wanted if corewlan is None else corewlan}


GOOD = [settled(True), settled(False)] * module.CYCLES


class Judge(unittest.TestCase):
    def test_every_change_reached(self):
        self.assertEqual(module.judge(GOOD), [])

    def test_a_freeze_midway_is_a_failure(self):
        self.assertIn('of', module.judge(GOOD[:1])[0])

    def test_radio_that_did_not_start(self):
        found = module.judge([settled(True, radio=False)] + GOOD[1:])
        self.assertEqual(len(found), 1)
        self.assertIn('did not go on', found[0])

    def test_switch_that_disagrees_with_the_radio(self):
        found = module.judge([settled(True, corewlan=False)] + GOOD[1:])
        self.assertIn('CoreWLAN', found[0])

    def test_unknown_corewlan_state_is_not_a_pass(self):
        steps = [{'event': 'settled', 'wanted': True, 'radio_up': True, 'corewlan_power': None}]
        self.assertTrue(any('CoreWLAN' in p for p in module.judge(steps + GOOD[1:])))


class Operations(unittest.TestCase):
    def test_in_first_asked_order(self):
        interface = {'RTW89 Op getRSSI': 2, 'RTW89 Op getCHANNEL': 1, 'IOClass': 'x'}
        self.assertEqual(module.operations(interface), ['getCHANNEL', 'getRSSI'])


if __name__ == '__main__':
    unittest.main()
