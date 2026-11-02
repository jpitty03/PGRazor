"""Reproducible game-update checks. Reports candidates, never approves compatibility."""
import argparse
from collections import Counter, defaultdict
import hashlib
import importlib.util
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
SCHEMA = ROOT / 'pgrazor/native/schema/game'


def helper(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'tools' / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def generated_outputs(profile):
    """Validate everything before touching any generated output."""
    outputs = {
        ROOT / 'pgrazor/native/catch_wait_schema.h': render(
            (SCHEMA / 'catch_wait.h.in').read_text(encoding='utf-8'), profile['modules']['catch_wait'], profile['bindings']),
        ROOT / 'pgrazor/native/game_bindings.h': helper('game_bindings').render(profile['bindings']),
    }
    for name in ('routes', 'gifts'):
        source = json.dumps(profile['schemas'][name], indent=2) + '\n'
        generator = helper('native_routes_schema' if name == 'routes' else 'native_gift_schema')
        content = generator.render(source) if name == 'routes' else generator.render(generator.parse(source))
        outputs[generator.SOURCE] = source
        outputs[generator.OUTPUT] = content
    return outputs


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f'Duplicate JSON key: {key}')
        result[key] = value
    return result


def read(path):
    return json.loads(Path(path).read_text(encoding='utf-8'), object_pairs_hook=unique_object)


def current_profile():
    name = read(SCHEMA / 'current.json')['profile']
    if Path(name).name != name or not re.fullmatch(r'[0-9.]+\.json', name):
        raise ValueError('Invalid profile filename')
    profile = read(SCHEMA / name)
    if type(profile['schema_version']) is not int or profile['schema_version'] != 1:
        raise ValueError('Unsupported profile schema')
    if profile['game_version'] + '.json' != name:
        raise ValueError('Profile filename and game version differ')
    if type(profile['version_code']) is not int or profile['version_code'] <= 0:
        raise ValueError('Invalid game version code')
    for key in ('input_sha256', 'library_sha256', 'metadata_sha256', 'game_signer_sha256'):
        if not isinstance(profile[key], str) or not re.fullmatch('[a-f0-9]{64}', profile[key]):
            raise ValueError('Invalid checksum: ' + key)
    return profile


def render(template, values, bindings=None):
    keys = set(re.findall(r'\{\{(\w+)\}\}', template))
    if keys != set(values):
        raise ValueError('Template/profile keys differ')
    definitions = helper('game_bindings').macros(bindings) if bindings is not None else {}

    def literal(match):
        item = values[match[1]]
        if set(item) != {'kind', 'value'}:
            raise ValueError('Invalid binding shape')
        kind, value = item['kind'], item['value']
        if kind == 'binding' and bindings is not None:
            if not isinstance(value, str) or not re.fullmatch(r'[A-Za-z][A-Za-z0-9_]*\.[A-Za-z][A-Za-z0-9_]*', value):
                raise ValueError('Invalid binding reference')
            return definitions['PGR_GAME_' + value.replace('.', '_').upper()]
        if kind == 'integer' and type(value) is int and 0 <= value < 2**64:
            return hex(value)
        if kind == 'string' and isinstance(value, str) and re.fullmatch(r'[A-Za-z0-9_<>.]+', value):
            return json.dumps(value)
        if kind == 'bytes' and isinstance(value, list) and 0 < len(value) <= 256 and all(type(x) is int and 0 <= x <= 255 for x in value):
            return '{' + ','.join(f'0x{x:02x}' for x in value) + '}'
        raise ValueError(f'Invalid binding: {match[1]}')
    return re.sub(r'\{\{(\w+)\}\}', literal, template)


def verify_input(path, profile):
    with Path(path).open('rb') as stream:
        actual = hashlib.file_digest(stream, 'sha256').hexdigest()
    if actual != profile['input_sha256']:
        raise ValueError('Input differs from the selected game profile; do not package with old bindings')
    return {'game_version': profile['game_version'], 'sha256': actual, 'identity_verified': True,
            'gameplay_verified': False}


def signature(method):
    required = ('owner', 'name', 'module', 'return', 'params', 'flags', 'argc', 'rva')
    if any(k not in method for k in required):
        return None
    if not all(isinstance(method[k], str) and method[k] for k in ('module', 'owner', 'name')):
        return None
    if not isinstance(method['params'], list) or type(method['flags']) is not int or type(method['argc']) is not int:
        return None
    types = [method['return'], *method['params']]
    if not all(isinstance(t, str) and t and '?' not in t and 'unknown' not in t.lower()
               and not re.search(r'type#|\bkind\d+\b', t) for t in types):
        return None
    if method['argc'] != len(method['params']) or type(method['rva']) is not int or method['rva'] <= 0:
        return None
    return method['return'], tuple(method['params']), method['flags'], method['argc']


