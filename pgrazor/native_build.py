"""Deterministic native build helper for the pilot libpgrazor-core.so.

This module is the pilot entry point for compiling the C++ `pgrazor_core`
target into a stripped
``libpgrazor-core.so`` per ABI. It is opt-in: the coordinator only calls it
with ``PGRAZOR_NATIVE_PILOT=1`` set. It performs no network access unless a
download is explicitly requested, pins every immutable input by SHA-256, and
emits a reproducibility receipt.

Nothing here makes any claim about hiding or defeating runtime detection; it
only produces and verifies the artifact so the packager can bundle it.
"""
import argparse
import hashlib
import json
import os
import platform
import shutil
import struct
import subprocess
import sys
import urllib.parse
import urllib.request
import zipfile
from pathlib import Path

# Pinned Android NDK. The development fallback checkout is android-ndk-r27d,
# whose source.properties reports this revision.
EXPECTED_NDK_VERSION = '27.3.13750724'
NDK_FALLBACK_NAME = 'android-ndk-r27d'

# Pinned Dobby source. Immutable commit archive from GitHub's codeload host.
DOBBY_COMMIT = '0932d69c320e786672361ab53825ba8f4245e9d3'
DOBBY_URL = 'https://codeload.github.com/jmpews/Dobby/zip/' + DOBBY_COMMIT
DOBBY_SHA256 = 'a08cd9449ed39246e9a2b493a55fcc4f843a8ebe79ecef7747a02180b58b21db'

# Release target settings. CMake defines hidden visibility and PIC on the target.
API_LEVEL = 28
ANDROID_STL = 'c++_static'
TARGET = 'pgrazor_core'
OUTPUT_SONAME = 'libpgrazor-core.so'
DEFAULT_ABIS = ('arm64-v8a', 'x86_64')

# EM_* machine ids keyed by ABI; both outputs must be ELFCLASS64.
ELF_MACHINE = {'arm64-v8a': 183, 'x86_64': 62}

# The library must export exactly these dynamic symbols and nothing else.
EXPECTED_EXPORTS = frozenset({'JNI_OnLoad', 'pgrazor_native_api'})

# Download opt-in. Mirror of runtime_assets' explicit-network contract.
_DOWNLOAD_ENV = 'PGRAZOR_NATIVE_DOWNLOAD'
_PILOT_ENV = 'PGRAZOR_NATIVE_PILOT'
_MAX_EXTRACT_BYTES = 500_000_000


