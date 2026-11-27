"""Simulation of the live rare-Pokemon berry fallback bug (old vs fixed path)."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_control_java import java_tools

ROOT = Path(__file__).resolve().parents[1]


class TagBerryLiveSimulationJavaTests(unittest.TestCase):
    def test_simulation(self):
        compiler, runtime = java_tools()
        with tempfile.TemporaryDirectory(prefix="pgrazor-berry-sim-") as work:
            sources = [ROOT / "src/dev/pgrazor/BerryPriority.java", ROOT / "test/TagBerryLiveBugSimulationTest.java"]
            result = subprocess.run([str(compiler), "-encoding", "UTF-8", "-d", work, *map(str, sources)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(runtime), "-cp", work, "dev.pgrazor.TagBerryLiveBugSimulationTest"], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("PASS simulation", result.stdout)


if __name__ == "__main__":
    unittest.main()
