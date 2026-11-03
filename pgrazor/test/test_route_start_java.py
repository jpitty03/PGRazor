"""Pure Java GPX route-start helpers (closest stop, distances, resume order)."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_control_java import java_tools
ROOT = Path(__file__).resolve().parents[1]
class RouteStartJavaTests(unittest.TestCase):
    def test_route_start(self):
        compiler, runtime = java_tools()
        with tempfile.TemporaryDirectory(prefix="pgrazor-route-start-") as work:
            sources = [ROOT / "src/dev/pgrazor/RouteStart.java", ROOT / "test/RouteStartTest.java"]
            result = subprocess.run([str(compiler), "-encoding", "UTF-8", "-d", work, *map(str, sources)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(runtime), "-cp", work, "dev.pgrazor.RouteStartTest"], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
if __name__ == "__main__":
    unittest.main()
