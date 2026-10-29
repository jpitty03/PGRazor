"""Offline isolated ART bridge build. No packaging, downloads, devices or cleanup.

Retains CMake objects/static archives, link map and unstripped library for relinking.
Example: Python312 -B pgrazor/art_build.py --abi arm64-v8a --abi x86_64
"""
import argparse
import datetime
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

import native_build

LSPLANT = 'd8b5d1dbb664abc606644036822e4bb64547edf6'
DEX_BUILDER = 'ac7fb2230954ee311808bad469b0db501f31bfb8'
PHMAP = '0cd57d29a959256ed66b2afdd1009928fc625d09'
XZ = 'ae63ae3a36ed01724674e8f3d750dc47bf125410'
SONAME = 'libpgrazor-art.so'
NDK_VERSION = '29.0.14206865'
LSPLANT_URL = 'https://github.com/LSPosed/LSPlant.git'
XZ_URL = 'https://github.com/tukaani-project/xz-embedded.git'


def dependency_paths(root, cache=None, environ=None):
    env = os.environ if environ is None else environ
    root = Path(root).resolve()
    chosen = cache or env.get('PGRAZOR_NATIVE_DEPS')
    if chosen:
        cache = Path(chosen).expanduser().resolve()
    else:
        portable = root / 'build/native/deps'
        legacy = root.parent / '.artifacts/native-migration'
        cache = portable if portable.exists() or not (legacy / 'LSPlant-runtime').exists() else legacy
    lsplant = cache / 'LSPlant-runtime'
    dex = lsplant / 'lsplant/src/main/jni/external/dex_builder'
    return {'lsplant': lsplant, 'dex_builder': dex,
            'parallel_hashmap': dex / 'external/parallel_hashmap',
            'xz_embedded': cache / ('xz-embedded-' + XZ)}


def resolve_pinned_ndk(root, revision, explicit_env, fallback_name, environ=None):
    env = os.environ if environ is None else environ
    candidates = []
    if env.get(explicit_env):
        candidates = [Path(env[explicit_env]).expanduser()]
    else:
        for key in ('ANDROID_NDK_HOME', 'ANDROID_NDK_ROOT'):
            if env.get(key):
                candidates.append(Path(env[key]).expanduser())
        for key in ('ANDROID_SDK_ROOT', 'ANDROID_HOME'):
            if env.get(key):
                candidates.append(Path(env[key]).expanduser() / 'ndk' / revision)
        candidates.append(Path(root).resolve().parent / '.artifacts/native-migration' / fallback_name)
    for candidate in candidates:
        candidate = candidate.resolve()
        if not (candidate / 'source.properties').is_file():
            continue
        if native_build._read_ndk_version(candidate) == revision:
            if not (candidate / 'build/cmake/android.toolchain.cmake').is_file():
                raise RuntimeError('NDK toolchain missing: ' + str(candidate))
            return candidate, revision
    raise RuntimeError('Pinned NDK ' + revision + ' not found. Set ' + explicit_env +
                       ' or install sdkmanager "ndk;' + revision + '" under ANDROID_SDK_ROOT. Checked: ' +
                       ', '.join(map(str, candidates)))


def resolve_art_ndk(root, environ=None):
    return resolve_pinned_ndk(root, NDK_VERSION, 'PGRAZOR_ART_NDK', 'android-ndk-r29', environ)


def run(args):
    result = subprocess.run(list(map(str, args)), capture_output=True, text=True, check=True)
    return result.stdout.strip()


def verified_git(path, commit):
    path = Path(path).resolve()
    if Path(run(['git', '-C', path, 'rev-parse', '--show-toplevel'])).resolve() != path:
        raise RuntimeError('Dependency must be its own repository: ' + str(path))
    actual = run(['git', '-C', path, 'rev-parse', 'HEAD'])
    if actual != commit:
        raise RuntimeError('Dependency HEAD mismatch: ' + str(path))
    # Include tracked edits and untracked compiler inputs; never trust HEAD alone.
    if run(['git', '-C', path, 'status', '--porcelain', '--untracked-files=all', '--ignore-submodules=untracked']):
        raise RuntimeError('Dependency tree is modified: ' + str(path))
    return {'path': str(path), 'commit': actual}


def gitlink(parent, relative, expected):
    row = run(['git', '-C', parent, 'ls-tree', 'HEAD', '--', relative]).split()
    if len(row) < 3 or row[0] != '160000' or row[2] != expected:
        raise RuntimeError('Dependency gitlink mismatch: ' + relative)


