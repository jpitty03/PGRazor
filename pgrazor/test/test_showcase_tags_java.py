import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

class ShowcaseTagStoreTests(unittest.TestCase):
    def test_durable_ownership(self):
        with tempfile.TemporaryDirectory(prefix='showcase-tags-') as output:
            subprocess.run(['javac', '-encoding', 'UTF-8', '-d', output,
                str(ROOT/'src/dev/pgrazor/ShowcaseTagStore.java'),
                str(ROOT/'test/ShowcaseTagStoreTest.java')], check=True)
            subprocess.run(['java', '-cp', output, 'dev.pgrazor.ShowcaseTagStoreTest'], check=True)

if __name__ == '__main__':
    unittest.main()
