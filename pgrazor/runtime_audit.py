"""Audit an actual local APK for production runtime retirement evidence.

No filename obfuscation is credited: scan decompressed assets/ELFs for runtime
markers too. This is an inventory, not proof that compiled code is unrecoverable.
Use --require-retired as the final packaging gate once all replacements work.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import zipfile


def audit(path):
    with path.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    report = {'apk': str(path.resolve()), 'sha256': digest,
              'production_scripts': [], 'interpreter_artifacts': [], 'native_libraries': [],
              'bootstrap_dependencies': [], 'runtime_retired': False, 'production_payload_retired': False, 'mode': {}}
    markers = (b'frida_gadget', b'frida-gadget', b'gum_interceptor', b'gum_script_backend',
               b'js_eval', b'quickjs')
    bootstrap = (b'libfrida-gadget.so', b'java-bridge.js', b'embedded-agent.js',
                 b'quick-close.js', b'quick-ball.js', b'quick-summary.js', b'Script.evaluate')
    with zipfile.ZipFile(path) as archive:
        names = archive.namelist()
        if 'assets/pgrazor/mode.json' in names:
            report['mode'] = json.loads(archive.read('assets/pgrazor/mode.json'))
        if len(names) != len(set(names)):
            raise ValueError('Duplicate APK entries')
        for item in archive.infolist():
            name = item.filename
            production = name.startswith('assets/pgrazor/')
            dex = re.fullmatch(r'classes\d*\.dex', name) is not None
            native = name.startswith('lib/') and name.endswith('.so')
            if not production and not dex and not native:
                continue
            if item.file_size > 256 * 1024 * 1024:
                raise ValueError('Runtime audit entry exceeds bounded size: ' + name)
            data = archive.read(item)
            if production and (name.endswith(('.js', '.mjs', '.cjs')) or
                               b'Interceptor.attach(' in data or b'Script.evaluate(' in data):
                report['production_scripts'].append({'entry': name, 'bytes': len(data),
                    'sha256': hashlib.sha256(data).hexdigest()})
            if (production or native) and data[:4] == b'\x7fELF':
                if len(data) < 64 or data[4:6] != b'\x02\x01':
                    raise ValueError('Unexpected runtime ELF format: ' + name)
                report['native_libraries'].append({'entry': name, 'bytes': len(data),
                    'machine': int.from_bytes(data[18:20], 'little'),
                    'sha256': hashlib.sha256(data).hexdigest()})
            matches = [marker.decode('ascii') for marker in markers if marker in data.lower()]
            if matches:
                # Dependency notices are retained while libraries ship, but are not
                # executable interpreter evidence by themselves.
                report['interpreter_artifacts'].append({'entry': name, 'markers': matches,
                    'notice_only': '/bridge-notices/' in name or '/native-notices/' in name})
            if dex:
                references = [marker.decode('ascii') for marker in bootstrap if marker in data]
                if references:
                    report['bootstrap_dependencies'].append({'entry': name, 'references': references})
    report['runtime_retired'] = not (report['production_scripts'] or report['bootstrap_dependencies'] or
        any(not row['notice_only'] for row in report['interpreter_artifacts']))
    mode = report['mode']
    report['production_payload_retired'] = (mode.get('compiledGameplay') is True
        and mode.get('gadget') is False and not mode.get('researchRuntime', False)
        and not report['production_scripts']
        and not any(not row['notice_only'] and not re.fullmatch(r'classes\d*\.dex', row['entry'])
                    for row in report['interpreter_artifacts']))
    report['limits'] = ('Static inventory only. No proof of functional parity, cold-start behavior, '
        'absence of encrypted/renamed interpreters, or protection of embedded secrets. '
        'DEX research loader references are reported even when production mode disables them; '
        'production_payload_retired checks payload and mode, not removal of dormant loader code. '
        'Native integration and live checks are separate requirements.')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('apk', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--require-retired', action='store_true')
    parser.add_argument('--require-production-assets', action='store_true', help='Require compiled mode and no research executable payload; still report dormant DEX references')
    args = parser.parse_args()
    result = audit(args.apk)
    args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print('Runtime retired:', result['runtime_retired'])
    print('Production script assets:', len(result['production_scripts']))
    print('Bootstrap DEX dependencies:', len(result['bootstrap_dependencies']))
    print('Receipt:', args.output)
    return int((args.require_retired and not result['runtime_retired']) or
               (args.require_production_assets and not result['production_payload_retired']))


if __name__ == '__main__':
    raise SystemExit(main())
