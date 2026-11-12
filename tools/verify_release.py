"""Reject incomplete or mixed Android release bundles before upload/publication."""
import argparse
import hashlib
import json
from pathlib import Path
import zipfile


DOWNLOADS = ('PGRazor.apk', 'PGRazor-Root.apk', 'PGRazor-Root-module.zip')


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def verify(folder, tag):
    folder = Path(folder)
    for name in (*DOWNLOADS, 'build-info.json', 'root-build-info.json', 'SHA256SUMS.txt'):
        if not (folder / name).is_file() or not (folder / name).stat().st_size:
            raise ValueError('Missing or empty release file: ' + name)
    checksums = {}
    for line in (folder / 'SHA256SUMS.txt').read_text(encoding='utf-8').splitlines():
        checksum, name = line.split('  ', 1)
        if name in checksums:
            raise ValueError('Duplicate checksum: ' + name)
        checksums[name] = checksum
    actual = {name: digest(folder / name) for name in DOWNLOADS}
    if checksums != actual:
        raise ValueError('Release checksums do not match the three downloads')
    normal = json.loads((folder / 'build-info.json').read_text(encoding='utf-8'))
    root = json.loads((folder / 'root-build-info.json').read_text(encoding='utf-8'))
    if (normal['apk_sha256'] != actual[DOWNLOADS[0]] or
            root['installer_sha256'] != actual[DOWNLOADS[1]] or
            root['module_sha256'] != actual[DOWNLOADS[2]]):
        raise ValueError('Build receipts do not match the downloads')
    if (normal['game_version'] != root['target']['versionName'] or
            normal['game_version_code'] != root['target']['versionCode']):
        raise ValueError('Rooted and non-rooted game targets differ')
    if root['version'] != tag:
        raise ValueError('Rooted build version differs from release tag')
    with zipfile.ZipFile(folder / DOWNLOADS[0]) as apk:
        mode = json.loads(apk.read('assets/pgrazor/mode.json'))
        if mode.get('pgrazorVersion') != tag:
            raise ValueError('Non-rooted build version differs from release tag')
    with zipfile.ZipFile(folder / DOWNLOADS[2]) as module:
        props = dict(line.split('=', 1) for line in module.read('module.prop').decode().splitlines()
                     if '=' in line)
        if props.get('id') != 'pgrazor' or props.get('version') != tag:
            raise ValueError('Module identity/version differs from release tag')
    with zipfile.ZipFile(folder / DOWNLOADS[1]) as installer:
        embedded = [name for name in installer.namelist() if name.startswith('assets/') and name.endswith('.zip')]
        if len(embedded) != 1:
            raise ValueError('Installer must contain exactly one module ZIP')
        with installer.open(embedded[0]) as stream:
            if hashlib.file_digest(stream, 'sha256').hexdigest() != actual[DOWNLOADS[2]]:
                raise ValueError('Installer embeds a different module from the published ZIP')
    return actual


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('folder', type=Path)
    parser.add_argument('--tag', required=True)
    args = parser.parse_args()
    for name, checksum in verify(args.folder, args.tag).items():
        print(checksum + '  ' + name)
