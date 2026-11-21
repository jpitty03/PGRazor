"""Check the joystick lock thresholds Pad uses, without an Android runtime."""
from pathlib import Path
import subprocess, tempfile, unittest
from test_control_java import java_tools

ROOT = Path(__file__).resolve().parents[1]

class JoystickLockJavaTests(unittest.TestCase):
    def test_thresholds(self):
        compiler, runtime = java_tools()
        with tempfile.TemporaryDirectory(prefix='pgrazor-joystick-lock-') as work:
            subprocess.run([str(compiler), '-encoding', 'UTF-8', '-d', work, str(ROOT / 'src/dev/pgrazor/JoystickLock.java'), str(ROOT / 'test/JoystickLockTest.java')], check=True, timeout=45)
            result = subprocess.run([str(runtime), '-cp', work, 'dev.pgrazor.JoystickLockTest'], capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('JoystickLock tests passed', result.stdout)
