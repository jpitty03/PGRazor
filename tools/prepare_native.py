"""Explicit native prerequisite setup/preflight; never builds or packages an APK.

Requires Python 3.11+. Install NDKs with sdkmanager "ndk;27.3.13750724" "ndk;29.0.14206865" and
install cmake/ninja on PATH (Python wheels also work). Set ANDROID_NDK_HOME
to the gameplay NDK27 directory and optionally PGRAZOR_ART_NDK to NDK29.
Run with --download once to fetch pinned sources, then omit it for offline checks.
Use --cache or PGRAZOR_NATIVE_DEPS for a shared dependency cache; retain the
same setting for later art_build/nonroot commands and retain build receipts.
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1] / 'pgrazor'
sys.path.insert(0, str(ROOT))
import art_build
import native_build


def clone_pinned(path, url, commit, download):
    path = Path(path)
    if path.exists():
        # Existing directories are never reset, replaced or silently repurposed.
        return art_build.verified_git(path, commit)
    if not download:
        raise RuntimeError('Missing pinned source: ' + str(path) + '. Run tools/prepare_native.py --download explicitly.')
    path.parent.mkdir(parents=True, exist_ok=True)
    art_build.run(['git', 'clone', '--no-checkout', url, path])
    art_build.run(['git', '-C', path, 'checkout', '--detach', commit])
    return art_build.verified_git(path, commit)


def prepare_dependencies(root, cache=None, download=False):
    paths = art_build.dependency_paths(root, cache)
    commits = {'lsplant': art_build.LSPLANT, 'dex_builder': art_build.DEX_BUILDER,
               'parallel_hashmap': art_build.PHMAP, 'xz_embedded': art_build.XZ}
    # Inspect existing sources before any checkout/fetch. Empty gitlink folders
    # are allowed only for the two required, not-yet-initialized submodules.
    for name, path in paths.items():
        if path.exists() and (name in ('lsplant', 'xz_embedded') or any(path.iterdir())):
            art_build.verified_git(path, commits[name])
    clone_pinned(paths['lsplant'], art_build.LSPLANT_URL, art_build.LSPLANT, download)
    art_build.gitlink(paths['lsplant'], 'lsplant/src/main/jni/external/dex_builder', art_build.DEX_BUILDER)
    dex = paths['dex_builder']
    if not (dex / '.git').exists():
        if not download:
            raise RuntimeError('Missing pinned DexBuilder submodule; run tools/prepare_native.py --download')
        # Do not initialize unrelated docs/test submodules (some use SSH URLs).
        art_build.run(['git', '-C', paths['lsplant'], 'submodule', 'update', '--init', '--',
                       'lsplant/src/main/jni/external/dex_builder'])
    art_build.verified_git(dex, art_build.DEX_BUILDER)
    art_build.gitlink(dex, 'external/parallel_hashmap', art_build.PHMAP)
    if not (paths['parallel_hashmap'] / '.git').exists():
        if not download:
            raise RuntimeError('Missing pinned parallel_hashmap submodule; run tools/prepare_native.py --download')
        art_build.run(['git', '-C', dex, 'submodule', 'update', '--init', '--', 'external/parallel_hashmap'])
    art_build.verified_git(paths['parallel_hashmap'], art_build.PHMAP)
    clone_pinned(paths['xz_embedded'], art_build.XZ_URL, art_build.XZ, download)
    native_build.ensure_dobby(Path(root), download=download)
    return paths


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--download', action='store_true', help='Explicitly permit fetching missing pinned Git sources and Dobby archive')
    parser.add_argument('--cache', type=Path, help='Dependency cache; use the same option/env for subsequent builds')
    args = parser.parse_args(argv)
    try:
        if sys.version_info < (3, 11):
            raise RuntimeError('Python 3.11+ is required by native_build; select the pinned CI Python or py -3.12 on Windows')
        # Fail early with actionable compiler/tool instructions before downloading.
        gameplay, _ = art_build.resolve_pinned_ndk(ROOT, native_build.EXPECTED_NDK_VERSION,
            'ANDROID_NDK_HOME', native_build.NDK_FALLBACK_NAME)
        art_build.resolve_art_ndk(ROOT)
        for name in ('cmake', 'ninja'):
            native_build.find_build_tool(name)
        prepare_dependencies(ROOT, args.cache, args.download)
        checked = art_build.preflight(ROOT, args.cache)
        checked['gameplay_ndk'] = str(gameplay)
        checked['gameplay_build_environment'] = {'ANDROID_NDK_HOME': str(gameplay)}
        checked['art_build_environment'] = {'PGRAZOR_ART_NDK': str(checked['ndk']),
            'PGRAZOR_NATIVE_DEPS': str(checked['paths']['lsplant'].parent)}
        print(json.dumps(checked, default=str, indent=2))
        return 0
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print('Native prerequisite check failed: ' + str(error), file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
