"""Catch chance overlay text and encounter state from native events."""
import os
import re
import subprocess
import tempfile
import unittest
from pathlib import Path
from test_control_java import java_tools
ROOT = Path(__file__).resolve().parents[1]
class CatchChanceJavaTests(unittest.TestCase):
    def test_overlay_text(self):
        compiler, runtime = java_tools()
        sdk = Path(os.environ.get('ANDROID_HOME', str(Path(os.environ['LOCALAPPDATA']) / 'Android/Sdk')))
        json_sources = sorted((sdk / 'sources').glob('android-*/org/json'), reverse=True)
        if not json_sources:
            self.skipTest('Android SDK JSON sources unavailable')
        with tempfile.TemporaryDirectory(prefix="pgrazor-catch-chance-") as work:
            work = Path(work)
            sources = [ROOT / "src/dev/pgrazor/CatchChance.java", ROOT / "test/CatchChanceTest.java"]
            for original in json_sources[0].glob('*.java'):
                text = original.read_text(encoding='utf-8')
                text = re.sub(r'^import (?:static )?(?:android\.|libcore\.).*;\s*$', '', text, flags=re.M)
                text = re.sub(r'@(?:UnsupportedAppUsage|SystemApi|NonNull|Nullable)(?:\([^)]*\))?\s*', '', text)
                target = work / original.name
                target.write_text(text, encoding='utf-8')
                sources.append(target)
            classes = work / 'classes'
            result = subprocess.run([str(compiler), "-encoding", "UTF-8", "-d", str(classes), *map(str, sources)], capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(runtime), "-cp", str(classes), "dev.pgrazor.CatchChanceTest"], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
if __name__ == "__main__":
    unittest.main()