# --------------------------------------------------------------------------- #
# Hashing helpers
# --------------------------------------------------------------------------- #
def sha256(path):
    with open(path, 'rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


# --------------------------------------------------------------------------- #
# NDK discovery
# --------------------------------------------------------------------------- #
def _read_ndk_version(ndk):
    props = Path(ndk) / 'source.properties'
    if not props.is_file():
        raise RuntimeError('NDK source.properties not found under ' + str(ndk))
    for line in props.read_text(encoding='utf-8').splitlines():
        key, sep, value = line.partition('=')
        if sep and key.strip() == 'Pkg.Revision':
            return value.strip()
    raise RuntimeError('NDK source.properties has no Pkg.Revision: ' + str(props))


def resolve_ndk(root):
    """Return the validated pinned NDK home.

    Honours ANDROID_NDK_HOME / ANDROID_NDK_ROOT, else falls back to the local
    development checkout under ``<root.parent>/.artifacts/native-migration``.
    The resolved NDK must report the pinned revision.
    """
    root = Path(root).resolve()
    candidate = os.environ.get('ANDROID_NDK_HOME') or os.environ.get('ANDROID_NDK_ROOT')
    if candidate:
        ndk = Path(candidate).resolve()
    else:
        ndk = (root.parent / '.artifacts' / 'native-migration' / NDK_FALLBACK_NAME).resolve()
    if not ndk.is_dir():
        raise RuntimeError(
            'Android NDK not found. Set ANDROID_NDK_HOME to NDK r27d (' +
            EXPECTED_NDK_VERSION + ') or place it at ' + str(ndk))
    version = _read_ndk_version(ndk)
    if version != EXPECTED_NDK_VERSION:
        raise RuntimeError(
            'NDK revision mismatch: expected ' + EXPECTED_NDK_VERSION +
            ', found ' + version + ' at ' + str(ndk))
    return ndk, version


def _ndk_host_tag():
    system = platform.system()
    if system == 'Windows':
        return 'windows-x86_64'
    if system == 'Darwin':
        return 'darwin-x86_64'
    return 'linux-x86_64'


def ndk_tool(ndk, name):
    """Locate an llvm tool (llvm-strip, llvm-readelf) inside the NDK."""
    base = Path(ndk) / 'toolchains' / 'llvm' / 'prebuilt' / _ndk_host_tag() / 'bin'
    for candidate in (base / name, base / (name + '.exe')):
        if candidate.is_file():
            return candidate
    raise RuntimeError('NDK tool not found: ' + name + ' under ' + str(base))


# --------------------------------------------------------------------------- #
# CMake / Ninja discovery (never silently installed)
# --------------------------------------------------------------------------- #
def _python_package_tool(name):
    """Return an executable from an installed cmake/ninja Python package.

    These packages ship the real binary under <prefix>/Scripts (Windows) or
    <prefix>/bin. We only return it when already importable; we never install.
    """
    try:
        module = __import__(name)
    except ImportError:
        return None
    pkg = Path(getattr(module, '__file__', '') or '').resolve().parent
    # cmake/ninja Python wheels expose the binary via a sibling scripts dir.
    roots = [pkg, pkg.parent, Path(sys.prefix)]
    names = [name + '.exe', name] if os.name == 'nt' else [name]
    for base in roots:
        for sub in ('Scripts', 'bin', 'data/bin', 'data/Scripts'):
            for exe in names:
                candidate = base / sub / exe
                if candidate.is_file():
                    return candidate
    return None


def find_build_tool(name):
    """Locate cmake/ninja on PATH, then from an installed Python package."""
    found = shutil.which(name)
    if found:
        return Path(found)
    found = _python_package_tool(name)
    if found:
        return found
    raise RuntimeError(
        name + ' not found on PATH or as an installed Python package. '
        'Install it yourself (e.g. pip install ' + name + '); this helper '
        'never installs build tools silently.')


# --------------------------------------------------------------------------- #
# Safe ZIP extraction
# --------------------------------------------------------------------------- #
def _safe_members(archive, destination):
    """Yield (ZipInfo, resolved target) rejecting traversal/absolute/symlink."""
    destination = Path(destination).resolve()
    total = 0
    names = archive.namelist()
    if len(names) != len(set(names)):
        raise ValueError('Duplicate ZIP entries in archive')
    for info in archive.infolist():
        name = info.filename
        if name.startswith('/') or name.startswith('\\') or ':' in name.split('/')[0]:
            raise ValueError('Unsafe absolute ZIP entry: ' + name)
        parts = name.replace('\\', '/').split('/')
        if '..' in parts:
            raise ValueError('Unsafe ZIP traversal entry: ' + name)
        # Reject stored symlinks (type bits 0xA000) to avoid link escapes.
        if (info.external_attr >> 16) & 0o170000 == 0o120000:
            raise ValueError('Refusing to extract symlink entry: ' + name)
        target = (destination / name).resolve()
        if target != destination and destination not in target.parents:
            raise ValueError('ZIP entry escapes destination: ' + name)
        total += info.file_size
        if total > _MAX_EXTRACT_BYTES:
            raise ValueError('ZIP extraction exceeds size limit')
        yield info, target


def safe_extract(zip_path, destination):
    destination = Path(destination)
    destination.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(zip_path) as archive:
        for info, target in _safe_members(archive, destination):
            if info.is_dir():
                target.mkdir(parents=True, exist_ok=True)
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            with archive.open(info) as reader, target.open('wb') as writer:
                shutil.copyfileobj(reader, writer)
    return destination


# --------------------------------------------------------------------------- #
# Dobby acquisition (pinned, SHA-verified, extraction gated on explicit opt-in)
# --------------------------------------------------------------------------- #
def _vendor_dir(root):
    return (Path(root).resolve() / 'build' / 'native' / 'vendor')


def ensure_dobby(root, download=False):
    """Return the extracted pinned Dobby source directory.

    Uses a SHA-verified archive cached under build/native/vendor. Downloads
    only when ``download`` is true (``--download`` / PGRAZOR_NATIVE_DOWNLOAD=1).
    """
    vendor = _vendor_dir(root)
    vendor.mkdir(parents=True, exist_ok=True)
    archive = vendor / ('dobby-' + DOBBY_COMMIT + '.zip')
    extracted = vendor / ('Dobby-' + DOBBY_COMMIT)
    marker = extracted / '.pgrazor-verified'

    if not archive.is_file():
        if not download:
            raise RuntimeError(
                'Pinned Dobby archive is missing: ' + str(archive) + '. Re-run '
                'with --download (or PGRAZOR_NATIVE_DOWNLOAD=1) once, or supply '
                'the verified cache. No download attempted.')
        _download_pinned(DOBBY_URL, archive)

    digest = sha256(archive)
    if digest != DOBBY_SHA256:
        raise RuntimeError(
            'Dobby archive checksum mismatch: expected ' + DOBBY_SHA256 +
            ', got ' + digest + ' for ' + str(archive))

    # Never trust a marker alone: validate the actual compiler inputs against the
    # verified archive on every build. Refuse altered caches instead of deleting them.
    if extracted.exists():
        with zipfile.ZipFile(archive) as package:
            expected = set()
            for info, target in _safe_members(package, vendor):
                if info.is_dir(): continue
                expected.add(target)
                if target.is_symlink() or not target.is_file() or target.read_bytes() != package.read(info):
                    raise RuntimeError('Dobby source cache changed: ' + str(target))
            actual = {p.resolve() for p in extracted.rglob('*') if p.is_file() and p != marker}
            if actual != expected: raise RuntimeError('Unexpected files in Dobby source cache')
        return extracted
    safe_extract(archive, vendor)
    if not extracted.is_dir():
        raise RuntimeError('Dobby archive did not contain expected root: ' + str(extracted))
    marker.write_text(DOBBY_SHA256, encoding='utf-8')
    return extracted


def _download_pinned(url, destination):
    parts = urllib.parse.urlsplit(url)
    if parts.scheme != 'https':
        raise RuntimeError('Pinned downloads require HTTPS: ' + url)
    request = urllib.request.Request(url, headers={'User-Agent': 'PGRazorNativeBuild/0.1'})
    partial = destination.with_suffix(destination.suffix + '.partial')
    print('Downloading pinned Dobby archive:', url, flush=True)
    with urllib.request.urlopen(request, timeout=60) as source, partial.open('wb') as writer:
        shutil.copyfileobj(source, writer)
    partial.replace(destination)


# --------------------------------------------------------------------------- #
# Source fingerprinting for the reproducibility receipt
# --------------------------------------------------------------------------- #
def native_source_root(root):
    source = Path(root).resolve() / 'native'
    if not (source / 'CMakeLists.txt').is_file():
        raise RuntimeError('Native CMake source not found at ' + str(source / 'CMakeLists.txt'))
    return source


def source_fingerprint(source_root):
    """Stable SHA-256 over every tracked native source/config file."""
    source_root = Path(source_root).resolve()
    digest = hashlib.sha256()
    files = sorted(
        p for p in source_root.rglob('*')
        if p.is_file() and 'build' not in p.relative_to(source_root).parts)
    for path in files:
        rel = path.relative_to(source_root).as_posix()
        digest.update(rel.encode('utf-8'))
        digest.update(b'\0')
        digest.update(sha256(path).encode('ascii'))
        digest.update(b'\n')
    return digest.hexdigest()


# --------------------------------------------------------------------------- #
# ELF validation and dynamic export guard
# --------------------------------------------------------------------------- #
def elf_identity(path):
    """Return (ei_class, e_machine) from the ELF header via a direct read."""
    with open(path, 'rb') as stream:
        header = stream.read(20)
    if len(header) < 20 or header[:4] != b'\x7fELF':
        raise RuntimeError('Not an ELF file: ' + str(path))
    ei_class = header[4]
    ei_data = header[5]
    endian = '<' if ei_data == 1 else '>'
    e_machine = struct.unpack_from(endian + 'H', header, 18)[0]
    return ei_class, e_machine


def _parse_dynamic_exports(path):
    """Pure-Python fallback: defined global/weak symbols from .dynsym.

    Robust enough to validate exports offline without the NDK present.
    """
    data = Path(path).read_bytes()
    if data[:4] != b'\x7fELF' or data[4] != 2:  # ELFCLASS64 only
        raise RuntimeError('Expected ELF64 for symbol parse: ' + str(path))
    endian = '<' if data[5] == 1 else '>'
    e_shoff = struct.unpack_from(endian + 'Q', data, 0x28)[0]
    e_shentsize = struct.unpack_from(endian + 'H', data, 0x3A)[0]
    e_shnum = struct.unpack_from(endian + 'H', data, 0x3C)[0]
    sections = []
    for i in range(e_shnum):
        base = e_shoff + i * e_shentsize
        sh_type = struct.unpack_from(endian + 'I', data, base + 0x04)[0]
        sh_link = struct.unpack_from(endian + 'I', data, base + 0x28)[0]
        sh_offset = struct.unpack_from(endian + 'Q', data, base + 0x18)[0]
        sh_size = struct.unpack_from(endian + 'Q', data, base + 0x20)[0]
        sh_entsize = struct.unpack_from(endian + 'Q', data, base + 0x38)[0]
        sections.append((sh_type, sh_link, sh_offset, sh_size, sh_entsize))
    exports = set()
    for sh_type, sh_link, sh_offset, sh_size, sh_entsize in sections:
        if sh_type != 11 or sh_entsize == 0:  # SHT_DYNSYM
            continue
        _, _, str_off, str_size, _ = sections[sh_link]
        strtab = data[str_off:str_off + str_size]
        for off in range(sh_offset, sh_offset + sh_size, sh_entsize):
            st_name = struct.unpack_from(endian + 'I', data, off)[0]
            st_info = data[off + 4]
            st_shndx = struct.unpack_from(endian + 'H', data, off + 6)[0]
            binding = st_info >> 4
            if st_shndx == 0 or binding not in (1, 2):  # defined GLOBAL/WEAK
                continue
            end = strtab.find(b'\0', st_name)
            name = strtab[st_name:end].decode('utf-8', 'replace')
            if name:
                exports.add(name)
    return exports


def dynamic_exports(path, ndk=None):
    """Exported dynamic symbol names via llvm-readelf, else a direct parse."""
    tool = None
    if ndk is not None:
        try:
            tool = ndk_tool(ndk, 'llvm-readelf')
        except RuntimeError:
            tool = None
    if tool is not None:
        out = subprocess.run(
            [str(tool), '--dyn-syms', str(path)],
            check=True, capture_output=True, text=True).stdout
        exports = set()
        for line in out.splitlines()[1:]:
            cols = line.split()
            if len(cols) < 8:
                continue
            bind, ndx, name = cols[4], cols[6], cols[7]
            if bind in ('GLOBAL', 'WEAK') and ndx != 'UND' and name:
                exports.add(name)
        return exports
    return _parse_dynamic_exports(path)


def assert_exports(names):
    """Guard: the dynamic export set must equal EXPECTED_EXPORTS exactly."""
    found = frozenset(names)
    if found != EXPECTED_EXPORTS:
        missing = sorted(EXPECTED_EXPORTS - found)
        extra = sorted(found - EXPECTED_EXPORTS)
        raise RuntimeError(
            'Unexpected dynamic exports. missing=' + repr(missing) +
            ' extra=' + repr(extra))


def validate_library(path, abi, ndk=None):
    """Confirm ELF64 + expected machine + exactly the expected exports."""
    ei_class, e_machine = elf_identity(path)
    if ei_class != 2:
        raise RuntimeError('Expected ELFCLASS64, got class ' + str(ei_class) + ': ' + str(path))
    expected_machine = ELF_MACHINE.get(abi)
    if expected_machine is None:
        raise RuntimeError('Unsupported ABI: ' + abi)
    if e_machine != expected_machine:
        raise RuntimeError(
            'ELF machine mismatch for ' + abi + ': expected ' + str(expected_machine) +
            ', got ' + str(e_machine))
    assert_exports(dynamic_exports(path, ndk))


# --------------------------------------------------------------------------- #
# Compile + strip
# --------------------------------------------------------------------------- #
def _run(args, **kwargs):
    print('+', ' '.join(str(a) for a in args), flush=True)
    subprocess.run([str(a) for a in args], check=True, **kwargs)


def _copy_dobby_license(dobby_dir, out_dir):
    """Preserve Dobby's LICENSE alongside the artifact for the packager."""
    for name in ('LICENSE', 'LICENSE.txt', 'LICENSE.md'):
        src = Path(dobby_dir) / name
        if src.is_file():
            dest = Path(out_dir) / 'DOBBY_LICENSE'
            shutil.copyfile(src, dest)
            return dest
    raise RuntimeError('Dobby LICENSE not found under ' + str(dobby_dir))


def build_one(root, abi, download=False, ndk=None, dobby_dir=None,
              cmake=None, ninja=None):
    """Configure, build, strip and validate one ABI; return the stripped Path."""
    if abi not in ELF_MACHINE:
        raise RuntimeError('Unsupported ABI: ' + abi)
    root = Path(root).resolve()
    source = native_source_root(root)
    if ndk is None:
        ndk, ndk_version = resolve_ndk(root)
    else:
        ndk_version = _read_ndk_version(ndk)
    if cmake is None:
        cmake = find_build_tool('cmake')
    if ninja is None:
        ninja = find_build_tool('ninja')
    if dobby_dir is None:
        dobby_dir = ensure_dobby(root, download=download)

    native_build = root / 'build' / 'native'
    build_dir = native_build / 'build' / abi
    out_dir = native_build / 'out' / abi
    unstripped_dir = out_dir / 'unstripped'
    for directory in (build_dir, unstripped_dir):
        directory.mkdir(parents=True, exist_ok=True)

    toolchain = ndk / 'build' / 'cmake' / 'android.toolchain.cmake'
    if not toolchain.is_file():
        raise RuntimeError('NDK CMake toolchain not found: ' + str(toolchain))

    _run([cmake, '-S', source, '-B', build_dir, '-G', 'Ninja',
          '-DCMAKE_MAKE_PROGRAM=' + str(ninja),
          '-DCMAKE_TOOLCHAIN_FILE=' + str(toolchain),
          '-DANDROID_ABI=' + abi,
          '-DANDROID_PLATFORM=android-' + str(API_LEVEL),
          '-DANDROID_STL=' + ANDROID_STL,
          '-DCMAKE_BUILD_TYPE=Release',
          '-DDOBBY_SOURCE_DIR=' + str(dobby_dir)])
    _run([cmake, '--build', build_dir, '--target', TARGET])

    built = _locate_built_library(build_dir)
    unstripped = unstripped_dir / OUTPUT_SONAME
    shutil.copyfile(built, unstripped)

    output = out_dir / OUTPUT_SONAME
    llvm_strip = ndk_tool(ndk, 'llvm-strip')
    _run([llvm_strip, '--strip-unneeded', '-o', output, unstripped])

    validate_library(output, abi, ndk)
    _copy_dobby_license(dobby_dir, out_dir)

    receipt = {
        'abi': abi,
        'soname': OUTPUT_SONAME,
        'ndk_version': ndk_version,
        'api_level': API_LEVEL,
        'android_stl': ANDROID_STL,
        'dobby_commit': DOBBY_COMMIT,
        'dobby_sha256': DOBBY_SHA256,
        'source_fingerprint': source_fingerprint(source),
        'unstripped_sha256': sha256(unstripped),
        'stripped_sha256': sha256(output),
        'exports': sorted(EXPECTED_EXPORTS),
    }
    (out_dir / 'receipt.json').write_text(json.dumps(receipt, indent=2), encoding='utf-8')
    return output


def _locate_built_library(build_dir):
    matches = sorted(Path(build_dir).rglob(OUTPUT_SONAME))
    if not matches:
        raise RuntimeError(
            'CMake build produced no ' + OUTPUT_SONAME + ' under ' + str(build_dir) +
            '. The pgrazor_core target must emit OUTPUT_NAME pgrazor-core.')
    return matches[0]


def build_all(root, abis=DEFAULT_ABIS, download=False):
    """Build every requested ABI; return {abi: Path to stripped .so}.

    Opt-in: requires PGRAZOR_NATIVE_PILOT=1. Shared NDK/CMake/Ninja/Dobby
    resolution is done once and reused across ABIs.
    """
    if os.environ.get(_PILOT_ENV) != '1':
        raise RuntimeError(
            'Native pilot build is opt-in. Set ' + _PILOT_ENV + '=1 to enable it.')
    root = Path(root).resolve()
    schema_generator=root.parent/'tools/native_routes_schema.py'
    if not schema_generator.is_file():
        raise RuntimeError('Missing compiled Routes schema generator')
    subprocess.run([sys.executable,'-B',str(schema_generator),'--check'],check=True,timeout=30)
    download = download or os.environ.get(_DOWNLOAD_ENV) == '1'
    abis = list(dict.fromkeys(abis))
    if not abis:
        raise RuntimeError('No ABIs requested')
    ndk, _ = resolve_ndk(root)
    cmake = find_build_tool('cmake')
    ninja = find_build_tool('ninja')
    dobby_dir = ensure_dobby(root, download=download)
    results = {}
    for abi in abis:
        results[abi] = build_one(
            root, abi, download=download, ndk=ndk, dobby_dir=dobby_dir,
            cmake=cmake, ninja=ninja)
    return results


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #
def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parent,
                        help='pgrazor project root (default: this file\'s directory)')
    parser.add_argument('--abi', action='append', dest='abis', choices=sorted(ELF_MACHINE),
                        help='ABI to build; repeat for several (default: all release ABIs)')
    parser.add_argument('--download', action='store_true',
                        help='allow the one pinned Dobby download if not cached')
    args = parser.parse_args(argv)
    abis = tuple(args.abis) if args.abis else DEFAULT_ABIS
    results = build_all(args.root, abis, download=args.download)
    for abi, path in results.items():
        print(abi, '->', path)
    return 0


if __name__ == '__main__':
    sys.exit(main())
