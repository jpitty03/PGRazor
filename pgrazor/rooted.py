"""PGRazor Root packager: a Zygisk module, and the installer app that carries it.

PGRazor Root runs PGRazor inside the unmodified, Niantic-signed game on rooted phones (Magisk 27+,
KernelSU or APatch, with Zygisk). The patched APK (nonroot.py) is unaffected; both builds share
PGRazor's DEX and native libraries.

Module layout:
  module.prop, META-INF/... (Magisk installer), zygisk/<abi>.so (the loader),
  pgrazor/classes.dex (PGRazor), pgrazor/payload.zip (everything the APK build ships under
  assets/, plus pgrazor/rooted/target.json and location-callers.txt).
"""
import argparse
import datetime as dt
import json
import os
import re
import shutil
import subprocess
import sys
import uuid
import zipfile
from pathlib import Path

import axml
import build
import nonroot
import runtime_assets

ROOT = Path(__file__).resolve().parent
ABIS = ['arm64-v8a', 'x86_64']
TRIPLES = {'arm64-v8a': 'aarch64-linux-android28', 'x86_64': 'x86_64-linux-android28'}
MODULE_ID = 'pgrazor'
INSTALLER_PACKAGE = 'dev.pgrazor.root'

UPDATE_BINARY = '''#!/sbin/sh
# Standard Magisk module installer (KernelSU and APatch install modules without it).
umask 022
ui_print() { echo "$1"; }
require_new_magisk() {
  ui_print "*******************************"
  ui_print " Please install Magisk v27+! "
  ui_print "*******************************"
  exit 1
}
OUTFD=$2
ZIPFILE=$3
mount /data 2>/dev/null
[ -f /data/adb/magisk/util_functions.sh ] || require_new_magisk
. /data/adb/magisk/util_functions.sh
[ $MAGISK_VER_CODE -lt 27000 ] && require_new_magisk
install_module
exit 0
'''

# Capability marker: an empty file that advertises this module understands the launch-time
# PGRazor: On / Off switch (the loader reads pgrazor/injection-off). The installer uses its
# presence to tell a toggle-aware module from an older one.
TOGGLE_CAPABILITY = 'pgrazor/toggle-v1'

# Soft-off flag the installer writes/removes in the live module. It is never shipped in the
# package (a fresh install defaults to On); it only ever exists because a user turned PGRazor off.
INJECTION_OFF_FLAG = 'pgrazor/injection-off'

# Sourced by the root manager (Magisk / KernelSU / APatch) during install, after the zip is
# unpacked into the staging directory $MODPATH. A module update replaces the whole module tree,
# which would silently turn injection back On; this carries the user's Off choice forward from the
# currently-installed (live) module. If it cannot preserve a known-off state it aborts the install
# rather than silently re-enabling. A fresh install ships no flag, so injection defaults to On. The
# same script runs whether the update is installed straight through the root manager or by the APK.
CUSTOMIZE_SH = '''#!/system/bin/sh
# PGRazor Root install customizer: preserve the user's PGRazor: On / Off choice across updates.
LIVE_FLAG="/data/adb/modules/pgrazor/''' + INJECTION_OFF_FLAG + '''"
STAGED_FLAG="${MODPATH}/''' + INJECTION_OFF_FLAG + '''"

# The live module is the source of truth for the current switch state. Staging dirs are wiped and
# re-extracted by the manager before this runs, so an earlier staged flag cannot be read here.
if [ -e "${LIVE_FLAG}" ]; then
  ui_print "- PGRazor is switched Off on this device; keeping it Off after this update."
  mkdir -p "${MODPATH}/pgrazor" || abort "! Cannot preserve the PGRazor Off switch (mkdir failed); aborting instead of silently re-enabling PGRazor."
  : > "${STAGED_FLAG}" 2>/dev/null
  [ -f "${STAGED_FLAG}" ] || abort "! Cannot preserve the PGRazor Off switch; aborting instead of silently re-enabling PGRazor."
else
  rm -f "${STAGED_FLAG}" || abort "! Cannot preserve the PGRazor On switch."
fi
'''


