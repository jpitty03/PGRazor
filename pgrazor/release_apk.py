"""Create a single installable PGRazor APK from a validated ARM64 package run.
Only library-only configuration splits can be merged; resource/code splits fail closed.
"""
import argparse
import copy
import json
import os
from pathlib import Path
import re
import shutil
import struct
import zipfile
import axml
import build
import nonroot
import runtime_assets


def standalone_manifest(data):
    header, chunks, pool, _, _ = axml._parse(data)
    remove = set()
    pending = None
    for i, event, name, attrs, depth in axml._walk(chunks, pool):
        if event == 'start' and (name == 'uses-split' or (name == 'meta-data' and attrs.get((axml.ANDROID_NS, 'name')) in ('com.android.vending.splits', 'com.android.vending.splits.required'))):
            pending = (i, depth)
        if pending and event == 'end' and depth == pending[1]:
            remove.update(range(pending[0], i+1)); pending = None
    output = []
    for i, (kind, original) in enumerate(chunks):
        if i in remove: continue
        raw = bytearray(original)
        if kind == 0x102:
            h = axml._u16(raw, 2)
            name = pool.string(axml._u32(raw, h+4))
            start, size, count = struct.unpack_from('<HHH', raw, h+8)
            brand_label = name == 'application'
            if name == 'activity':
                for index in range(count):
                    at = h+start+index*size
                    if pool.string(axml._u32(raw,at+4)) == 'name' and raw[at+15] == 3:
                        brand_label = pool.string(axml._u32(raw,at+16)) == 'com.nianticproject.holoholo.libholoholo.unity.UnityMainActivity'
            kept, indexes = [], {}
            for index in range(count):
                at = h+start+index*size; attr = bytearray(raw[at:at+size])
                ns, key = pool.string(axml._u32(attr, 0)), pool.string(axml._u32(attr, 4))
                if ns == axml.ANDROID_NS and key in ('requiredSplitTypes', 'splitTypes', 'isSplitRequired'): continue
                if brand_label and ns == axml.ANDROID_NS and key == 'label':
                    label = pool.add('Pok\u00e9mon GO'); struct.pack_into('<I', attr, 8, label); struct.pack_into('<HBBI', attr, 12, 8, 0, 3, label)
                indexes[index+1] = len(kept)+1; kept.append(attr)
            prefix = raw[:h+start]
            struct.pack_into('<H', prefix, h+12, len(kept))
            for offset in (14, 16, 18):
                old = axml._u16(prefix, h+offset); struct.pack_into('<H', prefix, h+offset, indexes.get(old, 0))
            raw = prefix+b''.join(kept)+raw[h+start+size*count:]
            struct.pack_into('<I', raw, 4, len(raw))
        output.append((kind, bytes(raw)))
    output[0] = (1, pool.build())
    body = b''.join(raw for _, raw in output)
    prefix = bytearray(data[:header]); struct.pack_into('<I', prefix, 4, header+len(body))
    result = bytes(prefix)+body
    info = axml.inspect_manifest(result)
    if info['split']: raise ValueError('Standalone output still declares a split')
    return result


def merge(apks, output):
    package, base, rows, abis = nonroot.inspect_set(apks)
    if package != 'com.nianticlabs.pokemongo' or abis != {'arm64-v8a'}:
        raise ValueError('Release supports the ARM64 Pokemon GO APK set only')
    extras = []
    for path, info in rows:
        if path == base: continue
        if info['split'] != 'config.arm64_v8a': raise ValueError('Unsupported split: '+str(info['split']))
        with zipfile.ZipFile(path) as archive:
            for entry in archive.infolist():
                if entry.is_dir() or entry.filename == 'AndroidManifest.xml' or nonroot.signature_entry(entry.filename): continue
                if not re.fullmatch(r'lib/arm64-v8a/[A-Za-z0-9_.+-]+\.so', entry.filename):
                    raise ValueError('Cannot merge resource/code split entry: '+entry.filename)
                extras.append((path, entry))
    with zipfile.ZipFile(base) as source, zipfile.ZipFile(output, 'w') as target:
        seen = set()
        for entry in source.infolist():
            if nonroot.signature_entry(entry.filename): continue
            seen.add(entry.filename)
            if entry.filename == 'AndroidManifest.xml':
                target.writestr(copy.copy(entry), standalone_manifest(source.read(entry))); continue
            with source.open(entry) as reader, target.open(copy.copy(entry), 'w') as writer: shutil.copyfileobj(reader, writer)
        for path, entry in extras:
            if entry.filename in seen: raise ValueError('Duplicate split member: '+entry.filename)
            seen.add(entry.filename)
            with zipfile.ZipFile(path) as archive, archive.open(entry) as reader, target.open(copy.copy(entry), 'w') as writer: shutil.copyfileobj(reader, writer)
        if 'lib/arm64-v8a/libil2cpp.so' not in seen: raise ValueError('Game runtime missing')


def release(run, destination):
    run = Path(run).resolve(); destination = Path(destination).resolve()
    report = json.loads((run/'report.json').read_text(encoding='utf8'))
    apks = []
    for row in report['outputs']:
        path = Path(row['file']).resolve()
        if path.parent != run or runtime_assets.sha256(path) != row['sha256']: raise ValueError('Build receipt mismatch')
        apks.append(path)
    _, base, rows, _ = nonroot.inspect_set(apks)
    identity = next(info for path, info in rows if path == base)
    game_version = identity.get('versionName')
    game_code = identity.get('versionCode')
    if not isinstance(game_version, str) or not game_version or not isinstance(game_code, int):
        raise ValueError('Base game must declare versionName and numeric versionCode')
    destination.mkdir(parents=True, exist_ok=True)
    unsigned, aligned, signed = (destination/name for name in ('unsigned.apk', 'aligned.apk', 'PGRazor.apk'))
    merge(apks, unsigned)
    nonroot.command(build.TOOLS/'zipalign.exe', '-f', '-P', '16', '4', unsigned, aligned)
    nonroot.command(build.TOOLS/'apksigner.bat', 'sign', *build.signing_args(), '--out', signed, aligned)
    nonroot.command(build.TOOLS/'apksigner.bat', 'verify', signed)
    nonroot.command(build.TOOLS/'zipalign.exe', '-c', '-P', '16', '4', signed)
    (destination/'SHA256SUMS.txt').write_text(runtime_assets.sha256(signed)+'  PGRazor.apk\n', encoding='utf8')
    (destination/'build-info.json').write_text(json.dumps({'game_version':game_version,'game_version_code':game_code,'architecture':'arm64-v8a','hook_runtime_architectures':report.get('runtime_abis',[report.get('runtime_abi','arm64-v8a')]),'input_sha256':report['input_sha256'],'apk_sha256':runtime_assets.sha256(signed),'source_commit':os.environ.get('GITHUB_SHA'),'device_tested':False},indent=2)+'\n',encoding='utf8')
    unsigned.unlink(); aligned.unlink()
    print('Release candidate:', signed)
    return signed

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run', type=Path, required=True)
    parser.add_argument('--output', type=Path, default=build.BUILD/'release')
    args = parser.parse_args(); release(args.run, args.output)
