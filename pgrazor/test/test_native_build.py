"""Offline guards for native_build.py.

These exercise the pure logic that does not need a real NDK/CMake toolchain or
any network access: checksum verification, ZIP traversal/symlink rejection,
the dynamic-export guard, ELF parsing, and stale-source failure modes.
"""
import importlib.util
import os
import struct
import tempfile
import unittest
import zipfile
from pathlib import Path

_spec = importlib.util.spec_from_file_location(
    'native_build', Path(__file__).resolve().parents[1] / 'native_build.py')
nb = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(nb)


def _elf64(machine, symbols):
    """Assemble a minimal ELF64 with a .dynsym/.dynstr pair.

    symbols: list of (name, binding, shndx). binding 1=GLOBAL, 2=WEAK;
    shndx 0 means undefined.
    """
    endian = '<'
    # String table.
    strtab = b'\x00'
    offsets = {}
    for name, _, _ in symbols:
        offsets[name] = len(strtab)
        strtab += name.encode('utf-8') + b'\x00'
    # Symbol table (leading null entry).
    symtab = b'\x00' * 24
    for name, binding, shndx in symbols:
        st_info = (binding << 4) | 2  # type FUNC
        symtab += struct.pack(endian + 'IBBHQQ', offsets[name], st_info, 0, shndx, 0, 0)

    ehdr_size = 64
    strtab_off = ehdr_size
    symtab_off = strtab_off + len(strtab)
    shoff = symtab_off + len(symtab)
    shentsize = 64

    def shdr(sh_type, sh_offset, sh_size, sh_link, sh_entsize):
        return struct.pack(
            endian + 'IIQQQQIIQQ',
            0, sh_type, 0, 0, sh_offset, sh_size, sh_link, 0, 1, sh_entsize)

    # index 0 null, 1 dynstr (STRTAB=3), 2 dynsym (DYNSYM=11 -> link index 1)
    sections = (
        shdr(0, 0, 0, 0, 0) +
        shdr(3, strtab_off, len(strtab), 0, 0) +
        shdr(11, symtab_off, len(symtab), 1, 24))

    ehdr = bytearray(64)
    ehdr[0:4] = b'\x7fELF'
    ehdr[4] = 2  # ELFCLASS64
    ehdr[5] = 1  # little endian
    ehdr[6] = 1
    struct.pack_into(endian + 'H', ehdr, 16, 3)        # e_type ET_DYN
    struct.pack_into(endian + 'H', ehdr, 18, machine)  # e_machine
    struct.pack_into(endian + 'I', ehdr, 20, 1)
    struct.pack_into(endian + 'Q', ehdr, 40, shoff)    # e_shoff
    struct.pack_into(endian + 'H', ehdr, 58, shentsize)
    struct.pack_into(endian + 'H', ehdr, 60, 3)        # e_shnum
    struct.pack_into(endian + 'H', ehdr, 62, 1)        # e_shstrndx
    return bytes(ehdr) + strtab + symtab + sections


