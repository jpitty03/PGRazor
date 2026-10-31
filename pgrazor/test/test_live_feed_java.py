"""Run the actual feed parser against Android's org.json implementation, offline.

Uses the installed SDK sources; only Android annotation imports/annotations are
removed in a temporary copy so the same JSON implementation runs on the JVM.
No captured account data, credentials, or network requests are used.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from test_control_java import java_tools

ROOT = Path(__file__).resolve().parents[1]


class LiveFeedJavaTests(unittest.TestCase):
    def test_feed_data(self):
        sdk = Path(os.environ.get('ANDROID_HOME', str(Path(os.environ.get('LOCALAPPDATA', str(Path.home()))) / 'Android/Sdk')))
        candidates = sorted((sdk / 'sources').glob('android-*/org/json'), reverse=True)
        if not candidates:
            self.skipTest('Install Android SDK sources to run the Android JSON parser on the JVM')
        compiler, runtime = java_tools()
        with tempfile.TemporaryDirectory(prefix='pgrazor-feed-test-') as directory:
            work = Path(directory)
            sources = []
            for original in candidates[0].glob('*.java'):
                text = original.read_text(encoding='utf-8')
                text = re.sub(r'^import (?:static )?(?:android\.|libcore\.).*;\s*$', '', text, flags=re.M)
                text = re.sub(r'@(?:UnsupportedAppUsage|SystemApi|NonNull|Nullable)(?:\([^)]*\))?\s*', '', text)
                target = work / original.name
                target.write_text(text, encoding='utf-8')
                sources.append(target)
            sources += [ROOT / 'src/dev/pgrazor/LiveFeedData.java', ROOT / 'src/dev/pgrazor/LiveFeedFilters.java', ROOT / 'src/dev/pgrazor/MapTapSequence.java', ROOT / 'src/dev/pgrazor/BerryPriority.java', ROOT / 'src/dev/pgrazor/SettingsBackupRules.java', ROOT / 'src/dev/pgrazor/BuddyEmotionPreferences.java', ROOT / 'test/LiveFeedDataTest.java', ROOT / 'test/SettingsBackupRulesTest.java']
            if (ROOT / 'test/LiveFeedFiltersTest.java').exists(): sources.append(ROOT / 'test/LiveFeedFiltersTest.java')
            result = subprocess.run([str(compiler), '-encoding', 'UTF-8', '-d', str(work), *map(str, sources)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            for main in ['LiveFeedDataTest','SettingsBackupRulesTest'] + (['LiveFeedFiltersTest'] if (ROOT / 'test/LiveFeedFiltersTest.java').exists() else []):
                result = subprocess.run([str(runtime), '-cp', str(work), main], capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
