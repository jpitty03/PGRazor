"""Pure Java Shiny Scanner settings and daily-count checks."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_control_java import java_tools
ROOT = Path(__file__).resolve().parents[1]
class ShinyScanPreferencesJavaTests(unittest.TestCase):
    def test_settings_contract(self):
        compiler, runtime = java_tools()
        with tempfile.TemporaryDirectory(prefix="pgrazor-shiny-scan-") as work:
            sources = [ROOT / "src/dev/pgrazor/AutoRaidPreferences.java", ROOT / "src/dev/pgrazor/ShinyScanPreferences.java",
                       ROOT / "test/ShinyScanPreferencesTest.java"]
            result = subprocess.run([str(compiler), "-encoding", "UTF-8", "-d", work, *map(str, sources)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(runtime), "-cp", work, "dev.pgrazor.ShinyScanPreferencesTest"], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
if __name__ == "__main__":
    unittest.main()