class ChecksumTests(unittest.TestCase):
    def test_dobby_download_blocked_without_optin(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(RuntimeError) as ctx:
                nb.ensure_dobby(tmp, download=False)
            self.assertIn('No download attempted', str(ctx.exception))

    def test_corrupt_cached_archive_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            vendor = nb._vendor_dir(tmp)
            vendor.mkdir(parents=True)
            (vendor / ('dobby-' + nb.DOBBY_COMMIT + '.zip')).write_bytes(b'not a real archive')
            with self.assertRaises(RuntimeError) as ctx:
                nb.ensure_dobby(tmp, download=False)
            self.assertIn('checksum mismatch', str(ctx.exception))

    def test_marker_does_not_bypass_archive_validation(self):
        with tempfile.TemporaryDirectory() as tmp:
            extracted = nb._vendor_dir(tmp) / ('Dobby-' + nb.DOBBY_COMMIT)
            extracted.mkdir(parents=True)
            (extracted / '.pgrazor-verified').write_text(nb.DOBBY_SHA256, encoding='utf-8')
            with self.assertRaisesRegex(RuntimeError, 'No download attempted'):
                nb.ensure_dobby(tmp, download=False)


class ZipSafetyTests(unittest.TestCase):
    def _extract(self, write):
        tmp = tempfile.mkdtemp()
        archive = Path(tmp) / 'a.zip'
        with zipfile.ZipFile(archive, 'w') as zf:
            write(zf)
        return nb.safe_extract(archive, Path(tmp) / 'out')

    def test_traversal_entry_rejected(self):
        with self.assertRaises(ValueError):
            self._extract(lambda zf: zf.writestr('../escape.txt', b'x'))

    def test_absolute_entry_rejected(self):
        with self.assertRaises(ValueError):
            self._extract(lambda zf: zf.writestr('/etc/passwd', b'x'))

    def test_symlink_entry_rejected(self):
        def write(zf):
            info = zipfile.ZipInfo('link')
            info.external_attr = (0o120777 << 16)
            zf.writestr(info, b'/target')
        with self.assertRaises(ValueError):
            self._extract(write)

    def test_duplicate_entries_rejected(self):
        def write(zf):
            zf.writestr('dup.txt', b'a')
            zf.writestr('dup.txt', b'b')
        with self.assertRaises(ValueError):
            self._extract(write)

    def test_normal_entry_extracts(self):
        out = self._extract(lambda zf: zf.writestr('pkg/file.txt', b'hello'))
        self.assertEqual((out / 'pkg' / 'file.txt').read_bytes(), b'hello')


class ExportGuardTests(unittest.TestCase):
    def test_exact_set_passes(self):
        nb.assert_exports({'JNI_OnLoad', 'pgrazor_native_api'})

    def test_missing_export_fails(self):
        with self.assertRaises(RuntimeError) as ctx:
            nb.assert_exports({'JNI_OnLoad'})
        self.assertIn('pgrazor_native_api', str(ctx.exception))

    def test_extra_export_fails(self):
        with self.assertRaises(RuntimeError) as ctx:
            nb.assert_exports({'JNI_OnLoad', 'pgrazor_native_api', 'leaked_symbol'})
        self.assertIn('leaked_symbol', str(ctx.exception))

    def test_parse_exports_ignores_undefined_and_local(self):
        data = _elf64(nb.ELF_MACHINE['arm64-v8a'], [
            ('JNI_OnLoad', 1, 1),
            ('pgrazor_native_api', 1, 1),
            ('imported_func', 1, 0),   # undefined import -> ignored
            ('local_helper', 0, 1),    # LOCAL binding -> ignored
        ])
        with tempfile.NamedTemporaryFile(suffix='.so', delete=False) as handle:
            handle.write(data)
            path = handle.name
        try:
            self.assertEqual(nb.dynamic_exports(path), {'JNI_OnLoad', 'pgrazor_native_api'})
        finally:
            os.unlink(path)


class ElfValidationTests(unittest.TestCase):
    def _write(self, data):
        handle = tempfile.NamedTemporaryFile(suffix='.so', delete=False)
        handle.write(data)
        handle.close()
        self.addCleanup(os.unlink, handle.name)
        return handle.name

    def test_valid_arm64_library_validates(self):
        path = self._write(_elf64(nb.ELF_MACHINE['arm64-v8a'], [
            ('JNI_OnLoad', 1, 1), ('pgrazor_native_api', 1, 1)]))
        nb.validate_library(path, 'arm64-v8a')

    def test_machine_mismatch_fails(self):
        path = self._write(_elf64(nb.ELF_MACHINE['x86_64'], [
            ('JNI_OnLoad', 1, 1), ('pgrazor_native_api', 1, 1)]))
        with self.assertRaises(RuntimeError) as ctx:
            nb.validate_library(path, 'arm64-v8a')
        self.assertIn('machine mismatch', str(ctx.exception))

    def test_non_elf_rejected(self):
        path = self._write(b'MZ not an elf at all........')
        with self.assertRaises(RuntimeError):
            nb.elf_identity(path)


class SourceAndNdkTests(unittest.TestCase):
    def test_missing_native_source_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(RuntimeError) as ctx:
                nb.native_source_root(tmp)
            self.assertIn('CMakeLists', str(ctx.exception))

    def test_source_fingerprint_changes_with_content(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / 'native'
            source.mkdir()
            (source / 'CMakeLists.txt').write_text('project(pgrazor_core)\n', encoding='utf-8')
            (source / 'core.cpp').write_text('int a;\n', encoding='utf-8')
            before = nb.source_fingerprint(source)
            (source / 'core.cpp').write_text('int b;\n', encoding='utf-8')
            after = nb.source_fingerprint(source)
            self.assertNotEqual(before, after)

    def test_stale_ndk_version_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            ndk = Path(tmp) / 'ndk'
            ndk.mkdir()
            (ndk / 'source.properties').write_text('Pkg.Revision = 26.1.0\n', encoding='utf-8')
            saved = {k: os.environ.get(k) for k in ('ANDROID_NDK_HOME', 'ANDROID_NDK_ROOT')}
            os.environ['ANDROID_NDK_HOME'] = str(ndk)
            os.environ.pop('ANDROID_NDK_ROOT', None)
            try:
                with self.assertRaises(RuntimeError) as ctx:
                    nb.resolve_ndk(tmp)
                self.assertIn('revision mismatch', str(ctx.exception))
            finally:
                for key, value in saved.items():
                    if value is None:
                        os.environ.pop(key, None)
                    else:
                        os.environ[key] = value

    def test_build_all_requires_pilot_optin(self):
        saved = os.environ.pop(nb._PILOT_ENV, None)
        try:
            with self.assertRaises(RuntimeError) as ctx:
                nb.build_all('.', ['arm64-v8a'])
            self.assertIn(nb._PILOT_ENV, str(ctx.exception))
        finally:
            if saved is not None:
                os.environ[nb._PILOT_ENV] = saved


if __name__ == '__main__':
    unittest.main()
