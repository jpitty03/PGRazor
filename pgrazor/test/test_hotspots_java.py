"""Offline catalog and destination cooldown checks."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_control_java import java_tools

ROOT = Path(__file__).resolve().parents[1]

class HotspotsJavaTests(unittest.TestCase):
    def test_catalog_and_cooldown(self):
        compiler, runtime = java_tools()
        with tempfile.TemporaryDirectory(prefix='pgrazor-hotspots-') as work:
            sources = [ROOT / 'src/dev/pgrazor/Hotspots.java', ROOT / 'src/dev/pgrazor/CooldownState.java', ROOT / 'test/HotspotsTest.java']
            result = subprocess.run([str(compiler), '-encoding', 'UTF-8', '-d', work, *map(str, sources)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(runtime), '-cp', work, 'dev.pgrazor.HotspotsTest'], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