def preflight(root, cache=None):
    if sys.version_info < (3, 11):
        raise RuntimeError('ART/native build helpers require Python 3.11+')
    root = Path(root).resolve()
    paths = dependency_paths(root, cache)
    lsplant, dex, phmap, xz = (paths[name] for name in ('lsplant', 'dex_builder', 'parallel_hashmap', 'xz_embedded'))
    provenance = {name: verified_git(path, commit) for name, path, commit in (
        ('lsplant', lsplant, LSPLANT), ('dex_builder', dex, DEX_BUILDER),
        ('parallel_hashmap', phmap, PHMAP), ('xz_embedded', xz, XZ))}
    gitlink(lsplant, 'lsplant/src/main/jni/external/dex_builder', DEX_BUILDER)
    gitlink(dex, 'external/parallel_hashmap', PHMAP)
    # Existing helper verifies every extracted Dobby input against the SHA-pinned
    # cached archive. Explicit False prevents all network access.
    dobby = native_build.ensure_dobby(root, download=False)
    provenance['dobby'] = {'path': str(dobby), 'commit': native_build.DOBBY_COMMIT,
                           'archive_sha256': native_build.DOBBY_SHA256}
    source = root / 'art'
    for name in ('CMakeLists.txt', 'pgrazor-art.map', 'art_symbols.cpp', 'art_symbols.h', 'java_hooks.cpp'):
        if not (source / name).is_file():
            raise RuntimeError('Required ART source missing: ' + name)
    # The pinned ART engine uses C++23 facilities requiring its upstream r29
    # compiler. Gameplay core intentionally retains its independently pinned r27d.
    ndk, revision = resolve_art_ndk(root)
    cmake, ninja = (native_build.find_build_tool(name) for name in ('cmake', 'ninja'))
    version = re.search(r'cmake version (\d+)\.(\d+)', run([cmake, '--version']))
    if not version or tuple(map(int, version.groups())) < (3, 28):
        raise RuntimeError('Pinned LSPlant requires CMake 3.28+; install a current cmake Python wheel or update PATH')
    return {'paths': paths, 'dependencies': provenance, 'ndk': ndk,
            'revision': revision, 'cmake': cmake, 'ninja': ninja, 'dobby': dobby}


def build_all(root, abis, cache=None):
    root = Path(root).resolve()
    checked = preflight(root, cache)
    lsplant, dex, phmap, xz = (checked['paths'][name] for name in ('lsplant', 'dex_builder', 'parallel_hashmap', 'xz_embedded'))
    provenance = checked['dependencies']
    ndk, revision, cmake, ninja, dobby = (checked[name] for name in ('ndk', 'revision', 'cmake', 'ninja', 'dobby'))
    source = root / 'art'
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%d-%H%M%S-%f')
    session = root / 'build/art' / stamp
    session.mkdir(parents=True)
    receipt = {'success': False, 'packaged': False, 'ndk': revision,
               'dependencies': provenance, 'source_sha256': native_build.source_fingerprint(source),
               'builder_sha256': native_build.sha256(Path(__file__)), 'abis': {},
               'relinkability': 'Retain entire session: cmake objects/static archives, link map and unstripped outputs; dependencies remain at recorded pins.',
               'license_sources': [str(lsplant / 'LICENSE'), str(dex / 'LICENSE'),
                                   str(phmap / 'LICENSE'), str(xz / 'COPYING'), str(dobby / 'LICENSE')]}
    try:
        for abi in abis:
            work = session / abi / 'cmake'
            out = session / abi
            run([cmake, '-S', source, '-B', work, '-G', 'Ninja',
                 '-DCMAKE_MAKE_PROGRAM=' + str(ninja),
                 '-DCMAKE_TOOLCHAIN_FILE=' + str(ndk / 'build/cmake/android.toolchain.cmake'),
                 '-DANDROID_ABI=' + abi, '-DANDROID_PLATFORM=android-' + str(native_build.API_LEVEL),
                 '-DANDROID_STL=c++_static', '-DCMAKE_BUILD_TYPE=Release',
                 '-DLSPLANT_SOURCE_DIR=' + str(lsplant), '-DDOBBY_SOURCE_DIR=' + str(dobby),
                 '-DXZ_SOURCE_DIR=' + str(xz)])
            run([cmake, '--build', work, '--target', 'pgrazor_art'])
            built = work / SONAME
            unstripped = out / 'unstripped' / SONAME
            unstripped.parent.mkdir()
            shutil.copyfile(built, unstripped)
            output = out / SONAME
            run([native_build.ndk_tool(ndk, 'llvm-strip'), '--strip-unneeded', '-o', output, unstripped])
            if native_build.elf_identity(output) != (2, native_build.ELF_MACHINE[abi]):
                raise RuntimeError('ART ELF identity mismatch: ' + abi)
            exports = native_build.dynamic_exports(output, ndk)
            if exports != {'JNI_OnLoad'}:
                raise RuntimeError('Unexpected ART exports: ' + repr(sorted(exports)))
            receipt['abis'][abi] = {'stripped': str(output), 'sha256': native_build.sha256(output),
                                    'unstripped': str(unstripped), 'unstripped_sha256': native_build.sha256(unstripped),
                                    'objects': str(work), 'exports': sorted(exports)}
        receipt['success'] = True
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        receipt['error'] = str(error)
        if isinstance(error, subprocess.CalledProcessError):
            receipt['build_output'] = (error.stdout or '') + (error.stderr or '')
        raise
    finally:
        path = session / 'receipt.json'
        path.write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
        print('Receipt: ' + str(path))
    return {abi: Path(item['stripped']) for abi, item in receipt['abis'].items()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--abi', action='append', choices=native_build.DEFAULT_ABIS)
    parser.add_argument('--cache', type=Path, help='Pinned dependency cache (or PGRAZOR_NATIVE_DEPS)')
    parser.add_argument('--check', action='store_true', help='Verify offline prerequisites without compiling')
    args = parser.parse_args()
    root = Path(__file__).resolve().parent
    if args.check:
        checked = preflight(root, args.cache)
        print(json.dumps(checked, default=str, indent=2))
    else:
        build_all(root, tuple(dict.fromkeys(args.abi or native_build.DEFAULT_ABIS)), args.cache)


if __name__ == '__main__':
    main()