def compare(old, new):
    """Only exact names + complete ABI signatures qualify as candidates.

    Renames, interior patch sites, fields, exports and code bytes require review.
    Metadata tokens and RVAs are deliberately not used as identity across builds.
    """
    index = defaultdict(list)
    exact = defaultdict(list)
    old_counts = Counter((m.get('module'), m.get('owner'), m.get('name'), signature(m)) for m in old['methods'])
    for method in new['methods']:
        identity = (method.get('module'), method.get('owner'), method.get('name'))
        index[identity].append(method)
        exact[(*identity, signature(method))].append(method)
    rows = []
    for method in old['methods']:
        identity = (method.get('module'), method.get('owner'), method.get('name'))
        sig = signature(method)
        candidates = index[identity]
        matches = exact[(*identity, sig)] if sig is not None else []
        status = ('incomplete_signature' if sig is None else
                  'ambiguous' if len(matches) > 1 or old_counts[(*identity, sig)] > 1 else
                  'candidate' if len(matches) == 1 else
                  'signature_changed' if candidates else 'missing_or_renamed')
        row = {'module': identity[0], 'owner': identity[1], 'name': identity[2],
               'old_rva': method.get('rva'), 'status': status}
        if status == 'candidate':
            row['new_rva'] = matches[0]['rva']
        elif status in ('signature_changed', 'ambiguous', 'incomplete_signature'):
            detail_keys = ('rva', 'return', 'params', 'argc', 'flags')
            row['old_signature'] = {k: method.get(k) for k in detail_keys}
            row['new_overload_count'] = len(candidates)
            row['new_overloads'] = [{k: m.get(k) for k in detail_keys} for m in candidates[:10]]
            row['overloads_truncated'] = len(candidates) > 10
        rows.append(row)
    return {'schema_version': 1, 'compatible': False,
            'scope': 'Method identity candidates only; no source modifications or automatic approval.',
            'requires_review': ['code fingerprints', 'field layouts', 'renamed types/methods',
                                'interior patch sites', 'exports', 'live feature behavior'],
            'counts': dict(Counter(r['status'] for r in rows)), 'methods': rows}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    generate = commands.add_parser('generate', help='Generate current version headers')
    generate.add_argument('--check', action='store_true')
    commands.add_parser('audit', help='Find inline address candidates outside the profile')
    inspect = commands.add_parser('verify-input', help='Check exact approved input identity')
    inspect.add_argument('input', type=Path)
    diff = commands.add_parser('compare', help='Compare extracted method-signature snapshots')
    diff.add_argument('--old', required=True, type=Path)
    diff.add_argument('--new', required=True, type=Path)
    diff.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    if args.command == 'compare':
        if args.output.resolve() in (args.old.resolve(), args.new.resolve()):
            raise ValueError('Report must not overwrite an input snapshot')
        report = compare(read(args.old), read(args.new))
        report['snapshots'] = {label: hashlib.sha256(path.read_bytes()).hexdigest()
                               for label, path in [('old', args.old), ('new', args.new)]}
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
        print(json.dumps(report['counts']))
        print('Report written; candidates are NOT verified compatibility.')
    elif args.command == 'audit':
        issues = helper('game_bindings').audit(ROOT / 'pgrazor/native')
        if issues:
            raise ValueError('\n'.join(issues))
        print('No inline address candidates in the audited ARM64 address range.')
    elif args.command == 'verify-input':
        print(json.dumps(verify_input(args.input, current_profile()), indent=2))
    else:
        profile = current_profile()
        outputs = generated_outputs(profile)
        for target, content in outputs.items():
            if args.check:
                if not target.is_file() or target.read_text(encoding='utf-8') != content:
                    raise ValueError(f'{target.relative_to(ROOT)} is stale; run game_update.py generate')
            else:
                target.write_text(content, encoding='utf-8')
        print(f"Game {profile['game_version']}: {len(outputs)} outputs {'checked' if args.check else 'generated'}")


if __name__ == '__main__':
    try:
        main()
    except (ValueError, KeyError, OSError, TypeError) as error:
        raise SystemExit(str(error)) from None
