"""Focused coverage for the compiled-only production gameplay cutover (default production selection).

These tests exercise the real ZIP rewriting while native/ART/DEX compilation and the Android SDK
commands are inert fixtures, so nothing here builds, installs or touches a device. They assert the
final production APK carries zero PGRazor JavaScript or Frida assets, keeps the compiled native/ART
runtimes and their licenses, and that the packaging report omits every reference to omitted assets.
"""
import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
import axml
import nonroot
import runtime_assets

STANDARD = dict(quick_close=True, quick_ball=True, camera_steering=True)

class CompiledGameplayRejectionTests(unittest.TestCase):
    def test_production_rejects_research_modes_before_build(self):
        for name in ('il2cpp_survey','scene_probe','scene_observe','camera_sample'):
            with self.subTest(name=name), patch.object(nonroot,'validated_build') as build:
                with self.assertRaises(ValueError):
                    nonroot.package(Path('unused.apk'),**{name:True})
                build.assert_not_called()

    def test_retired_or_conflicting_research_options_reject_before_build(self):
        for options in ({'quick_summary':True}, {'research_runtime':True,'quick_summary':True},
                        {'research_runtime':True,'quick_close':True},
                        {'research_runtime':True,'quick_ball':True},
                        {'research_runtime':True,'panel_only':True}):
            with self.subTest(options=options), patch.object(nonroot,'validated_build') as build:
                with self.assertRaises(ValueError):
                    nonroot.package(Path('unused.apk'),**options)
                build.assert_not_called()


