"""Exercise the real AutoRaid controller against narrow boundary shells.

The Android handler/looper/clock and the org.json parser under
``auto_raid_stubs/`` are deliberately minimal boundary shells; these tests
exercise the controller's explicit raid-session lifecycle (token handling,
deferred main-thread travel, the movement/manual/watchdog guards, and the
frozen session copy), not Android rendering or general JSON parsing. The real
AutoRaid, AutoRaidPreferences, and MotionEngine are compiled unmodified.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_control_java import java_tools

ROOT = Path(__file__).resolve().parents[1]
HERE = Path(__file__).resolve().parent
STUB_ROOT = HERE / "auto_raid_stubs"


class AutoRaidControllerJavaTests(unittest.TestCase):
    def test_controller_lifecycle(self):
        compiler, runtime = java_tools()
        stub_sources = sorted(STUB_ROOT.rglob("*.java"))
        self.assertTrue(stub_sources, "auto_raid_stubs contains no stub sources")
        real_sources = [
            ROOT / "src/dev/pgrazor/AutoRaid.java",
            ROOT / "src/dev/pgrazor/AutoRaidPreferences.java",
            ROOT / "src/dev/pgrazor/MotionEngine.java",
            HERE / "AutoRaidControllerTest.java",
        ]
        for source in real_sources:
            self.assertTrue(source.exists(), f"missing source {source}")
        with tempfile.TemporaryDirectory(prefix="pgrazor-auto-raid-") as directory:
            work = Path(directory)
            sources = [*map(str, stub_sources), *map(str, real_sources)]
            compiled = subprocess.run(
                [str(compiler), "-encoding", "UTF-8", "-d", str(work), *sources],
                capture_output=True, text=True, timeout=60)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            tested = subprocess.run(
                [str(runtime), "-ea", "-cp", str(work), "dev.pgrazor.AutoRaidControllerTest"],
                capture_output=True, text=True, timeout=30)
            self.assertEqual(tested.returncode, 0, tested.stdout + tested.stderr)
            self.assertIn("PASS AutoRaid:", tested.stdout)


if __name__ == "__main__":
    unittest.main()
