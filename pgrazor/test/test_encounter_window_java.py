"""Pure Java rolling 24-hour encounter count checks."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_control_java import java_tools
ROOT = Path(__file__).resolve().parents[1]
class EncounterWindowJavaTests(unittest.TestCase):
    def test_rolling_window(self):
        compiler, runtime = java_tools()
        with tempfile.TemporaryDirectory(prefix="pgrazor-encounters-") as work:
            sources = [ROOT / "src/dev/pgrazor/EncounterWindow.java", ROOT / "test/EncounterWindowTest.java"]
            result = subprocess.run([str(compiler), "-encoding", "UTF-8", "-d", work, *map(str, sources)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(runtime), "-cp", work, "dev.pgrazor.EncounterWindowTest"], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
if __name__ == "__main__":
    unittest.main()
