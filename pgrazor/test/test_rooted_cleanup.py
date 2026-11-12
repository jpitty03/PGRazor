"""Exercise rooted cleanup against disposable build trees, never real artifacts."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(os.name == 'nt', 'PowerShell cleanup is Windows-specific')
class RootedCleanupTests(unittest.TestCase):
    def fixture(self, folder):
        project = Path(folder).resolve()
        self.assertTrue(project.is_relative_to((ROOT / 'build').resolve()))
        shutil.copyfile(ROOT / 'cleanup-rooted.ps1', project / 'cleanup-rooted.ps1')
        runs = []
        for i in range(5):
            run = project / 'build/rooted' / ('20260101-00000%d-1234abcd' % i)
            run.mkdir(parents=True)
            module = run / 'PGRazor-Root-module-v0.2.2-root.zip'
            module.write_bytes(b'fixture module')
            (run / 'report.json').write_text(json.dumps({'module': str(module), 'module_sha256': hashlib.sha256(module.read_bytes()).hexdigest()}))
            for name in ['PGRazor-Root-v0.2.2-root.apk', 'payload.zip', 'input-0.apk', 'unique-backup.bin']:
                file = run / name
                file.write_bytes(b'fixture')
            for file in run.iterdir():
                os.utime(file, (time.time() - 7200, time.time() - 7200))
            runs.append(run)
        (project / 'build-retention.json').write_text(json.dumps({'pinnedRootedRuns': [{'run': runs[0].name, 'reason': 'tested rollback'}]}))
        (project / 'build/rooted/zz-not-a-run').mkdir()
        (project / 'build/rooted/20260102-000000-1234abcd').mkdir()  # incomplete build
        return project, runs

    def invoke(self, project, apply=False):
        return subprocess.run(['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', str(project / 'cleanup-rooted.ps1'), *(['-Apply'] if apply else [])], capture_output=True, text=True, timeout=30)

    def test_pins_completed_latest_and_versioned_installer(self):
        (ROOT / 'build').mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='rooted-cleanup-test-', dir=ROOT / 'build') as folder:
            project, runs = self.fixture(folder)
            dry = self.invoke(project)
            self.assertEqual(dry.returncode, 0, dry.stdout + dry.stderr)
            self.assertTrue(all((r / 'input-0.apk').exists() for r in runs))
            result = self.invoke(project, True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            for i, run in enumerate(runs):
                self.assertEqual((run / 'PGRazor-Root-v0.2.2-root.apk').exists(), i != 1)
                self.assertFalse((run / 'input-0.apk').exists())
                self.assertTrue((run / 'unique-backup.bin').exists())
                self.assertTrue((run / 'report.json').exists())

    def test_checksum_failure_preserves_everything(self):
        (ROOT / 'build').mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='rooted-cleanup-test-', dir=ROOT / 'build') as folder:
            project, runs = self.fixture(folder)
            (runs[-1] / 'PGRazor-Root-module-v0.2.2-root.zip').write_bytes(b'changed')
            result = self.invoke(project, True)
            self.assertNotEqual(result.returncode, 0)
            self.assertTrue(all((r / 'input-0.apk').exists() for r in runs))
