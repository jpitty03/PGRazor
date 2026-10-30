"""Pure Java Smart Catch configuration contract checks."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_control_java import java_tools
ROOT = Path(__file__).resolve().parents[1]
class SmartCatchPreferencesJavaTests(unittest.TestCase):
    def test_configuration_contract(self):
        compiler, runtime = java_tools()
        with tempfile.TemporaryDirectory(prefix="pgrazor-smart-catch-") as work:
            sources = [ROOT / "src/dev/pgrazor/SmartCatchPreferences.java", ROOT / "test/SmartCatchPreferencesTest.java"]
            result = subprocess.run([str(compiler), "-encoding", "UTF-8", "-d", work, *map(str, sources)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(runtime), "-cp", work, "dev.pgrazor.SmartCatchPreferencesTest"], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
