"""Species catalog extraction and Java filtering semantics."""
from pathlib import Path
import importlib.util
import json
import hashlib
import re
import subprocess
import tempfile
import unittest
from test_control_java import java_tools

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('pokemon_categories_generator', ROOT/'generate_pokemon_categories.py')
generator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generator)


def row(template, **settings):
    symbol = template.split('_POKEMON_')[-1].removesuffix('_NORMAL')
    return {'templateId': template, 'data': {'pokemonSettings': {'pokemonId': symbol, **settings}}}


class PokemonCategoriesTests(unittest.TestCase):
    def test_displayed_species_and_categories_have_names(self):
        # This fails on the old 1-1008 table even though every Java filter test passes.
        raw = (ROOT/'assets/map-icons/pokemon-names.json').read_bytes()
        names = json.loads(raw)
        picker = (ROOT/'src/dev/pgrazor/PokemonSpeciesDialog.java').read_text(encoding='utf-8')
        maximum = int(re.search(r'DISPLAY_MAX\s*=\s*(\d+)', picker)[1])
        required = set(range(1, maximum+1))
        source = (ROOT/'src/dev/pgrazor/PokemonCategories.java').read_text()
        for body in re.findall(r'int\[\] \w+_IDS = \{(.*?)\};', source, re.S):
            required.update(map(int, re.findall(r'\d+', body)))
        self.assertEqual([], [dex for dex in sorted(required)
            if not names.get(str(dex), '').strip() or names[str(dex)].startswith('#')])
        self.assertEqual('Iron Boulder', names['1022'])
        self.assertEqual('Iron Crown', names['1023'])
        provenance = json.loads((ROOT/'assets/map-icons/provenance.json').read_text())['species_names']
        self.assertEqual(provenance['output_sha256'], hashlib.sha256(raw).hexdigest())
        self.assertEqual(provenance['entries'], len(names))

    def test_evolution_stages_include_babies_and_ignore_temporary_forms(self):
        rows = [row('V0172_POKEMON_PICHU'),
                row('V0025_POKEMON_PIKACHU', parentPokemonId='PICHU'),
                row('V0026_POKEMON_RAICHU', parentPokemonId='PIKACHU'),
                row('V0026_POKEMON_RAICHU_ALOLA', pokemonId='RAICHU', parentPokemonId='PIKACHU'),
                row('V0133_POKEMON_EEVEE'),
                row('V0134_POKEMON_VAPOREON', parentPokemonId='EEVEE'),
                row('V0196_POKEMON_ESPEON', parentPokemonId='EEVEE'),
                row('V0150_POKEMON_MEWTWO', tempEvoOverrides=[{'tempEvoId':'TEMP_EVOLUTION_MEGA_X'}])]
        groups = generator.categories(rows)
        self.assertEqual({25, 134, 196}, groups['STAGE_2'])
        self.assertEqual({26}, groups['STAGE_3'])
        self.assertEqual({150}, groups['MEGA'])

    def test_broken_evolution_graph_is_rejected(self):
        cases = [
            [row('V0002_POKEMON_IVYSAUR', parentPokemonId='MISSING')],
            [row('V0001_POKEMON_BULBASAUR', parentPokemonId='IVYSAUR'),
             row('V0002_POKEMON_IVYSAUR', parentPokemonId='BULBASAUR')],
            [row('V0002_POKEMON_IVYSAUR', parentPokemonId='BULBASAUR'),
             row('V0002_POKEMON_IVYSAUR_NORMAL', parentPokemonId='VENUSAUR')],
        ]
        for rows in cases:
            with self.subTest(rows=rows), self.assertRaises(ValueError):
                generator.evolution_stages(rows)

    def test_forms_dedupe_and_overlapping_groups(self):
        groups = generator.categories([
            row('V0150_POKEMON_MEWTWO', pokemonClass='POKEMON_CLASS_LEGENDARY',
                tempEvoOverrides=[{'tempEvoId': 'TEMP_EVOLUTION_MEGA_X'}, {'tempEvoId': 'TEMP_EVOLUTION_MEGA_Y'}]),
            row('V0150_POKEMON_MEWTWO_NORMAL', pokemonClass='POKEMON_CLASS_LEGENDARY'),
            row('V0382_POKEMON_KYOGRE', pokemonClass='POKEMON_CLASS_LEGENDARY',
                tempEvoOverrides=[{'tempEvoId': 'TEMP_EVOLUTION_PRIMAL'}]),
            row('V0151_POKEMON_MEW', pokemonClass='POKEMON_CLASS_MYTHIC'),
            row('V0793_POKEMON_NIHILEGO', pokemonClass='POKEMON_CLASS_ULTRA_BEAST'),
            row('V0004_POKEMON_CHARMANDER'),
        ])
        self.assertEqual(groups['LEGENDARY'], {150, 382})
        self.assertEqual(groups['MEGA'], {150})
        self.assertEqual(groups['PRIMAL'], {382})
        self.assertEqual(groups['MYTHICAL'], {151})
        self.assertEqual(groups['ULTRA_BEAST'], {793})

    def test_unrecognized_schema_requires_review(self):
        for entry in (row('not-a-dex-template'), row('V2001_POKEMON_EXAMPLE'),
                      row('V0001_POKEMON_EXAMPLE', pokemonClass='NEW_CLASS'),
                      row('V0001_POKEMON_EXAMPLE', tempEvoOverrides=[{'tempEvoId': 'NEW_EVOLUTION'}])):
            with self.subTest(entry=entry), self.assertRaises(ValueError):
                generator.categories([entry])

    def test_java_filter_contract(self):
        compiler, runtime = java_tools()
        with tempfile.TemporaryDirectory(prefix='pgrazor-categories-') as work:
            sources = [ROOT/'src/dev/pgrazor/PokemonCategories.java',
                       ROOT/'src/dev/pgrazor/SmartCatchPreferences.java', ROOT/'test/PokemonCategoriesTest.java']
            result = subprocess.run([str(compiler), '-encoding', 'UTF-8', '-d', work, *map(str, sources)],
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
            result = subprocess.run([str(runtime), '-cp', work, 'dev.pgrazor.PokemonCategoriesTest'],
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)


if __name__ == '__main__':
    unittest.main()
