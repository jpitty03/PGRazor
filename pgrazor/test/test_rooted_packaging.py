"""Root installer version metadata must be safe for both Android and module.prop."""
import sys
import json
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import rooted
import rooted_installer


class RootedPackagingTests(unittest.TestCase):
    def test_native_target_cannot_follow_an_arbitrary_input_apk(self):
        schema = rooted.ROOT / 'native/schema/game'
        profile = json.loads((schema / json.loads((schema / 'current.json').read_text())['profile']).read_text())
        target = dict(versionCode=profile['version_code'], versionName=profile['game_version'])
        rooted.require_supported_target(target)
        for field, value in [('versionCode', target['versionCode'] + 1), ('versionName', 'unreviewed')]:
            with self.subTest(field=field), self.assertRaises(ValueError):
                rooted.require_supported_target(dict(target, **{field: value}))

    def test_shared_version_and_manifest(self):
        for version, code in [('v0.2.2-root', 2002), ('1.23.456-rc.1', 1023456)]:
            self.assertEqual(rooted.version_code(version), code)
            manifest = ET.fromstring(rooted_installer._manifest(version))
            self.assertEqual(manifest.attrib['{http://schemas.android.com/apk/res/android}versionCode'], str(code))
            self.assertEqual(manifest.attrib['package'], 'dev.pgrazor.root')

    def test_reject_ambiguous_overflow_and_injection(self):
        for version in ['0.0.0', '1.1000.0', '1.0.1000', '9999.0.0', '1.2.3-x\nupdateJson=evil', '1.2.3-"x', '1.2.3-', 'v1.2']:
            with self.subTest(version=version), self.assertRaises(ValueError):
                rooted_installer._manifest(version)


class ToggleCapabilityTests(unittest.TestCase):
    """The launch-time PGRazor: On / Off switch: capability marker, preservation script, loader."""

    def test_capability_marker_is_an_empty_file_under_pgrazor(self):
        entries = rooted.capability_module_entries()
        self.assertEqual(rooted.TOGGLE_CAPABILITY, 'pgrazor/toggle-v1')
        self.assertIn(rooted.TOGGLE_CAPABILITY, entries)
        # An empty capability marker -- presence, not contents, is the signal.
        self.assertEqual(entries[rooted.TOGGLE_CAPABILITY], b'')

    def test_customize_script_is_shipped_in_the_module(self):
        entries = rooted.capability_module_entries()
        self.assertEqual(entries['customize.sh'], rooted.CUSTOMIZE_SH)
        # package() merges these into the module zip without overwriting payload/dex/loader.
        self.assertEqual(set(entries), {'customize.sh', rooted.TOGGLE_CAPABILITY})

    def test_injection_off_is_never_shipped_by_default(self):
        # A fresh install must default to On: the off flag is not a packaged entry, and the
        # installer clears stale staged Off only when the live choice is On.
        self.assertEqual(rooted.INJECTION_OFF_FLAG, 'pgrazor/injection-off')
        self.assertNotIn(rooted.INJECTION_OFF_FLAG, rooted.capability_module_entries())
        script = rooted.CUSTOMIZE_SH
        self.assertIn('rm -f "${STAGED_FLAG}"', script)

    def test_customize_script_preserves_choice_and_refuses_failed_write(self):
        import os, shutil, subprocess, tempfile
        bash = shutil.which('bash')
        if os.name == 'nt':
            candidate = Path(os.environ.get('ProgramFiles', 'C:/Program Files')) / 'Git/bin/bash.exe'
            if candidate.exists(): bash = str(candidate)
        if not bash: self.skipTest('POSIX shell unavailable')
        script = rooted.CUSTOMIZE_SH.replace('/data/adb/modules/pgrazor/', './live/')
        prefix = 'MODPATH=./staged; ui_print(){ :; }; abort(){ echo "$1"; exit 1; }; '
        with tempfile.TemporaryDirectory(prefix='pgrazor-toggle-install-') as tmp:
            work = Path(tmp)
            live = work / 'live/pgrazor/injection-off'
            staged = work / 'staged/pgrazor/injection-off'
            live.parent.mkdir(parents=True); staged.parent.mkdir(parents=True)
            def run():
                return subprocess.run([bash, '-c', prefix + script], cwd=tmp,
                                      capture_output=True, text=True, timeout=10)
            self.assertEqual(run().returncode, 0)
            self.assertFalse(staged.exists())
            live.touch()
            self.assertEqual(run().returncode, 0)
            self.assertTrue(staged.is_file())
            live.unlink()
            self.assertEqual(run().returncode, 0)
            self.assertFalse(staged.exists())
            live.touch(); staged.mkdir()  # An unwritable flag destination must abort.
            failed = run()
            self.assertNotEqual(failed.returncode, 0, failed.stdout + failed.stderr)
            self.assertTrue(live.is_file())

    def test_loader_checks_injection_off_before_reading_the_dex(self):
        source = (rooted.ROOT / 'zygisk/loader.cpp').read_text(encoding='utf-8')
        check = source.find('faccessat(dir, "pgrazor/injection-off", F_OK, 0) == 0')
        dex = source.find('read_at(dir, "pgrazor/classes.dex"')
        self.assertNotEqual(check, -1, 'loader must gate on the injection-off flag')
        self.assertNotEqual(dex, -1)
        # The switch is honoured before any payload is read, and disarms this launch.
        self.assertLess(check, dex)
        gate = source[check:dex]
        self.assertIn('target_ = false;', gate)
        self.assertIn('return;', gate)


if __name__ == '__main__':
    unittest.main()