class CompiledGameplayPackageTests(unittest.TestCase):
    def setUp(self):
        cleanup_patch = patch.object(nonroot, 'cleanup_generated_builds')
        self.cleanup = cleanup_patch.start(); self.addCleanup(cleanup_patch.stop)
        signing_patch = patch.object(nonroot.build, 'signing_args', return_value=['--ks', 'test-only-unused.jks'])
        signing_patch.start(); self.addCleanup(signing_patch.stop)

    def test_production_apk_has_zero_js_or_frida_and_keeps_native_runtimes(self):
        import native_build
        import art_build
        import dex_bridges
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as temp:
            work = Path(temp)
            source = work / 'game.apk'; dex = work / 'module.dex'; dex.write_bytes(b'dex\nfixture')
            with zipfile.ZipFile(source, 'w') as archive:
                archive.writestr('AndroidManifest.xml', b'manifest')
                archive.writestr('classes.dex', b'original-game-dex')
            # Inert native core compilation outputs plus the bundled Dobby license.
            native_outputs = {}
            for abi in ('arm64-v8a', 'x86_64'):
                native_dir = work / 'build/native/out' / abi; native_dir.mkdir(parents=True)
                binary = native_dir / 'libpgrazor-core.so'; binary.write_bytes(('native fixture ' + abi).encode())
                (native_dir / 'receipt.json').write_text(json.dumps({'abi': abi, 'fixture': True}))
                native_outputs[abi] = binary
            dobby = work / 'dobby-fixture'; dobby.mkdir(); (dobby / 'LICENSE').write_text('Dobby fixture license')
            # Inert compiled Location ART outputs, receipt and the five dependency licenses.
            art_session = work / 'build/art/fixture-session'; art_outputs = {}; art_abis = {}
            for abi in ('arm64-v8a', 'x86_64'):
                directory = art_session / abi; directory.mkdir(parents=True)
                binary = directory / 'libpgrazor-art.so'; binary.write_bytes(('stripped ART fixture ' + abi).encode())
                art_outputs[abi] = binary
                art_abis[abi] = {'stripped': str(binary.resolve()), 'sha256': runtime_assets.sha256(binary), 'exports': ['JNI_OnLoad']}
            licenses = []
            for index in range(5):
                license_path = art_session / ('license-' + str(index)); license_path.write_text('ART dependency license ' + str(index))
                licenses.append(str(license_path))
            art_receipt = {'success': True, 'abis': art_abis, 'license_sources': licenses,
                           'dependencies': {'fixture': {'commit': 'fixture-pin'}}, 'relinkability': 'retain fixture objects locally'}
            (art_session / 'receipt.json').write_text(json.dumps(art_receipt))
            info = {'package': 'com.nianticlabs.pokemongo', 'split': None,
                    'providers': [{'name': axml.PROVIDER, 'exported': False}]}

            def sdk_fixture(*args, **kwargs):
                if str(args[0]).endswith('zipalign.exe') and args[1] == '-f':
                    shutil.copyfile(args[-2], args[-1])
                elif args[1] == 'sign':
                    shutil.copyfile(args[-1], args[args.index('--out') + 1])
                return Mock(stdout='Signer #1 certificate SHA-256 digest: ' + nonroot.NIANTIC_CERT_SHA256)

            original_hash = runtime_assets.sha256(source)
            def compile_native_fixture(*args, **kwargs):
                self.assertEqual(os.environ.get('PGRAZOR_NATIVE_PILOT'), '1')
                return native_outputs
            for options in ({}, {'interaction_probe': True}):
                with self.subTest(options=options), \
                     patch.dict(os.environ, {key:'0' for key in ('PGRAZOR_COMPILED_GAMEPLAY','PGRAZOR_NATIVE_PILOT','PGRAZOR_COMPILED_LOCATION','PGRAZOR_COMPILED_JAVA_BRIDGES','PGRAZOR_COMPILED_NATIVE_TRANSPORT')}), \
                     patch.object(nonroot, 'ROOT', work), patch.object(nonroot, 'validated_build', return_value=dex), \
                     patch.object(nonroot, 'inspect_set', return_value=(info['package'], source, [(source, info)], {'arm64-v8a'})), \
                     patch.object(nonroot, 'command', side_effect=sdk_fixture), \
                     patch.object(axml, 'inspect_manifest', return_value=info), \
                     patch.object(axml, 'patch_manifest', side_effect=lambda data: data), \
                     patch.object(axml, 'add_account_restart_activity', side_effect=lambda data: data), \
                     patch.object(native_build, 'build_all', side_effect=compile_native_fixture), \
                     patch.object(native_build, 'ensure_dobby', return_value=dobby), \
                     patch.object(art_build, 'build_all', return_value=art_outputs), \
                     patch.object(dex_bridges, 'rewrite', return_value=({}, {'fixture': True})), \
                     contextlib.redirect_stdout(io.StringIO()):
                    report, folder = nonroot.package(source, **options)
                    self.assertEqual(os.environ.get('PGRAZOR_NATIVE_PILOT'), '0')

                self.assertEqual(json.loads((folder / 'report.json').read_text()), report)
                self.assertEqual(report['mode'], 'compiled-gameplay')
                for omitted in ('bridge_sha256', 'bridge_upstream_patch', 'agent_sha256', 'script_bundles', 'diagnostics'):
                    self.assertNotIn(omitted, report)
                self.assertEqual(report['runtime'], [])
                self.assertIn('native_pilot', report)
                self.assertIn('compiled_location_getters', report)
                self.assertIn('compiled_java_bridges', report)

                with zipfile.ZipFile(report['outputs'][0]['file']) as archive:
                    names = set(archive.namelist())
                    mode = json.loads(archive.read('assets/pgrazor/mode.json'))
                    self.assertIs(mode['gadget'], False)
                    self.assertIs(mode['compiledGameplay'], True)
                    for flag in ('nativePilot', 'compiledJavaBridges', 'compiledLocationGetters',
                                 'compiledNativeTransport', 'compiledCatchWait', 'quickSummary', 'quickClose',
                                 'quickBall', 'cameraSteering', 'cameraProbe'):
                        self.assertIs(mode.get(flag), True, flag)
                    self.assertEqual(mode.get('interactionProbe', False), options.get('interaction_probe', False))
                    self.assertEqual(mode['preferredAbi'], '')
                    self.assertEqual(mode['runtimeAbis'], ['arm64-v8a', 'x86_64'])

                    # Zero PGRazor JavaScript and zero Frida Gadget anywhere in the final APK.
                    self.assertFalse(any(n.startswith('assets/pgrazor/') and n.endswith('.js') for n in names), names)
                    self.assertFalse(any(n.endswith('libfrida-gadget.so') for n in names), names)
                    self.assertFalse(any(n.startswith('assets/pgrazor/bridge-notices/') for n in names), names)
                    for omitted in ('assets/pgrazor/embedded-agent.js', 'assets/pgrazor/java-bridge.js',
                                    'assets/pgrazor/quick-close.js', 'assets/pgrazor/quick-summary.js',
                                    'assets/pgrazor/quick-ball.js', 'assets/pgrazor/camera-probe.js',
                                    'assets/pgrazor/interaction-probe.js'):
                        self.assertNotIn(omitted, names)

                    # Compiled native core, ART getters and their licenses are retained.
                    self.assertIn('assets/pgrazor/native-notices/Dobby-LICENSE', names)
                    self.assertEqual(len({n for n in names if n.startswith('assets/pgrazor/art-notices/')}), 5)
                    for abi, binary in native_outputs.items():
                        self.assertEqual(archive.read('assets/pgrazor/' + abi + '/libpgrazor-core.so'), binary.read_bytes())
                    for abi, binary in art_outputs.items():
                        self.assertEqual(archive.read('assets/pgrazor/' + abi + '/libpgrazor-art.so'), binary.read_bytes())
                    # The original game payload is preserved untouched.
                    self.assertEqual(archive.read('classes.dex'), b'original-game-dex')

            self.assertEqual(runtime_assets.sha256(source), original_hash)


if __name__ == '__main__':
    unittest.main()
