"""Exercise the root installer's bounded su-shell on a plain JVM, offline.

RootShell has no Android dependency, so its output-drain / timeout logic runs as a
host-JVM fixture with fake Processes (no su, no root manager, no device). This guards
the timeout fix: a command whose output stream stays open after su is killed must not
pin the caller. No credentials, device, or network are used.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_control_java import java_tools

ROOT = Path(__file__).resolve().parents[1]


class RootedInstallerShellTests(unittest.TestCase):
    def test_bounded_shell(self):
        compiler, runtime = java_tools()
        sources = [
            ROOT / 'root-installer/src/dev/pgrazor/root/RootShell.java',
            ROOT / 'root-installer/test/RootShellTest.java',
        ]
        with tempfile.TemporaryDirectory(prefix='pgrazor-rootshell-') as work:
            compiled = subprocess.run(
                [str(compiler), '-encoding', 'UTF-8', '-d', work, *map(str, sources)],
                capture_output=True, text=True, timeout=30)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            tested = subprocess.run(
                [str(runtime), '-cp', work, 'dev.pgrazor.root.RootShellTest'],
                capture_output=True, text=True, timeout=30)
            self.assertEqual(tested.returncode, 0, tested.stdout + tested.stderr)
            self.assertIn('PASS root shell', tested.stdout)


if __name__ == '__main__':
    unittest.main()
