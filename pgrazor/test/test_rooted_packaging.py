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


if __name__ == '__main__':
    unittest.main()
