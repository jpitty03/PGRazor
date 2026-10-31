"""Pure Java Gym Automation configuration contract checks."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_control_java import java_tools
ROOT = Path(__file__).resolve().parents[1]
class GymAutomationPreferencesJavaTests(unittest.TestCase):
    def test_configuration_contract(self):
        compiler, runtime = java_tools()
        with tempfile.TemporaryDirectory(prefix="pgrazor-gym-automation-") as work:
            sources = [ROOT / "src/dev/pgrazor/GymAutomationPreferences.java", ROOT / "test/GymAutomationPreferencesTest.java"]
            result = subprocess.run([str(compiler), "-encoding", "UTF-8", "-d", work, *map(str, sources)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(runtime), "-cp", work, "dev.pgrazor.GymAutomationPreferencesTest"], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
