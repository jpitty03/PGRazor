"""Exercise the production settings-search catalogue without requiring an Android runtime."""
from pathlib import Path
import os, shutil, subprocess, tempfile, unittest

ROOT = Path(__file__).resolve().parents[1]


class SettingsSearchTests(unittest.TestCase):
    def test_search_catalogue(self):
        home = Path(os.environ.get('JAVA_HOME', '')) / 'bin'
        suffix = '.exe' if os.name == 'nt' else ''
        javac = str(home / ('javac' + suffix)) if (home / ('javac' + suffix)).is_file() else shutil.which('javac')
        java = str(home / ('java' + suffix)) if (home / ('java' + suffix)).is_file() else shutil.which('java')
        if not javac or not java:
            self.skipTest('JDK required')
        with tempfile.TemporaryDirectory(prefix='pgrazor-settings-search-') as work:
            subprocess.run(
                [javac, '-encoding', 'UTF-8', '-d', work,
                 str(ROOT / 'src/dev/pgrazor/SettingsSearch.java'),
                 str(ROOT / 'test/SettingsSearchTest.java')],
                check=True, timeout=45)
            result = subprocess.run([java, '-cp', work, 'dev.pgrazor.SettingsSearchTest'],
                                    capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('SettingsSearch', result.stdout)
            print(result.stdout.strip())
