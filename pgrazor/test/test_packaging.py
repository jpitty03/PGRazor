import hashlib
import importlib.util
import contextlib
import io
import os
import json
import shutil
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch
import zipfile

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT))
import axml
import nonroot
import runtime_assets

class PackageTests(unittest.TestCase):
    def setUp(self):
        # CLI tests must never run the real deletion workflow.
        cleanup_patch=patch.object(nonroot,'cleanup_generated_builds')
        self.cleanup=cleanup_patch.start()
        self.addCleanup(cleanup_patch.stop)
        signing_patch=patch.object(nonroot.build,"signing_args",return_value=["--ks","test-only-unused.jks"])
        signing_patch.start();self.addCleanup(signing_patch.stop)

    def test_research_cli_is_explicit_and_never_installs(self):
        for flag in ('--il2cpp-survey','--scene-probe','--scene-observe','--camera-sample'):
            with self.subTest(flag=flag), patch.object(nonroot,'package',return_value=({'outputs':[]},ROOT)) as package, patch.object(nonroot,'probe') as install, contextlib.redirect_stdout(io.StringIO()):
                nonroot.main(['--apk','game.apkm','--research-runtime',flag,'--skip-build'])
                self.assertTrue(package.call_args.kwargs['research_runtime'])
                install.assert_not_called()

    def test_retired_summary_and_research_catch_cli_reject(self):
        with patch.object(nonroot,'package') as package, contextlib.redirect_stderr(io.StringIO()):
            for flags in (['--quick-summary'],['--research-runtime','--quick-summary'],
                          ['--research-runtime','--quick-close'],['--research-runtime','--quick-ball'],
                          ['--research-runtime','--panel-only']):
                with self.subTest(flags=flags), self.assertRaises(SystemExit):
                    nonroot.main(['--apk','game.apkm','--skip-build',*flags])
            package.assert_not_called()

    def test_emulator_runtime_is_explicit_and_never_installs(self):
        with patch.object(nonroot,'package',return_value=({'outputs':[]},ROOT)) as package, patch.object(nonroot,'probe') as install, contextlib.redirect_stdout(io.StringIO()):
            nonroot.main(['--apk','game.apkm','--runtime-abi','x86_64','--skip-build'])
        self.assertEqual(package.call_args.kwargs['game_runtime_abi'],'x86_64')
        install.assert_not_called()
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            nonroot.main(['--probe','--runtime-abi','x86_64'])

    def test_research_zip_preserves_game_and_bundles_only_retained_probes(self):
        with tempfile.TemporaryDirectory(dir=ROOT/'build') as temp:
            work=Path(temp);source=work/'game.apk';dex=work/'module.dex'
            dex.write_bytes(b'dex fixture')
            with zipfile.ZipFile(source,'w') as archive:
                archive.writestr('AndroidManifest.xml',b'manifest')
                archive.writestr('classes.dex',b'original-game-dex')
            for name in ('runtime','diagnostics','vendor'):
                shutil.copytree(ROOT/name,work/name)
            shutil.copyfile(ROOT/'embedded-agent.js',work/'embedded-agent.js')
            bridge=work/'bridge.js';bridge.write_bytes(b'// bridge fixture')
            gadget=work/'gadget.so';gadget.write_bytes(b'gadget fixture')
            info={'package':'com.nianticlabs.pokemongo','split':None,
                  'providers':[{'name':axml.PROVIDER,'exported':False}]}
            def sdk_fixture(*args,**kwargs):
                if str(args[0]).endswith('zipalign.exe') and args[1]=='-f':
                    shutil.copyfile(args[-2],args[-1])
                elif args[1]=='sign':
                    shutil.copyfile(args[-1],args[args.index('--out')+1])
                return Mock(stdout='Signer #1 certificate SHA-256 digest: '+nonroot.NIANTIC_CERT_SHA256)
            original_hash=runtime_assets.sha256(source)
            with patch.object(nonroot,'ROOT',work), patch.object(nonroot,'validated_build',return_value=dex), \
                 patch.object(nonroot,'inspect_set',return_value=(info['package'],source,[(source,info)],{'arm64-v8a'})), \
                 patch.object(nonroot,'command',side_effect=sdk_fixture), \
                 patch.object(runtime_assets,'gadget',return_value=(gadget,{'abi':'fixture'})), \
                 patch.object(runtime_assets,'java_bridge',return_value=bridge), \
                 patch.object(axml,'inspect_manifest',return_value=info), \
                 patch.object(axml,'patch_manifest',side_effect=lambda data:data), \
                 patch.object(axml,'add_account_restart_activity',side_effect=lambda data:data), \
                 contextlib.redirect_stdout(io.StringIO()):
                for options in ({},{'il2cpp_survey':True},{'scene_probe':True},
                                {'scene_observe':True},{'camera_sample':True},{'interaction_probe':True}):
                    with self.subTest(options=options):
                        report,folder=nonroot.package(source,research_runtime=True,**options)
                        self.assertEqual(report['mode'],'research-runtime')
                        with zipfile.ZipFile(report['outputs'][0]['file']) as archive:
                            names=set(archive.namelist())
                            mode=json.loads(archive.read('assets/pgrazor/mode.json'))
                            self.assertTrue(mode['gadget'])
                            self.assertFalse(mode.get('compiledGameplay',False))
                            self.assertEqual(archive.read('classes.dex'),b'original-game-dex')
                            self.assertIn('assets/pgrazor/embedded-agent.js',names)
                            self.assertIn('assets/pgrazor/java-bridge.js',names)
                            self.assertFalse(any(name.endswith(('/quick-close.js','/quick-ball.js','/quick-summary.js')) for name in names))
                            for name,receipt in report['script_bundles'].items():
                                self.assertEqual(receipt['sha256'],hashlib.sha256(archive.read('assets/pgrazor/'+name)).hexdigest())
            self.assertEqual(runtime_assets.sha256(source),original_hash)

    def test_clean_game_baseline_rejects_both_observed_modified_signers(self):
        nonroot.require_clean_game_certificate([nonroot.NIANTIC_CERT_SHA256])
        for signer in ['e601d7e0383f0cc1e0f0a4243394e42cafba6e7cedf1af377f9052c3a00a46a4','1e08a903aef9c3a721510b64ec764d01d3d094eb954161b62544ea8f187b5953']:
            with self.assertRaisesRegex(ValueError,'clean original'):
                nonroot.require_clean_game_certificate([signer])

    def test_rewrite_preserves_app_payload_and_original(self):
        source=ROOT/'build/pgrazor-preview.apk'
        if not source.exists(): self.skipTest('Preview APK not built')
        original_hash=runtime_assets.sha256(source)
        with tempfile.TemporaryDirectory(dir=ROOT/'build') as temp:
            output=Path(temp)/'patched.apk'
            nonroot.rewrite_apk(source,output,base=True,preview=True,assets={'assets/pgrazor/mode.json':b'{"gadget":false}'})
            with zipfile.ZipFile(source) as before,zipfile.ZipFile(output) as after:
                for name in before.namelist():
                    if name=='AndroidManifest.xml' or nonroot.signature_entry(name): continue
                    self.assertEqual(before.read(name),after.read(name),name)
                self.assertFalse(any(nonroot.signature_entry(name) for name in after.namelist()))
                info=axml.inspect_manifest(after.read('AndroidManifest.xml'))
                self.assertTrue(any(p['name']==axml.PROVIDER and p['exported'] is False for p in info['providers']))
            self.assertEqual(original_hash,runtime_assets.sha256(source))
            with self.assertRaisesRegex(ValueError,'already has PGRazor'):
                nonroot.rewrite_apk(output,Path(temp)/'twice.apk',base=True,preview=True)

    def test_bundle_paths_are_never_used_as_output_paths(self):
        with tempfile.TemporaryDirectory(dir=ROOT/'build') as temp:
            work=Path(temp); bundle=work/'input.apkm'
            with zipfile.ZipFile(bundle,'w') as out: out.writestr('../../outside.apk',b'fixture')
            apks=nonroot.input_apks(bundle,work)
            self.assertEqual(apks,[work/'input-0.apk']); self.assertEqual(apks[0].read_bytes(),b'fixture')

    def test_missing_runtime_never_downloads_without_flag(self):
        with tempfile.TemporaryDirectory(dir=ROOT/'build') as temp,patch.object(runtime_assets,'ROOT',Path(temp)),patch.object(runtime_assets,'get') as network:
            with self.assertRaisesRegex(RuntimeError,'No download attempted'): runtime_assets.gadget('arm64-v8a')
            network.assert_not_called()

    def test_bridge_is_pinned_and_tampering_is_rejected(self):
        self.assertEqual(runtime_assets.sha256(runtime_assets.java_bridge()),runtime_assets.BRIDGE_SHA256)
        with tempfile.TemporaryDirectory(dir=ROOT/'build') as temp,patch.object(runtime_assets,'ROOT',Path(temp)):
            with self.assertRaisesRegex(RuntimeError,'missing'): runtime_assets.java_bridge()
            vendor=Path(temp)/'vendor'; vendor.mkdir()
            (vendor/'frida-java-bridge-302a588.js').write_text('modified bridge')
            with self.assertRaisesRegex(RuntimeError,'checksum mismatch'): runtime_assets.java_bridge()

    def test_elf_architecture_verified(self):
        with tempfile.TemporaryDirectory(dir=ROOT/'build') as temp:
            path=Path(temp)/'gadget.so'; header=bytearray(64); header[:6]=b'\x7fELF\x02\x01'; struct.pack_into('<HH',header,16,3,183); path.write_bytes(header)
            runtime_assets.verify_elf(path,'arm64-v8a')
            with self.assertRaisesRegex(RuntimeError,'architecture'): runtime_assets.verify_elf(path,'x86_64')

    def test_stale_source_rejected(self):
        with tempfile.TemporaryDirectory(dir=ROOT/'build') as temp,patch.object(nonroot,'ROOT',Path(temp)):
            work=Path(temp); (work/'build').mkdir(); (work/'src').mkdir(); (work/'AndroidManifest.xml').write_text('new')
            (work/'VERSION').write_text('0.1.6')
            (work/'build/build-state.json').write_text(json.dumps({'sources':{}}))
            with self.assertRaisesRegex(RuntimeError,'stale'): nonroot.validated_build()

    def test_current_receipt_accepts_version_and_rejects_changed_release(self):
        with tempfile.TemporaryDirectory(dir=ROOT/'build') as temp, patch.object(nonroot,'ROOT',Path(temp)), \
             patch.dict(nonroot.build.os.environ, {'PGRAZOR_VERSION':'v0.1.6'}):
            work=Path(temp); (work/'build').mkdir(); (work/'src').mkdir()
            (work/'AndroidManifest.xml').write_text('manifest')
            (work/'VERSION').write_text('0.1.6')
            (work/'src/Example.java').write_text('class Example {}')
            dex=work/'build/pgrazor.dex'; dex.write_bytes(b'dex\nLdev/pgrazor/BootstrapProvider;')
            state={'version':nonroot.build.release_version(work),
                   'sources':nonroot.build.source_fingerprints(work),
                   'dex_sha256':runtime_assets.sha256(dex)}
            (work/'build/build-state.json').write_text(json.dumps(state))
            self.assertEqual(nonroot.validated_build(),dex)
            with patch.dict(nonroot.build.os.environ, {'PGRAZOR_VERSION':'v0.1.7'}):
                with self.assertRaisesRegex(RuntimeError,'version is stale'): nonroot.validated_build()
            (work/'VERSION').write_text('0.1.7')
            with self.assertRaisesRegex(RuntimeError,'classes are stale'): nonroot.validated_build()

    def test_game_install_is_never_a_probe(self):
        with self.assertRaisesRegex(RuntimeError,'restricted'):
            nonroot.probe({'package':'com.nianticlabs.pokemongo','outputs':[{}]},ROOT,None)

    def test_phone_build_does_not_install_or_capture(self):
        report={'outputs':[{'file':'phone-preview.apk'}]}
        with patch.object(nonroot,'package',return_value=(report,ROOT)) as package, \
             patch.object(nonroot,'probe') as install, patch.object(nonroot,'command') as command, \
             contextlib.redirect_stdout(io.StringIO()):
            nonroot.main(['--build-probe','--research-runtime','--abi','arm64-v8a','--skip-build'])
        package.assert_called_once_with(ROOT/'build/pgrazor-preview.apk',panel_only=False,download=False,preview=True,probe_abi='arm64-v8a',research_runtime=True)
        install.assert_not_called(); command.assert_not_called()

    def test_phone_build_requires_explicit_architecture(self):
        with patch.object(nonroot,'package') as package, patch.object(nonroot,'command') as command, contextlib.redirect_stderr(io.StringIO()):
            for args in (['--build-probe'],['--probe','--abi','arm64-v8a'],['--apk','game.apk','--abi','arm64-v8a']):
                with self.assertRaises(SystemExit): nonroot.main(args)
        package.assert_not_called(); command.assert_not_called()

    def test_phone_probe_uses_selected_device_and_its_architecture(self):
        android=Mock(); android.shell.return_value='arm64-v8a\n'
        monitor=Mock(); monitor.select_device.return_value=android
        report={'outputs':[{'file':'phone-preview.apk'}]}
        with patch.dict(sys.modules,{'monitor':monitor}), patch.object(nonroot,'package',return_value=(report,ROOT)) as package, patch.object(nonroot,'probe') as probe:
            nonroot.main(['--probe','--research-runtime','--serial','PHONE123','--skip-build'])
        monitor.select_device.assert_called_once_with(monitor.find_adb.return_value,'PHONE123')
        self.assertEqual(package.call_args.kwargs['probe_abi'],'arm64-v8a')
        probe.assert_called_once_with(report,ROOT,android,0)

    def test_missing_selected_phone_stops_before_packaging(self):
        monitor=Mock(); monitor.select_device.side_effect=RuntimeError('not online')
        with patch.dict(sys.modules,{'monitor':monitor}), patch.object(nonroot,'package') as package, patch.object(nonroot,'command') as command:
            with self.assertRaisesRegex(RuntimeError,'PHONE123'):
                nonroot.main(['--probe','--research-runtime','--serial','PHONE123'])
        package.assert_not_called(); command.assert_not_called()

