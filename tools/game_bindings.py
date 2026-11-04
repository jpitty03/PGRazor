"""Generate C/C++ binding literals without moving runtime logic into configuration."""
import json
import re

NAME = re.compile(r'[a-zA-Z][a-zA-Z0-9_]*\Z')
NUMBER = re.compile(r'(0x[0-9a-fA-F]+|0|[1-9][0-9]*)([uU](?:ll|LL|l|L)?|(?:ll|LL|l|L)[uU]?)?\Z')
LEXER = re.compile(r'''//[^\n]*|/\*[\s\S]*?\*/|'(?:[^'\\]|\\.)*'|"(?:[^"\\]|\\.)*"|\b(?:0x[0-9a-fA-F]+|[0-9]+)(?:ULL|ull|UL|ul|LL|ll|u|U|L)?\b''')
GENERATED = {'game_bindings.h', 'catch_wait_schema.h', 'routes_schema.h', 'shared_gift_code.h'}


def audit(native_root):
    """Catch newly inlined pins in this game's address range, not arbitrary C++ semantics."""
    issues = []
    for path in sorted(native_root.rglob('*')):
        if path.suffix not in ('.h', '.cpp', '.inc') or 'test' in path.name or path.name in GENERATED:
            continue
        raw = path.read_bytes()
        try:
            source = raw.decode('utf-8')
        except UnicodeDecodeError:
            source = raw.decode('cp1252')
        for match in LEXER.finditer(source):
            try:
                value = number(match[0])
            except ValueError:
                continue
            if not 0x4000000 < value < 0x10000000:
                continue
            # This duration is a day in milliseconds, not a method address.
            if path.name == 'gym_runtime.cpp' and match[0] == '86400000':
                continue
            issues.append(f'{path.relative_to(native_root)}:{source.count(chr(10), 0, match.start()) + 1}: inline address candidate {match[0]}')
    return issues


def number(value):
    if not isinstance(value, str) or not (match := NUMBER.fullmatch(value)):
        raise ValueError('Expected a C integer literal')
    result = int(match[1], 16 if match[1].startswith('0x') else 10)
    if result >= 2**64:
        raise ValueError('Integer binding exceeds uint64')
    return result


def literal(entry):
    if set(entry) != {'kind', 'value', 'purpose'} or not isinstance(entry['purpose'], str) or not entry['purpose'].strip():
        raise ValueError('Binding needs kind, value and purpose')
    kind, value = entry['kind'], entry['value']
    if kind in ('rva', 'offset', 'instruction', 'layout'):
        parsed = number(value)
        if kind == 'rva' and (parsed == 0 or parsed % 4):
            raise ValueError('ARM64 RVA must be nonzero and aligned')
        if kind == 'offset' and parsed > 0x100000:
            raise ValueError('Unexpected field offset')
        return value
    if kind == 'name':
        if not isinstance(value, str) or not value or len(value) > 1024 or any(ord(c) < 32 or ord(c) > 126 for c in value):
            raise ValueError('Invalid metadata name')
        return json.dumps(value, ensure_ascii=True)
    if kind == 'hex':
        if not isinstance(value, str) or not re.fullmatch(r'(?:[0-9a-fA-F]{2}){1,256}', value):
            raise ValueError('Invalid hex fingerprint')
        return json.dumps(value)
    if kind in ('bytes', 'words'):
        if not isinstance(value, list) or not 0 < len(value) <= 256:
            raise ValueError('Invalid fingerprint size')
        limit = 256 if kind == 'bytes' else 2**32
        if any(number(v) >= limit for v in value):
            raise ValueError('Invalid fingerprint value')
        return '{' + ','.join(value) + '}'
    raise ValueError(f'Unknown binding kind: {kind}')


def macros(bindings):
    result = {}
    def resolve(module, key, active=()):
        path = module + '.' + key
        if path in active:
            raise ValueError('Binding alias cycle: ' + path)
        entry = bindings[module][key]
        if entry.get('kind') in ('alias', 'view'):
            required = {'kind', 'value', 'purpose'}
            if entry['kind'] == 'view':
                required |= {'view', 'count'}
            if set(entry) != required or not isinstance(entry['purpose'], str) or not entry['purpose'] or not isinstance(entry['value'], str):
                raise ValueError('Invalid alias')
            target = entry['value'].split('.')
            if len(target) != 2 or target[0] not in bindings or target[1] not in bindings[target[0]]:
                raise ValueError('Missing alias target: ' + entry['value'])
            resolved = resolve(*target, (*active, path))
            if entry['kind'] == 'alias':
                return resolved
            if not resolved.startswith('{') or not resolved.endswith('}'):
                raise ValueError('Fingerprint view needs a byte-array source')
            data = bytes(number(v) for v in resolved[1:-1].split(','))
            count = entry['count']
            if type(count) is not int or not 0 < count <= len(data):
                raise ValueError('Invalid fingerprint view length')
            data = data[:count]
            if entry['view'] == 'hex':
                return json.dumps(data.hex())
            if entry['view'] == 'bytes':
                return '{' + ','.join(f'0x{v:02x}' for v in data) + '}'
            if entry['view'] == 'words_le' and len(data) % 4 == 0:
                return '{' + ','.join(f'0x{int.from_bytes(data[i:i+4], "little"):08x}' for i in range(0, len(data), 4)) + '}'
            raise ValueError('Invalid fingerprint view')
        return literal(entry)
    for module, entries in bindings.items():
        if not NAME.fullmatch(module) or not entries:
            raise ValueError('Invalid or empty binding module')
        for key, entry in entries.items():
            if not NAME.fullmatch(key):
                raise ValueError('Invalid binding key')
            name = f'PGR_GAME_{module}_{key}'.upper()
            if name in result:
                raise ValueError(f'Colliding binding name: {name}')
            result[name] = resolve(module, key)
    return result


def render(bindings):
    lines = ['#pragma once', '// Generated by tools/game_update.py generate. Do not edit.',
             '// Source: selected native/schema/game version profile.',
             '// Literal macros preserve C enum, array initialization and C++ ABI behavior.', '']
    lines += [f'#define {name} {value}' for name, value in macros(bindings).items()]
    return '\n'.join(lines) + '\n'