def capability_module_entries():
    """Static module entries for the launch-time On/Off switch: the install-time preservation
    script and the empty capability marker. Merged into the packaged module by `package()`."""
    return {'customize.sh': CUSTOMIZE_SH, TOGGLE_CAPABILITY: b''}


def compiled_mode(abis):
    """The release (compiled gameplay) flags, minus browser sign-in: the official app's works."""
    return {'gadget': False, 'version': runtime_assets.VERSION, 'preferredAbi': '', 'runtimeAbis': abis,
            'compiledGameplay': True, 'nativePilot': True, 'compiledNativeTransport': True,
            'compiledCatchWait': True, 'compiledLocationGetters': True, 'compiledJavaBridges': True,
            'cameraProbe': True, 'cameraSteering': True, 'interactionProbe': True,
            'quickSummary': True, 'quickClose': True, 'quickBall': True}


def version_code(version):
    from rooted_installer import installer_version_code
    return installer_version_code(version)


def require_supported_target(target):
    schema = ROOT / 'native/schema/game'
    name = json.loads((schema / 'current.json').read_text(encoding='utf-8'))['profile']
    if not re.fullmatch(r'[0-9.]+\.json', name):
        raise ValueError('Invalid native game profile')
    profile = json.loads((schema / name).read_text(encoding='utf-8'))
    if (target['versionCode'] != profile['version_code'] or
            target['versionName'] != profile['game_version']):
        raise ValueError('PGRazor Root requires the reviewed native game target ' + profile['game_version'])


def clang(abi):
    import native_build
    ndk, _ = native_build.resolve_ndk(ROOT)
    base = Path(ndk) /'toolchains/llvm/prebuilt' / native_build._ndk_host_tag() / 'bin'
    for name in (TRIPLES[abi] + '-clang++.cmd', TRIPLES[abi] + '-clang++'):
        if (base / name).is_file(): return base / name
    raise RuntimeError('NDK clang++ not found for ' + abi)


def build_loader(abi, output):
    """Compiles zygisk/loader.cpp for one ABI into a stripped shared library."""
    subprocess.run([str(clang(abi)), '-std=c++17', '-O2', '-fPIC', '-shared', '-fno-exceptions', '-fno-rtti',
                    '-fvisibility=hidden', '-static-libstdc++', '-I', str(ROOT / 'vendor/zygisk'),
                    str(ROOT / 'zygisk/loader.cpp'), '-llog', '-Wl,--gc-sections', '-Wl,-z,max-page-size=16384',
                    '-s', '-o', str(output)], check=True, timeout=300)
    if not output.is_file() or output.stat().st_size == 0: raise RuntimeError('Loader build failed: ' + abi)
    return output


def location_callers(locations_tsv):
    """Game methods that call the four Location getters (rewriter inventory), one descriptor per line."""
    rows = [line.split('\t') for line in locations_tsv.read_text(encoding='utf-8').splitlines()]
    return sorted({row[2] for row in rows if row and row[0] == 'rewritten'})


