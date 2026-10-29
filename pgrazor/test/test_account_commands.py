from pathlib import Path
import subprocess
import tempfile
import unittest

from test_control_java import java_tools

ROOT = Path(__file__).resolve().parents[1]


class AccountCommandTests(unittest.TestCase):
    def test_process_lifetime_commands(self):
        compiler, runtime = java_tools()
        with tempfile.TemporaryDirectory(prefix='pgrazor-account-command-') as directory:
            sources = [ROOT / 'src/dev/pgrazor/AccountCommandState.java',
                       ROOT / 'test/AccountCommandStateTest.java']
            result = subprocess.run([str(compiler), '-encoding', 'UTF-8', '-d', directory,
                                     *map(str, sources)], capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(runtime), '-cp', directory,
                                     'dev.pgrazor.AccountCommandStateTest'],
                                    capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('PASS: AccountCommandState', result.stdout)
