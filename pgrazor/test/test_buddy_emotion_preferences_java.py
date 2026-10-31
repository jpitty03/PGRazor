"""Pure Java Buddy Emotion configuration contract checks."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_control_java import java_tools
ROOT = Path(__file__).resolve().parents[1]
class BuddyEmotionPreferencesJavaTests(unittest.TestCase):
    def test_configuration_contract(self):
        compiler, runtime = java_tools()
        with tempfile.TemporaryDirectory(prefix="pgrazor-buddy-emotion-") as work:
            sources = [ROOT / "src/dev/pgrazor/BuddyEmotionPreferences.java", ROOT / "test/BuddyEmotionPreferencesTest.java"]
            result = subprocess.run([str(compiler), "-encoding", "UTF-8", "-d", work, *map(str, sources)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(runtime), "-cp", work, "dev.pgrazor.BuddyEmotionPreferencesTest"], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
