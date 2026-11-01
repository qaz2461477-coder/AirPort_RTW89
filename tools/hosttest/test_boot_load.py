import os
from pathlib import Path
import stat
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).parents[1] / 'boot_load/load_at_boot.sh'
FAKE_KMUTIL = '''#!/bin/sh
if [ "$1" = showloaded ]; then
    printf '%s\\n' "$FAKE_LOADED"
    exit 0
fi
echo load >> "$FAKE_CALLS"
exit "$FAKE_RESULT"
'''


class LoadAtBoot(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.dir = Path(temporary.name)
        kmutil = self.dir / 'kmutil'
        kmutil.write_text(FAKE_KMUTIL, encoding='utf-8')
        kmutil.chmod(kmutil.stat().st_mode | stat.S_IEXEC)
        self.calls = self.dir / 'calls'
        self.marker = self.dir / 'load-in-progress'

    def run_script(self, loaded='', result=0):
        environment = {**os.environ, 'PATH': f"{self.dir}:{os.environ['PATH']}",
                       'RTW89_BOOT_DIR': str(self.dir), 'RTW89_BOOT_SETTLE': '0',
                       'RTW89_BOOT_RETRY_WAIT': '0', 'FAKE_LOADED': loaded,
                       'FAKE_CALLS': str(self.calls), 'FAKE_RESULT': str(result)}
        return subprocess.run(['/bin/sh', str(SCRIPT)], env=environment, capture_output=True,
                              text=True, timeout=10, encoding='utf-8', errors='replace')

    def load_count(self):
        return len(self.calls.read_text(encoding='utf-8').split()) if self.calls.exists() else 0

    def test_loads_once_and_clears_the_marker(self):
        self.assertEqual(self.run_script().returncode, 0)
        self.assertEqual(self.load_count(), 1)
        self.assertFalse(self.marker.exists())

    def test_marker_from_a_failed_boot_stops_the_load(self):
        self.marker.touch()
        self.assertEqual(self.run_script().returncode, 0)
        self.assertEqual(self.load_count(), 0)
        self.assertTrue(self.marker.exists())

    def test_already_loaded_driver_is_left_alone(self):
        self.run_script(loaded='com.rtw89.nativebridge (0.7.2)')
        self.assertEqual(self.load_count(), 0)

    def test_another_wifi_driver_stops_the_load(self):
        self.run_script(loaded='com.zxystd.itlwm (2.0)')
        self.assertEqual(self.load_count(), 0)

    def test_failure_is_retried_and_clears_the_marker(self):
        self.assertEqual(self.run_script(result=1).returncode, 1)
        self.assertEqual(self.load_count(), 3)
        self.assertFalse(self.marker.exists())

    def test_missing_approval_is_not_retried(self):
        self.assertEqual(self.run_script(result=27).returncode, 27)
        self.assertEqual(self.load_count(), 1)


if __name__ == '__main__':
    unittest.main()
