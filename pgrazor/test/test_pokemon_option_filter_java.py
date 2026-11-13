"""Run catalog filtering against the production matcher and real category table."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_control_java import java_tools

ROOT = Path(__file__).resolve().parents[1]


class PokemonOptionFilterTests(unittest.TestCase):
    def test_catalog_filters(self):
        compiler, runtime = java_tools()
        sources = [ROOT / 'src/dev/pgrazor/PokemonCategories.java',
                   ROOT / 'src/dev/pgrazor/PokemonOptionFilter.java',
                   ROOT / 'test/PokemonOptionFilterTest.java']
        with tempfile.TemporaryDirectory(prefix='pgrazor-option-filter-') as work:
            result = subprocess.run([str(compiler), '-encoding', 'UTF-8', '-d', work,
                                     *map(str, sources)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(runtime), '-cp', work, 'dev.pgrazor.PokemonOptionFilterTest'],
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