def payload_assets(download=False):
    """Everything the APK build ships under assets/pgrazor/ for compiled gameplay, keyed by asset name."""
    import art_build
    import native_build
    assets = {}
    with nonroot.scoped_env(PGRAZOR_NATIVE_PILOT='1'):
        native_outputs = native_build.build_all(ROOT, ABIS, download=download)
    if set(native_outputs) != set(ABIS): raise RuntimeError('Native core ABI set mismatch')
    for abi, binary in native_outputs.items(): assets[f'pgrazor/{abi}/libpgrazor-core.so'] = Path(binary)
    assets['pgrazor/native-notices/Dobby-LICENSE'] = native_build.ensure_dobby(ROOT) / 'LICENSE'
    art_outputs = art_build.build_all(ROOT, ABIS)
    if set(art_outputs) != set(ABIS): raise RuntimeError('ART build ABI set mismatch')
    sessions = {Path(binary).resolve().parent.parent for binary in art_outputs.values()}
    if len(sessions) != 1: raise RuntimeError('ART outputs must share one build session')
    art_receipt = json.loads((sessions.pop() / 'receipt.json').read_text(encoding='utf-8'))
    if art_receipt.get('success') is not True: raise RuntimeError('ART build receipt unsuccessful')
    for abi, binary in art_outputs.items():
        entry = art_receipt.get('abis', {}).get(abi, {})
        if entry.get('sha256') != runtime_assets.sha256(binary) or entry.get('exports') != ['JNI_OnLoad']:
            raise RuntimeError('ART build output does not match verified receipt: ' + abi)
        assets[f'pgrazor/{abi}/libpgrazor-art.so'] = Path(binary)
    names = ('LSPlant-LICENSE', 'DexBuilder-LICENSE', 'parallel-hashmap-LICENSE', 'XZ-Embedded-COPYING', 'Dobby-LICENSE')
    sources = art_receipt.get('license_sources', [])
    if len(sources) != len(names): raise RuntimeError('All five ART dependency licenses required')
    for name, source in zip(names, sources): assets['pgrazor/art-notices/' + name] = Path(source)
    for name, path in build.map_icon_assets().items(): assets['pgrazor/map-icons/' + name] = Path(path)
    for name, path in build.feed_icon_assets().items(): assets['pgrazor/feed-icons/pokemon/' + name] = Path(path)
    return assets


def write_zip(path, entries, compression=zipfile.ZIP_DEFLATED):
    with zipfile.ZipFile(path, 'w', compression) as archive:
        for name, value in sorted(entries.items()):
            if isinstance(value, (bytes, str)):
                archive.writestr(name, value.encode('utf-8') if isinstance(value, str) else value)
            else:
                archive.write(value, name)
    return path


