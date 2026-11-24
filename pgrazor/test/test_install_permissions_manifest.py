"""Binary-manifest tests for the install permissions the in-app updater needs.

Reuses the synthetic binary manifests in test_axml.py.
"""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('pgrazor_axml', ROOT / 'axml.py')
axml = importlib.util.module_from_spec(spec)
spec.loader.exec_module(axml)

test_spec = importlib.util.spec_from_file_location('pgrazor_test_axml', Path(__file__).resolve().parent / 'test_axml.py')
test_axml = importlib.util.module_from_spec(test_spec)
test_spec.loader.exec_module(test_axml)
fixture = test_axml.fixture


class InstallPermissionsManifestTest(unittest.TestCase):
    def packaged(self, **options):
        return axml.add_account_restart_activity(axml.patch_manifest(fixture(**options)))

    def test_both_permissions_are_declared_once_before_the_application(self):
        for options in ({}, {'utf8': False}, {'styled': True}):
            with self.subTest(options=options):
                before = self.packaged(**options)
                result = axml.add_install_permissions(before)
                declared = axml.manifest_permissions(result)
                for name in axml.INSTALL_PERMISSIONS:
                    self.assertEqual(declared.count(name), 1)
                _, chunks, pool, _, _ = axml._parse(result)
                order = [name for _, event, name, _, depth in axml._walk(chunks, pool)
                         if event == 'start' and depth == 1 and name in ('uses-permission', 'application')]
                self.assertEqual(order[-1], 'application')
                self.assertEqual(order[-3:-1], ['uses-permission', 'uses-permission'])

    def test_existing_declarations_are_kept(self):
        before = self.packaged()
        result = axml.add_install_permissions(before)
        old, new = axml.inspect_manifest(before), axml.inspect_manifest(result)
        for key in ('package', 'versionCode', 'versionName', 'minSdkVersion', 'providers', 'activities'):
            self.assertEqual(old[key], new[key])
        kept = axml.manifest_permissions(before)
        self.assertEqual(axml.manifest_permissions(result)[:len(kept)], kept)

    def test_a_manifest_that_already_has_them_is_returned_unchanged(self):
        once = axml.add_install_permissions(self.packaged())
        self.assertEqual(axml.add_install_permissions(once), once)

    def test_split_manifests_are_rejected(self):
        with self.assertRaises(ValueError):
            axml.add_install_permissions(fixture(split=True))


if __name__ == '__main__':
    unittest.main()
