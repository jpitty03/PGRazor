"""Compile selected isolated native tests; optionally run on an explicit ADB device.

Never installs an APK, starts the game, or attaches to its process. The caller
must build/review the native core separately before running a harness against it.
"""
import argparse
import datetime
import json
import platform
import re
import subprocess
from pathlib import Path

import native_build


def main(argv=None):
    root = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('tests', nargs='+', help='Native test stem, e.g. map_tap or buddy_feeding')
    parser.add_argument('--abi', choices=native_build.DEFAULT_ABIS, default='x86_64')
    parser.add_argument('--serial', help='Explicit device serial; omitted means compile only')
    parser.add_argument('--adb', default='adb')
    parser.add_argument('--library', type=Path, help='Reviewed core to test; defaults to native build output')
    args = parser.parse_args(argv)
    sources = []
    for name in args.tests:
        if not re.fullmatch(r'[a-z][a-z0-9_]*', name):
            parser.error('Invalid test stem: ' + name)
        source = root / 'native' / (name + '_test.cpp')
        if name in ('event_collector',):
            source = root / 'native/events' / (name + '_test.cpp')
        if not source.is_file():
            parser.error('No harness source: ' + str(source))
        sources.append(source)
    ndk, revision = native_build.resolve_ndk(root)
    host = {'Windows': 'windows-x86_64', 'Linux': 'linux-x86_64', 'Darwin': 'darwin-x86_64'}[platform.system()]
    triple = 'aarch64-linux-android' if args.abi == 'arm64-v8a' else 'x86_64-linux-android'
    compiler = ndk / 'toolchains/llvm/prebuilt' / host / 'bin' / (
        triple + str(native_build.API_LEVEL) + '-clang++' + ('.cmd' if platform.system() == 'Windows' else ''))
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%d-%H%M%S-%f')
    output = root / 'build/native/harness' / stamp
    output.mkdir(parents=True)
    receipt = {'abi': args.abi, 'ndk': revision, 'serial': args.serial, 'tests': [], 'success': False}

    def run(command):
        result = subprocess.run([str(x) for x in command], capture_output=True, text=True, timeout=90)
        if result.returncode:
            raise RuntimeError('Command failed (exit ' + str(result.returncode) + '): ' + str(command[0]) + '\n' + result.stdout + result.stderr)
        return result.stdout + result.stderr

    try:
        library = (args.library or root / 'build/native/out' / args.abi / native_build.OUTPUT_SONAME).resolve()
        remote = '/data/local/tmp/pgrazor-harness-' + stamp
        adb = [args.adb, '-s', args.serial] if args.serial else None
        if adb:
            # MuMu may report aarch64 through uname even with an x86_64 host
            # linker. Check the ELF itself rather than an advertised guest ABI.
            header = bytes(int(x) for x in run(adb + [
                'shell', 'od', '-An', '-tu1', '-N20', '/system/bin/linker64']).split())
            if len(header) != 20 or header[:6] != b'\x7fELF\x02\x01':
                raise RuntimeError('Cannot identify the device native linker ABI')
            machine = int.from_bytes(header[18:20], 'little')
            expected = {'arm64-v8a': 183, 'x86_64': 62}[args.abi]
            if machine != expected:
                raise RuntimeError('Harness ABI does not match device linker: ' + str(machine))
            receipt['device_elf_machine'] = machine
            native_build.validate_library(library, args.abi, ndk)
            receipt['library'] = str(library)
            receipt['library_sha256'] = native_build.sha256(library)
            run(adb + ['shell', 'mkdir', '-p', remote])
            run(adb + ['push', library, remote + '/core.so'])
        for source in sources:
            executable = output / source.stem
            entry = {'source': str(source.relative_to(root)), 'source_sha256': native_build.sha256(source)}
            receipt['tests'].append(entry)
            extra_flags = []
            if source.stem in ('event_collector_test',):
                vendor = native_build.ensure_dobby(root, download=False)
                extra_flags = ['-fexceptions', '-I'+str(vendor/'include'), '-llog']
                entry['included_source_sha256'] = native_build.sha256(source.with_name(source.stem.removesuffix('_test')+'.cpp'))
            if source.stem == 'runtime_host_flow_test':
                extra_flags = ['-llog']
            if source.stem == 'code_patch_boundary_test':
                vendor = native_build.ensure_dobby(root, download=False)
                dobby = root / 'build/native/build' / args.abi / 'dobby/libdobby.a'
                extra_flags = ['-I'+str(vendor/'include'), dobby, '-llog']
                entry['dobby_sha256'] = native_build.sha256(dobby)
            if source.stem == 'hook_engine_near_test':
                vendor = native_build.ensure_dobby(root, download=False)
                dobby = root / 'build/native/build' / args.abi / 'dobby/libdobby.a'
                hook_engine = root / 'native/hook_engine.cpp'
                extra_flags = [hook_engine, '-I'+str(vendor/'include'), dobby, '-llog']
                entry['dobby_sha256'] = native_build.sha256(dobby)
                entry['hook_engine_sha256'] = native_build.sha256(hook_engine)
            if source.stem in ('catch_observers_interaction_test', 'spin_camera_interaction_test', 'quick_spin_interaction_test'):
                vendor = native_build.ensure_dobby(root, download=False)
                dobby = root / 'build/native/build' / args.abi / 'dobby/libdobby.a'
                owner = root / 'native' / (source.stem.removesuffix('_interaction_test') + '.cpp')
                hook_engine = root / 'native/hook_engine.cpp'
                extra_flags = [owner, hook_engine, '-fexceptions', '-I'+str(vendor/'include'), dobby, '-llog']
                entry['dobby_sha256'] = native_build.sha256(dobby)
                entry['owner_sha256'] = native_build.sha256(owner)
                entry['hook_engine_sha256'] = native_build.sha256(hook_engine)
                entry['observer_header_sha256'] = native_build.sha256(root/'native/interaction_observer.h')
            if source.stem in ('shared_host_flow_test', 'camera_host_test', 'interaction_host_test', 'encounter_host_test'):
                vendor = native_build.ensure_dobby(root, download=False)
                dobby = root / 'build/native/build' / args.abi / 'dobby/libdobby.a'
                if not dobby.is_file():
                    raise RuntimeError('Build the matching native core before host flow fixtures')
                extra_flags = ['-fexceptions', '-I'+str(vendor/'include'), dobby, '-llog']
                entry['dobby_sha256'] = native_build.sha256(dobby)
                if source.stem == 'encounter_host_test':
                    extra_flags.insert(0, root/'native/hook_engine.cpp')
                host_name = {'shared_host_flow_test':'shared_host.cpp','camera_host_test':'camera_host.cpp','interaction_host_test':'interaction_host.cpp','encounter_host_test':'encounter_host.cpp'}[source.stem]
                entry['included_host_sha256'] = native_build.sha256(root/'native'/host_name)
            entry['compile_output'] = run([compiler, '-O2', '-std=c++17', '-static-libstdc++', source, *extra_flags, '-ldl', '-o', executable])
            entry['executable_sha256'] = native_build.sha256(executable)
            if adb:
                target = remote + '/' + source.stem
                run(adb + ['push', executable, target])
                run(adb + ['shell', 'chmod', '700', target])
                entry['run_output'] = run(adb + ['shell', target, remote + '/core.so'])
                print(entry['run_output'].strip())
            else:
                print('Compiled ' + source.stem + ' (' + args.abi + ')')
        receipt['success'] = True
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        receipt['error'] = str(error)
        print(str(error))
    finally:
        path = output / 'receipt.json'
        path.write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
        print('Receipt: ' + str(path))
    return 0 if receipt['success'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