class CleanupIntegrationTests(unittest.TestCase):
    def test_cleanup_uses_native_powershell_apply_without_shell_interpolation(self):
        with patch.object(nonroot.subprocess,'run') as run:
            nonroot.cleanup_generated_builds()
        run.assert_called_once_with(['powershell.exe','-NoProfile','-ExecutionPolicy','Bypass',
            '-File',str(ROOT/'cleanup.ps1'),'-Apply'],check=True,timeout=300)

    def test_cleanup_failures_warn_without_invalidating_packaging(self):
        failures=[OSError('PowerShell unavailable'),nonroot.subprocess.CalledProcessError(1,['cleanup']),
                  nonroot.subprocess.TimeoutExpired(['cleanup'],300)]
        for failure in failures:
            output=io.StringIO()
            with patch.object(nonroot.subprocess,'run',side_effect=failure), contextlib.redirect_stderr(output):
                nonroot.cleanup_generated_builds()
            self.assertIn('packaging succeeded',output.getvalue())
            self.assertIn('cleanup did not finish',output.getvalue())

    def test_cleanup_occurs_after_successful_package_before_probe_capture(self):
        android=Mock(); android.shell.return_value='arm64-v8a\n'
        monitor=Mock(); monitor.select_device.return_value=android
        report={'outputs':[{'file':'phone-preview.apk'}]}; calls=Mock()
        with patch.dict(sys.modules,{'monitor':monitor}), \
             patch.object(nonroot,'package',return_value=(report,ROOT)) as package, \
             patch.object(nonroot,'cleanup_generated_builds') as cleanup, patch.object(nonroot,'probe') as probe:
            calls.attach_mock(package,'package');calls.attach_mock(cleanup,'cleanup');calls.attach_mock(probe,'probe')
            nonroot.main(['--probe','--research-runtime','--serial','PHONE123','--skip-build'])
        self.assertEqual([call[0] for call in calls.mock_calls],['package','cleanup','probe'])

    def test_failed_package_does_not_clean(self):
        with patch.object(nonroot,'package',side_effect=RuntimeError('build failed')), \
             patch.object(nonroot,'cleanup_generated_builds') as cleanup:
            with self.assertRaisesRegex(RuntimeError,'build failed'):
                nonroot.main(['--apk','game.apk','--skip-build'])
        cleanup.assert_not_called()


if __name__=='__main__': unittest.main()
