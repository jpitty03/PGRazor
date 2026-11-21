"""Session stats rolling history: hourly and 24-hour changes, storage and resets."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_control_java import java_tools

ROOT = Path(__file__).resolve().parents[1]

class SessionStatsHistoryJavaTests(unittest.TestCase):
    def test_rolling_changes(self):
        compiler, runtime = java_tools()
        with tempfile.TemporaryDirectory(prefix='pgrazor-session-stats-') as work:
            sources = [ROOT / 'src/dev/pgrazor/SessionStatsHistory.java', ROOT / 'test/SessionStatsHistoryTest.java']
            result = subprocess.run([str(compiler), '-encoding', 'UTF-8', '-d', work, *map(str, sources)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(runtime), '-cp', work, 'dev.pgrazor.SessionStatsHistoryTest'], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
