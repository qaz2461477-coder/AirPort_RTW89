import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location(
    'verify_native_bridge', Path(__file__).parents[1] / 'verify_native_bridge.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

CONTROLLER = {
    'RTW89 Bridge Version': module.EXPECTED_VERSION,
    'RTW89 Bridge Stage': module.READY_STAGE,
    'RTW89 BSD Publication Complete': True,
    'RTW89 Shared Provider WorkLoop': True,
}
CARD = {
    'RTW89 Hardware MAC': '00:e0:4c:12:34:56',
    'RTW89 Firmware Version': module.EXPECTED_FIRMWARE,
}
UUID = 'AD63CFDA-DE79-3D2E-8D25-BE6C264C832F'
IFCONFIG = 'en4: flags=8823<UP>\n\tether 00:e0:4c:12:34:56\n\tstatus: inactive\n'


class Evaluate(unittest.TestCase):
    def problems(self, controller=None, card=None, built=UUID, loaded=UUID, ifconfig=IFCONFIG):
        return module.evaluate({**CONTROLLER, **(controller or {})}, {**CARD, **(card or {})},
                               built, loaded, ifconfig)

    def test_expected_state_passes(self):
        self.assertEqual(self.problems(), [])

    def test_missing_objects(self):
        self.assertEqual(len(module.evaluate(None, None, UUID, UUID, IFCONFIG)), 2)

    def test_random_mac_is_refused(self):
        found = self.problems(card={'RTW89 Hardware MAC': '7a:1d:45:db:37:8d'})
        self.assertIn('RTW89 Hardware MAC', found[0])

    def test_bsd_address_must_match_the_card(self):
        self.assertIn('BSD address', self.problems(ifconfig=IFCONFIG.replace('00:e0', '04:e0'))[0])

    def test_private_address_is_accepted(self):
        self.assertEqual(self.problems(ifconfig=IFCONFIG.replace('00:e0', '02:52')), [])

    def test_no_interface_output(self):
        self.assertIn('BSD address', self.problems(ifconfig='')[0])

    def test_active_link_is_refused_while_radio_off(self):
        self.assertIn('active', self.problems(ifconfig=IFCONFIG.replace('inactive', 'active'))[0])

    def test_stale_cached_binary_is_refused(self):
        self.assertIn('loaded UUID', self.problems(loaded='00000000-0000-0000-0000-000000000000')[0])

    def test_unknown_build_is_refused(self):
        self.assertIn('loaded UUID', self.problems(built=None, loaded=None)[0])

    def test_wrong_stage_is_refused(self):
        self.assertIn('RTW89 Bridge Stage', self.problems(controller={'RTW89 Bridge Stage': 'x'})[0])


if __name__ == '__main__':
    unittest.main()
