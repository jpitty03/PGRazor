"""Exercise release rejection with real ZIPs and mismatched publication artifacts."""
import json
from pathlib import Path
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
import verify_release


class ReleaseBundleTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name)
        self.tag = 'v0.2.6'
        with zipfile.ZipFile(self.folder / 'PGRazor.apk', 'w') as apk:
            apk.writestr('assets/pgrazor/mode.json', json.dumps({'version': '17.24.2', 'pgrazorVersion': self.tag}))
        with zipfile.ZipFile(self.folder / 'PGRazor-Root-module.zip', 'w') as module:
            module.writestr('module.prop', 'id=pgrazor\nversion=' + self.tag + '\n')
        self.installer(self.folder / 'PGRazor-Root-module.zip')
        self.receipts()

    def installer(self, module):
        with zipfile.ZipFile(self.folder / 'PGRazor-Root.apk', 'w') as apk:
            apk.write(module, 'assets/module.zip')

    def receipts(self):
        hashes = {name: verify_release.digest(self.folder / name) for name in verify_release.DOWNLOADS}
        (self.folder / 'SHA256SUMS.txt').write_text(''.join(h + '  ' + n + '\n' for n, h in hashes.items()))
        (self.folder / 'build-info.json').write_text(json.dumps({
            'game_version': '0.435.2', 'game_version_code': 2026110702, 'apk_sha256': hashes['PGRazor.apk']}))
        (self.folder / 'root-build-info.json').write_text(json.dumps({
            'version': self.tag, 'target': {'versionName': '0.435.2', 'versionCode': 2026110702},
            'installer_sha256': hashes['PGRazor-Root.apk'], 'module_sha256': hashes['PGRazor-Root-module.zip']}))

    def test_complete_bundle(self):
        self.assertEqual(set(verify_release.verify(self.folder, self.tag)), set(verify_release.DOWNLOADS))

    def test_missing_download_blocks_publication(self):
        (self.folder / 'PGRazor-Root-module.zip').unlink()
        with self.assertRaisesRegex(ValueError, 'Missing'):
            verify_release.verify(self.folder, self.tag)

    def test_corrupted_download_blocks_publication(self):
        with (self.folder / 'PGRazor.apk').open('ab') as stream:
            stream.write(b'changed')
        with self.assertRaisesRegex(ValueError, 'checksums'):
            verify_release.verify(self.folder, self.tag)

    def test_installer_with_different_module_even_when_receipts_match(self):
        other = self.folder / 'old.zip'
        other.write_bytes(b'old module')
        self.installer(other)
        self.receipts()
        with self.assertRaisesRegex(ValueError, 'different module'):
            verify_release.verify(self.folder, self.tag)

    def test_wrong_game_target(self):
        path = self.folder / 'root-build-info.json'
        value = json.loads(path.read_text())
        value['target']['versionCode'] += 1
        path.write_text(json.dumps(value))
        with self.assertRaisesRegex(ValueError, 'game targets'):
            verify_release.verify(self.folder, self.tag)

    def test_stale_nonroot_apk_even_when_receipts_match(self):
        with zipfile.ZipFile(self.folder / 'PGRazor.apk', 'w') as apk:
            apk.writestr('assets/pgrazor/mode.json', json.dumps({'version': '17.24.2', 'pgrazorVersion': 'v0.2.5'}))
        self.receipts()
        with self.assertRaisesRegex(ValueError, 'Non-rooted build version'):
            verify_release.verify(self.folder, self.tag)

    def test_wrong_release_tag(self):
        with self.assertRaisesRegex(ValueError, 'Rooted build version'):
            verify_release.verify(self.folder, 'v0.2.7')


if __name__ == '__main__':
    unittest.main()
