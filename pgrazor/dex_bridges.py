"""Build-time Java bridge migration using pinned dexlib2 (never shipped in the APK)."""
from pathlib import Path
import hashlib
import json
import os
import subprocess
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / 'tools/dex_bridge'
CACHE = ROOT / '.artifacts/native-migration/dex-tools'


def sha256(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def dependencies(download=False):
    jars = []
    for item in json.loads((TOOLS / 'dependencies.json').read_text(encoding='utf-8')):
        path = CACHE / item['url'].rsplit('/', 1)[1]
        if not path.is_file():
            if not download:
                raise RuntimeError('Missing pinned DEX build dependency: ' + path.name)
            data = urllib.request.urlopen(item['url'], timeout=60).read()
            if hashlib.sha256(data).hexdigest() != item['sha256']:
                raise RuntimeError('Downloaded DEX dependency checksum mismatch')
            CACHE.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        if path.is_symlink() or sha256(path) != item['sha256']:
            raise RuntimeError('DEX build dependency checksum mismatch: ' + path.name)
        jars.append(path)
    return jars


def rewrite(source, work, *, download=False):
    """Return replacement DEX paths only after all twelve exact targets validate.

    Caller owns packaging, bootstrap selection and final ART verification. Never
    modifies the input APK. Missing or changed descriptors are a packaging error.
    """
    source, work = Path(source).resolve(strict=True), Path(work).resolve()
    if work.exists():
        raise ValueError('DEX rewrite work directory must be new')
    input_hash = sha256(source)
    jars = dependencies(download)
    work.mkdir(parents=True)
    classes = work / 'tool-classes'
    classes.mkdir()
    classpath = os.pathsep.join(map(str, jars))
    tool = TOOLS / 'BridgeRewriter.java'
    subprocess.run(['javac', '-encoding', 'UTF-8', '-cp', classpath, '-d', str(classes), str(tool)], check=True, timeout=90)
    output = work / 'rewritten'
    subprocess.run(['java', '-cp', classpath + os.pathsep + str(classes), 'BridgeRewriter', str(source), str(output)], check=True, timeout=180)
    targets = (output / 'bridges.tsv').read_text(encoding='utf-8').splitlines()
    if len(targets) != 12 or len(set(targets)) != 12:
        raise RuntimeError('Incomplete DEX rewrite receipt')
    locations = (output / 'locations.tsv').read_text(encoding='utf-8').splitlines()
    location_counts = {}
    for row in locations:
        columns = row.split('\t')
        if columns[0] == '#count':
            if len(columns) != 3 or columns[1] in location_counts:
                raise RuntimeError('Invalid Location rewrite counts')
            location_counts[columns[1]] = int(columns[2])
        elif len(columns) != 8 or columns[0] not in ('rewritten', 'ignored-owner', 'ignored-descriptor'):
            raise RuntimeError('Invalid Location rewrite inventory')
    if set(location_counts) != {'rewritten', 'ignored', 'unsupported'} or location_counts['unsupported']:
        raise RuntimeError('Incomplete Location rewrite inventory')
    if any(value < 0 for value in location_counts.values()) or sum(row.startswith('rewritten\t') for row in locations) != location_counts['rewritten'] or sum(row.startswith(('ignored-owner\t', 'ignored-descriptor\t')) for row in locations) != location_counts['ignored']:
        raise RuntimeError('Location rewrite count mismatch')
    result = {p.name: p for p in output.glob('classes*.dex')}
    if not result:
        raise RuntimeError('DEX rewrite produced no output')
    with zipfile.ZipFile(source) as archive:
        if len(set(archive.namelist())) != len(archive.namelist()):
            raise ValueError('Duplicate input APK members')
        for name, path in result.items():
            if name not in archive.namelist() or not path.read_bytes().startswith(b'dex\n'):
                raise RuntimeError('Invalid DEX replacement')
    if sha256(source) != input_hash:
        raise RuntimeError('Input APK changed during rewrite')
    receipt = {'input_sha256': input_hash, 'rewriter_sha256': sha256(tool),
               'dependencies': {p.name: sha256(p) for p in jars}, 'targets': targets,
               'dex_sha256': {n: sha256(p) for n, p in result.items()},
               'runtime_verified': False, 'location_counts': location_counts,
               'location_inventory_sha256': sha256(output / 'locations.tsv'),
               'location_offset_units': 'source DEX code units',
               'location_callsites': [row for row in locations if not row.startswith('#count\t')]}
    (work / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
    return result, receipt
