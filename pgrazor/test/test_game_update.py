import hashlib
import copy
import importlib.util
from pathlib import Path
import re
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('game_update', ROOT / 'tools/game_update.py')
update = importlib.util.module_from_spec(spec)
spec.loader.exec_module(update)


class GameUpdateTests(unittest.TestCase):
    def test_all_profile_outputs_match_and_references_resolve(self):
        profile = update.current_profile()
        for path, expected in update.generated_outputs(profile).items():
            self.assertEqual(path.read_text(encoding='utf-8'), expected, str(path))
        definitions = update.helper('game_bindings').macros(profile['bindings'])
        referenced = set()
        for path in (ROOT / 'pgrazor/native').rglob('*'):
            if path.suffix not in ('.h', '.cpp', '.inc') or path.name == 'game_bindings.h':
                continue
            referenced.update(re.findall(r'\bPGR_GAME_\w+\b', path.read_bytes().decode('latin1')))
        self.assertTrue(referenced)
        self.assertFalse(referenced - definitions.keys())

    def test_binding_aliases_views_and_invalid_values(self):
        binding = update.helper('game_bindings')
        entries = {'shared': {
            'bytes': {'kind': 'bytes', 'value': ['0x01', '0x02', '0x03', '0x04'], 'purpose': 'fixture'},
            'words': {'kind': 'view', 'value': 'shared.bytes', 'view': 'words_le', 'count': 4, 'purpose': 'fixture'},
            'alias': {'kind': 'alias', 'value': 'shared.words', 'purpose': 'fixture'}}}
        values = binding.macros(entries)
        self.assertEqual(values['PGR_GAME_SHARED_WORDS'], '{0x04030201}')
        self.assertEqual(values['PGR_GAME_SHARED_ALIAS'], '{0x04030201}')
        for target in ['shared.alias', 'shared.missing', 'missing.bytes']:
            changed = copy.deepcopy(entries)
            changed['shared']['alias']['value'] = target
            with self.assertRaises(ValueError):
                binding.macros(changed)
        for count in [0, 3, 5, True]:
            changed = copy.deepcopy(entries)
            changed['shared']['words']['count'] = count
            with self.assertRaises(ValueError):
                binding.macros(changed)
        for kind, value in [('rva', '0x101'), ('offset', '-4'), ('offset', '0x400000'),
                            ('instruction', '1; abort()'), ('bytes', ['0x100']),
                            ('words', ['0x100000000']), ('hex', 'not machine code')]:
            with self.assertRaises(ValueError):
                binding.literal({'kind': kind, 'value': value, 'purpose': 'fixture'})

    def test_address_audit_ignores_comments_and_strings(self):
        binding = update.helper('game_bindings')
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / 'module.cpp'
            source.write_text('// old 0xabc1234\nconst char *s="0xabc1234"; char c=\'"\';\n')
            self.assertEqual(binding.audit(root), [])
            with source.open('a') as stream:
                stream.write('const auto rva=0xabc1234;\n')
            self.assertEqual(len(binding.audit(root)), 1)

    def method(self, **changes):
        return dict(dict(module='Game.dll', owner='Game.Player', name='Capture',
                         **{'return': 'System.Void'}, params=['System.Int32'],
                         flags=6, argc=1, rva=4096), **changes)

    def status(self, old, new):
        return update.compare({'methods': old}, {'methods': new})['methods'][0]['status']

    def test_exact_match_is_only_candidate(self):
        report = update.compare({'methods': [self.method()]}, {'methods': [self.method(rva=8192)]})
        self.assertFalse(report['compatible'])
        self.assertEqual(report['methods'][0]['new_rva'], 8192)
        self.assertEqual(report['methods'][0]['status'], 'candidate')

    def test_changed_abi_and_rename_require_review(self):
        self.assertEqual(self.status([self.method()], [self.method(params=['System.Boolean'])]), 'signature_changed')
        self.assertEqual(self.status([self.method()], [self.method(name='Renamed')]), 'missing_or_renamed')
        self.assertEqual(self.status([self.method()], [self.method(flags=22)]), 'signature_changed')

    def test_duplicate_matches_on_either_side_block(self):
        m = self.method()
        self.assertEqual(self.status([m], [m, m]), 'ambiguous')
        self.assertEqual(self.status([m, m], [m]), 'ambiguous')

    def test_large_overload_groups_keep_report_bounded(self):
        m = self.method()
        row = update.compare({'methods': [m]}, {'methods': [m] * 1000})['methods'][0]
        self.assertEqual(row['status'], 'ambiguous')
        self.assertEqual(row['new_overload_count'], 1000)
        self.assertTrue(row['overloads_truncated'])
        self.assertEqual(len(row['new_overloads']), 10)

    def test_unknown_or_partial_signatures_block(self):
        m = self.method(params=['?'])
        self.assertEqual(self.status([m], [m]), 'incomplete_signature')
        m = self.method(argc=2)
        self.assertEqual(self.status([m], [m]), 'incomplete_signature')
        m = self.method(rva=0)
        self.assertEqual(self.status([m], [m]), 'incomplete_signature')
        for unresolved in ['kind19', 'List<kind30>', 'type#123']:
            m = self.method(params=[unresolved])
            self.assertEqual(self.status([m], [m]), 'incomplete_signature')

    def test_cross_module_identity_not_accepted(self):
        self.assertEqual(self.status([self.method()], [self.method(module='Other.dll')]), 'missing_or_renamed')

    def test_render_rejects_injection_missing_keys_and_invalid_bytes(self):
        for value in [{'kind': 'string', 'value': 'x";danger()'},
                      {'kind': 'bytes', 'value': [256]}, {'kind': 'integer', 'value': True}]:
            with self.assertRaises(ValueError):
                update.render('{{x}}', {'x': value})
        with self.assertRaises(ValueError):
            update.render('{{x}}', {})

    def test_exact_input_hash(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'input.apkm'
            path.write_bytes(b'fixture')
            profile = {'input_sha256': hashlib.sha256(b'fixture').hexdigest(), 'game_version': 'fixture'}
            self.assertTrue(update.verify_input(path, profile)['identity_verified'])
            path.write_bytes(b'changed')
            with self.assertRaises(ValueError):
                update.verify_input(path, profile)

    def test_duplicate_json_rejected(self):
        with self.assertRaises(ValueError):
            update.unique_object([('x', 1), ('x', 2)])

    def test_generated_header_matches(self):
        profile = update.current_profile()
        rendered = update.render((update.SCHEMA / 'catch_wait.h.in').read_text(), profile['modules']['catch_wait'], profile['bindings'])
        self.assertEqual(rendered, (ROOT / 'pgrazor/native/catch_wait_schema.h').read_text())
        tokens = re.sub(r'\s+', '', re.sub(r'//[^\n]*', '', rendered))
        # Golden from the shipped 0.435.1 header before moving it into the profile.
        self.assertEqual(hashlib.sha256(tokens.encode()).hexdigest(),
                         '812ae37f87e2bbe1921932f942cf8043bfdd98884f055051f0aeef783eb4409e')


if __name__ == '__main__':
    unittest.main()