def package(source, download=False, installer=True):
    """Builds the PGRazor Root module zip for the game in `source` (original APK/APKM)."""
    dex = nonroot.validated_build()
    version = build.release_version(ROOT)
    folder = ROOT / 'build/rooted' / (dt.datetime.now().strftime('%Y%m%d-%H%M%S') + '-' + uuid.uuid4().hex[:8])
    folder.mkdir(parents=True)
    inputs = nonroot.input_apks(Path(source), folder)
    package_name, base, rows, native_abis = nonroot.inspect_set(inputs)
    if package_name != 'com.nianticlabs.pokemongo': raise ValueError('Expected the Pokémon GO package')
    if native_abis != {'arm64-v8a'}: raise ValueError('Expected an ARM64 game APK set; found ' + repr(native_abis))
    for path, _ in rows:
        verified = nonroot.command(build.TOOLS / 'apksigner.bat', 'verify', '--print-certs', path, capture=True)
        digests = sorted(d.lower() for d in re.findall(r'Signer #\d+ certificate SHA-256 digest: ([0-9a-fA-F]+)', verified.stdout))
        nonroot.require_clean_game_certificate(digests)
    with zipfile.ZipFile(base) as archive:
        manifest = axml.inspect_manifest(archive.read('AndroidManifest.xml'))
    target = {'package': package_name, 'versionCode': int(manifest['versionCode']),
              'versionName': manifest['versionName'], 'certificateSha256': nonroot.NIANTIC_CERT_SHA256}
    require_supported_target(target)

    # The same exact-descriptor validation as the patched build: every bridge target must exist in
    # this game version. Its Location inventory lists the callers PGRazor Root deoptimizes.
    import dex_bridges
    dex_bridges.rewrite(base, folder / 'java-bridges', download=download)
    callers = location_callers(folder / 'java-bridges/rewritten/locations.tsv')

    assets = payload_assets(download)
    assets['pgrazor/mode.json'] = json.dumps(compiled_mode(ABIS))
    assets['pgrazor/rooted/target.json'] = json.dumps(target)
    assets['pgrazor/rooted/location-callers.txt'] = '\n'.join(callers) + '\n'
    payload = write_zip(folder / 'payload.zip', assets, zipfile.ZIP_STORED)

    module = {
        'module.prop': '\n'.join([f'id={MODULE_ID}', 'name=PGRazor Root', f'version={version}',
                                  f'versionCode={version_code(version)}', 'author=PGRazor',
                                  f'description=PGRazor for the official Pokémon GO app ({target["versionName"]}). Requires Zygisk.', '']),
        'META-INF/com/google/android/update-binary': UPDATE_BINARY,
        'META-INF/com/google/android/updater-script': '#MAGISK\n',
        'pgrazor/classes.dex': Path(dex),
        'pgrazor/payload.zip': payload,
    }
    module.update(capability_module_entries())
    for abi in ABIS: module[f'zygisk/{abi}.so'] = build_loader(abi, folder / f'loader-{abi}.so')
    module_zip = write_zip(folder / f'PGRazor-Root-module-{version}.zip', module)

    report = {'version': version, 'target': target, 'module': str(module_zip), 'module_sha256': runtime_assets.sha256(module_zip),
              'dex_sha256': runtime_assets.sha256(dex), 'payload_sha256': runtime_assets.sha256(payload),
              'location_callers': len(callers), 'abis': ABIS, 'installed': False}
    if installer:
        from rooted_installer import build_installer
        apk = build_installer(module_zip, target, version, folder)
        report.update(installer=str(apk), installer_sha256=runtime_assets.sha256(apk))
    (folder / 'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print('Packaged:', module_zip, flush=True)
    if os.name == 'nt':
        try:
            subprocess.run([shutil.which('pwsh') or 'powershell.exe', '-NoProfile', '-ExecutionPolicy', 'Bypass',
                            '-File', str(ROOT / 'cleanup.ps1'), '-RootedOnly', '-Apply'],
                           check=True, timeout=300)
        except (OSError, subprocess.SubprocessError) as error:
            print('WARNING: Rooted package complete but cleanup failed: ' + str(error), flush=True)
    return report, folder


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('source', help='Original Pokémon GO APK or APKM (Niantic-signed)')
    parser.add_argument('--download', action='store_true', help='Download pinned build dependencies when missing')
    parser.add_argument('--skip-build', action='store_true', help='Reuse a source-validated PGRazor DEX')
    parser.add_argument('--module-only', action='store_true', help='Build only the development module ZIP')
    parser.add_argument('--output', type=Path, help='Copy installer, module ZIP, and rooted build receipt to this release directory')
    args = parser.parse_args(argv)
    if args.output and args.module_only: parser.error('--output requires the installer')
    if not args.skip_build:
        subprocess.run([sys.executable, '-B', str(ROOT / 'build.py')], check=True)
    report, _ = package(args.source, download=args.download, installer=not args.module_only)
    if args.output:
        args.output.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(report['installer'], args.output / 'PGRazor-Root.apk')
        shutil.copyfile(report['module'], args.output / 'PGRazor-Root-module.zip')
        public = {key: report[key] for key in ('version', 'target', 'module_sha256', 'installer_sha256', 'abis')}
        (args.output / 'root-build-info.json').write_text(json.dumps(public, indent=2) + '\n', encoding='utf-8')
        sums = args.output / 'SHA256SUMS.txt'
        lines = sums.read_text(encoding='utf-8').splitlines() if sums.exists() else []
        lines = [line for line in lines if not line.endswith(('  PGRazor-Root.apk', '  PGRazor-Root-module.zip'))]
        lines.append(report['installer_sha256'] + '  PGRazor-Root.apk')
        lines.append(report['module_sha256'] + '  PGRazor-Root-module.zip')
        sums.write_text('\n'.join(lines) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
