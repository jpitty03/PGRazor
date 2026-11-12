"""Compile Android sources and round-trip the real diagnostic ZIP/JSON implementation."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from test_control_java import java_tools

ROOT = Path(__file__).resolve().parents[1]

class LogExportJavaTests(unittest.TestCase):
    def test_bundle(self):
        sdk = Path(os.environ.get('ANDROID_HOME', str(Path(os.environ['LOCALAPPDATA']) / 'Android/Sdk')))
        android = sdk / 'platforms' / os.environ.get('PGRAZOR_ANDROID_PLATFORM', 'android-36.1') / 'android.jar'
        json_sources = sorted((sdk / 'sources').glob('android-*/org/json'), reverse=True)
        self.assertTrue(json_sources, 'Android SDK JSON sources required')
        compiler, runtime = java_tools()
        with tempfile.TemporaryDirectory(prefix='pgrazor-log-export-') as directory:
            work = Path(directory)
            version = work / 'BuildVersion.java'
            version.write_text('package dev.pgrazor; final class BuildVersion { static final String NAME="test"; }')
            result = subprocess.run([str(compiler), '-encoding', 'UTF-8', '-source', '8', '-target', '8', '-cp', str(android), '-d', str(work), str(version), *map(str, (ROOT / 'src').rglob('*.java'))], capture_output=True, text=True, timeout=90)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            sources = []
            for original in json_sources[0].glob('*.java'):
                text = original.read_text(encoding='utf-8')
                text = re.sub(r'^import (?:static )?(?:android\.|libcore\.).*;\s*$', '', text, flags=re.M)
                text = re.sub(r'@(?:UnsupportedAppUsage|SystemApi|NonNull|Nullable)(?:\([^)]*\))?\s*', '', text)
                target = work / original.name
                target.write_text(text, encoding='utf-8')
                sources.append(str(target))
            cp = str(work) + os.pathsep + str(android)
            result = subprocess.run([str(compiler), '-encoding', 'UTF-8', '-cp', cp, '-d', str(work), *sources, str(ROOT / 'test/LogExportTest.java')], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(runtime), '-cp', cp, 'dev.pgrazor.LogExportTest'], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

if __name__ == '__main__':
    unittest.main()
