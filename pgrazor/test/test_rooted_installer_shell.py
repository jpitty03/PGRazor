"""Exercise the root installer's bounded su-shell on a plain JVM, offline.

RootShell has no Android dependency, so its output-drain / timeout logic runs as a
host-JVM fixture with fake Processes (no su, no root manager, no device). This guards
the timeout fix: a command whose output stream stays open after su is killed must not
pin the caller. No credentials, device, or network are used.
"""
from pathlib import Path
import os
import shutil
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

    def test_toggle_state_and_executable_commands(self):
        compiler, runtime = java_tools()
        bash = shutil.which('bash')
        if os.name == 'nt':
            candidate = Path(os.environ.get('ProgramFiles', 'C:/Program Files')) / 'Git/bin/bash.exe'
            if candidate.exists(): bash = str(candidate)
        with tempfile.TemporaryDirectory(prefix='pgrazor-root-toggle-') as work:
            base = ROOT / 'root-installer'
            sources = [base / 'src/dev/pgrazor/root/RootModuleState.java',
                       base / 'src/dev/pgrazor/root/RootToggleCommand.java', base / 'test/RootToggleTest.java']
            compiled = subprocess.run([str(compiler), '-encoding', 'UTF-8', '-d', work, *map(str,sources)],
                                      capture_output=True, text=True, timeout=30)
            self.assertEqual(compiled.returncode,0,compiled.stderr)
            def java(*args):
                return subprocess.run([str(runtime),'-cp',work,'dev.pgrazor.root.RootToggleTest',*args],
                                      capture_output=True,text=True,timeout=10)
            checked = java()
            self.assertEqual(checked.returncode,0,checked.stderr)
            if not bash: self.skipTest('JVM state passed; POSIX shell unavailable')
            commands = {name: java(name).stdout for name in ('on','off','preserve','probe')}
            folder = Path(work)
            live = folder / 'live'; pending = folder / 'pending'
            (live/'pgrazor').mkdir(parents=True)
            (live/'module.prop').write_text('version=v0.2.5-test\n')
            marker = live/'pgrazor/toggle-v1'; flag = live/'pgrazor/injection-off'
            stopped = folder/'stopped'
            def run(name, fail_stop=False):
                script = commands[name].replace('/data/adb/modules_update/pgrazor','./pending').replace('/data/adb/modules/pgrazor','./live')
                fake_am = 'am(){ return 1; }; ' if fail_stop else 'am(){ [ "$1" = force-stop ] && [ "$2" = com.nianticlabs.pokemongo ] && touch stopped; }; '
                return subprocess.run([bash,'-c',fake_am+script],cwd=work,capture_output=True,text=True,timeout=10)
            self.assertNotIn('MODTOGGLE=1',run('probe').stdout)
            self.assertNotEqual(run('off').returncode,0)
            self.assertFalse(flag.exists()); self.assertFalse(stopped.exists())
            marker.touch()
            for staged in (False,True):
                if staged: (pending/'pgrazor').mkdir(parents=True)
                for on in (False,True):
                    result=run('on' if on else 'off')
                    self.assertEqual(result.returncode,0,result.stdout+result.stderr)
                    self.assertEqual(flag.is_file(),not on)
                    if staged: self.assertEqual((pending/'pgrazor/injection-off').is_file(),not on)
                    self.assertTrue(stopped.is_file()); stopped.unlink()
                    probe=run('probe').stdout
                    self.assertIn('MODTOGGLE=1',probe)
                    self.assertEqual('MODOFF=1' in probe,not on)
                    self.assertEqual('MODPENDING=1' in probe,staged)
            for block in ('disable','remove'):
                (live/block).touch(); self.assertNotEqual(run('off').returncode,0)
                self.assertFalse(flag.exists()); self.assertFalse(stopped.exists()); (live/block).unlink()
            result=run('off',True)
            self.assertNotEqual(result.returncode,0); self.assertNotIn('PGRAZOR_TOGGLE_OK',result.stdout)
            self.assertTrue(flag.exists())
            run('on'); stopped.unlink()
            (pending/'pgrazor/injection-off').mkdir()
            failed=run('off')
            self.assertNotEqual(failed.returncode,0); self.assertFalse(stopped.exists())
            self.assertNotIn('PGRAZOR_TOGGLE_OK',failed.stdout)
            (pending/'pgrazor/injection-off').rmdir()
            self.assertEqual(run('preserve').returncode,0)
            self.assertTrue((pending/'pgrazor/injection-off').is_file())
            self.assertFalse(stopped.exists())


if __name__ == '__main__':
    unittest.main()
