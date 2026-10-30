"""Neutral metadata validation and pre-retirement generated-table parity."""
import copy
import hashlib
import importlib.util
import json
import tempfile
from pathlib import Path
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]


def generator(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'tools' / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


ROUTES = generator('native_routes_schema')
GIFTS = generator('native_gift_schema')


def body_digest(text):
    # Recorded from the JS-derived checked-in headers BEFORE retiring the JS.
    # Excludes only whole-line comments; all C++ tables, row comments, indices,
    # declarations, ordering, byte values and literal types remain covered.
    body = '\n'.join(line for line in text.splitlines() if not line.startswith('//')) + '\n'
    return hashlib.sha256(body.encode()).hexdigest()


class NativeSchemaTests(unittest.TestCase):
    def setUp(self):
        self.routes = json.loads(ROUTES.SOURCE.read_text(encoding='utf-8'))
        self.gifts = json.loads(GIFTS.SOURCE.read_text(encoding='utf-8'))

    def test_routes_exact_pre_retirement_tables(self):
        source = json.dumps(self.routes)
        classes, indices, fields, methods = ROUTES.parse(source)
        self.assertEqual((len(classes), len(fields), len(methods)), (15, 24, 18))
        self.assertEqual(list(indices.values()), list(range(15)))
        output = ROUTES.render(source)
        self.assertEqual(body_digest(output), 'cc9a188632484c13438cf59cb4794a2167ca6b8b3b04f19c7b22641be6307af0')
        # read_text normalizes line endings so a CRLF checkout of the LF
        # header still matches; raw bytes would differ under core.autocrlf.
        self.assertEqual(output, ROUTES.OUTPUT.read_text(encoding='utf-8'))

    def test_gifts_exact_pre_retirement_tables(self):
        values = GIFTS.parse(json.dumps(self.gifts))
        self.assertEqual((len(values['GIFT_TYPES']), len(values['GIFT_CODE'])), (11, 36))
        output = GIFTS.render(values)
        self.assertEqual(body_digest(output), 'd0c7c14fabc4d1b06e4c42952f2f6c0acfcf57c0d30dc21fdfa56590df1d9c89')
        self.assertEqual(output, GIFTS.OUTPUT.read_text(encoding='utf-8'))

    def test_production_inputs_are_neutral_json(self):
        for module in (ROUTES, GIFTS):
            self.assertEqual(module.SOURCE.parent, ROOT / 'pgrazor/native/schema')
            self.assertEqual(module.SOURCE.suffix, '.json')

    def test_routes_explicit_array_order_controls_indices(self):
        self.routes['classes'][0], self.routes['classes'][1] = self.routes['classes'][1], self.routes['classes'][0]
        _, indices, _, _ = ROUTES.parse(json.dumps(self.routes))
        self.assertEqual(indices[self.routes['classes'][0]['key']], 0)
        self.assertEqual(indices[self.routes['classes'][1]['key']], 1)

    def test_routes_reject_invalid_descriptors(self):
        mutations = [
            lambda v: v.update(version=2),
            lambda v: v.update(version=True),
            lambda v: v.update(extra='unexpected'),
            lambda v: v['classes'][0].update(canonical='wrong.Type'),
            lambda v: v['classes'].append(copy.deepcopy(v['classes'][0])),
            lambda v: v['fields'][0].update(owner='missing'),
            lambda v: v['fields'][0].update(offset=True),
            lambda v: v['fields'][0].update(offset=1 << 64),
            lambda v: v['fields'][0].update(kind=True),
            lambda v: v['fields'][0].update(size=99),
            lambda v: v['fields'].append(copy.deepcopy(v['fields'][0])),
            lambda v: v['methods'][0].update(params=['A', 'B']),
            lambda v: v['methods'][0].update(params=[None]),
            lambda v: v['methods'][0].update(name='bad\nname'),
            lambda v: v['methods'].append(copy.deepcopy(v['methods'][0])),
        ]
        for index, mutate in enumerate(mutations):
            with self.subTest(index=index):
                value = copy.deepcopy(self.routes)
                mutate(value)
                with self.assertRaises(ValueError):
                    ROUTES.parse(json.dumps(value))

    def test_gifts_reject_invalid_descriptors(self):
        code_key = next(iter(self.gifts['codes']))
        mutations = [
            lambda v: v.update(version=2),
            lambda v: v.update(version=True),
            lambda v: v.update(extra='unexpected'),
            lambda v: v['types'].update({'01': 'Bad.Type'}),
            lambda v: v['types'].update({'99': 'Bad\nType'}),
            lambda v: v['codes'].update({'bad': [4, '00']}),
            lambda v: v['codes'].update({code_key: [True, '00']}),
            lambda v: v['codes'].update({code_key: [3, '00']}),
            lambda v: v['codes'].update({code_key: [1 << 53, '00']}),
            lambda v: v['codes'].update({code_key: [4, '0g']}),
            lambda v: v['codes'].update({code_key: [4, '00' * 81]}),
            lambda v: v.update(open_sequence=[0, '00']),
        ]
        for index, mutate in enumerate(mutations):
            with self.subTest(index=index):
                value = copy.deepcopy(self.gifts)
                mutate(value)
                with self.assertRaises(ValueError):
                    GIFTS.parse(json.dumps(value))

    def test_check_tolerates_crlf_checkout(self):
        # Regression: a fresh Windows checkout under core.autocrlf materializes
        # the checked-in LF headers with CRLF (no eol=lf pin for *.h). The
        # generators' --check must still match so CI does not report a spurious
        # schema mismatch (observed as a Routes failure on Windows runners).
        cases = (
            (ROUTES, ROUTES.render(json.dumps(self.routes))),
            (GIFTS, GIFTS.render(GIFTS.parse(json.dumps(self.gifts)))),
        )
        for module, generated in cases:
            with self.subTest(module=module.__name__):
                with tempfile.TemporaryDirectory() as tmp:
                    header = Path(tmp) / 'header.h'
                    header.write_bytes(generated.replace('\n', '\r\n').encode('utf-8'))
                    with mock.patch.object(module, 'OUTPUT', header), \
                         mock.patch('sys.argv', [module.__name__, '--check']), \
                         mock.patch('sys.stdout'), mock.patch('sys.stderr'):
                        # main() returns 0 (routes) or None (gifts) on a match
                        # and must not signal a mismatch for the CRLF header.
                        self.assertIn(module.main(), (0, None))

    def test_duplicate_json_keys_rejected(self):
        for module, value in ((ROUTES, self.routes), (GIFTS, self.gifts)):
            source = json.dumps(value).replace('"version": 1', '"version": 1, "version": 1', 1)
            with self.subTest(module=module.__name__), self.assertRaises(ValueError):
                module.parse(source)


if __name__ == '__main__':
    unittest.main()
