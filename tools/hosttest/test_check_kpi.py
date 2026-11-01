import importlib.util
from pathlib import Path
import unittest
spec = importlib.util.spec_from_file_location('check_kpi', Path(__file__).parents[1] / 'check_kpi.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
class ParseExports(unittest.TestCase):
    def test_multiple_exporters_and_following_symbol(self):
        text = '''For arch x86_64:
    _normal in /System/Boot.kc: com.apple.kpi.iokit (25.6.0)
    _shared in multiple:
        /System/Boot.kc: com.apple.kpi.private (25.6.0)
        /System/Boot.kc: com.apple.kpi.unsupported (25.6.0)
    _next in /System/Boot.kc: com.apple.kpi.bsd (25.6.0)
'''
        self.assertEqual(module.parse_exports(text), {
            '_normal': {'com.apple.kpi.iokit'},
            '_shared': {'com.apple.kpi.private', 'com.apple.kpi.unsupported'},
            '_next': {'com.apple.kpi.bsd'}})
    def test_unrelated_lines_are_not_exports(self):
        self.assertEqual(module.parse_exports('error: unable to inspect\n'), {})
if __name__ == '__main__': unittest.main()
