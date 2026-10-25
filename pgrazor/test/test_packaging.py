import hashlib
import importlib.util
import contextlib
import io
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

    def test_quick_close_cli_and_invalid_modes(self):
        with patch.object(nonroot,'package',return_value=({'outputs':[]},ROOT)) as package, \
             patch.object(nonroot,'probe') as install, patch.object(nonroot,'command') as command, \
             contextlib.redirect_stdout(io.StringIO()):
            nonroot.main(['--apk','game.apkm','--quick-close','--quick-ball','--camera-steering','--skip-build'])
            self.assertTrue(package.call_args.kwargs['quick_close'])
            self.assertNotIn('quick_summary',package.call_args.kwargs)
            install.assert_not_called(); command.assert_not_called()
        with patch.object(nonroot,'validated_build') as build, contextlib.redirect_stderr(io.StringIO()):
            for extra in (['--quick-summary'],['--panel-only'],['--il2cpp-survey'],['--scene-probe'],['--camera-sample']):
                with self.assertRaises(SystemExit):
                    nonroot.main(['--apk','game.apkm','--quick-close']+extra)
            for options in ({'quick_summary':True},{'panel_only':True},{'game_runtime_abi':'x86_64'},{'il2cpp_survey':True}):
                with self.assertRaises(ValueError): nonroot.package(Path('unused.apk'),quick_close=True,**options)
            build.assert_not_called()

    def test_quick_summary_cli_accepts_supported_combinations_without_installing(self):
        for extra in ([],['--camera-steering'],['--interaction-probe'],['--camera-steering','--interaction-probe'],['--camera-steering','--camera-probe']):
            with patch.object(nonroot,'package',return_value=({'outputs':[]},ROOT)) as package, \
                 patch.object(nonroot,'probe') as install, patch.object(nonroot,'command') as command, \
                 contextlib.redirect_stdout(io.StringIO()):
                nonroot.main(['--apk','game.apkm','--quick-summary','--skip-build']+extra)
            self.assertTrue(package.call_args.kwargs['quick_summary'])
            self.assertEqual(package.call_args.kwargs.get('camera_steering',False),'--camera-steering' in extra)
            self.assertEqual(package.call_args.kwargs.get('interaction_probe',False),'--interaction-probe' in extra)
            install.assert_not_called(); command.assert_not_called()

    def test_quick_summary_rejects_incompatible_cli_before_build(self):
        with patch.object(nonroot,'package') as package, patch.object(nonroot,'command') as command, \
             contextlib.redirect_stderr(io.StringIO()):
            for flag in ['--panel-only','--il2cpp-survey','--scene-probe','--scene-observe','--camera-probe','--camera-sample']:
                with self.assertRaises(SystemExit):
                    nonroot.main(['--apk','game.apk','--quick-summary',flag])
            for args in (['--apk','game.apk','--runtime-abi','x86_64'], ['--build-probe','--abi','x86_64']):
                with self.assertRaisesRegex(ValueError,'--quick-summary requires'):
                    nonroot.main(args+['--quick-summary'])
        package.assert_not_called(); command.assert_not_called(); self.cleanup.assert_not_called()

    def test_quick_summary_api_rejects_incompatible_modes_before_outputs(self):
        with patch.object(nonroot,'validated_build') as validated, patch.object(nonroot,'input_apks') as inputs:
            for kwargs in ({'panel_only':True}, {'game_runtime_abi':'x86_64'}, {'preview':True,'probe_abi':'x86_64'}, {'preview':True}):
                with self.assertRaisesRegex(ValueError,'--quick-summary requires'):
                    nonroot.package(Path('unused.apk'),quick_summary=True,**kwargs)
            for flag in ['il2cpp_survey','scene_probe','scene_observe','camera_probe','camera_sample']:
                with self.assertRaisesRegex(ValueError,'cannot be combined'):
                    nonroot.package(Path('unused.apk'),quick_summary=True,**{flag:True})
        validated.assert_not_called(); inputs.assert_not_called()

    def test_quick_summary_x64_probe_stops_after_readonly_architecture_check(self):
        android=Mock(); android.shell.return_value='x86_64\n'
        monitor=Mock(); monitor.select_device.return_value=android
        with patch.dict(sys.modules,{'monitor':monitor}), patch.object(nonroot,'package') as package, \
             patch.object(nonroot,'probe') as install, patch.object(nonroot,'command') as command:
            with self.assertRaisesRegex(ValueError,'--quick-summary requires'):
                nonroot.main(['--probe','--quick-summary'])
        package.assert_not_called(); install.assert_not_called(); command.assert_not_called()
        android.call.assert_not_called()
        android.shell.assert_called_once_with('getprop','ro.product.cpu.abi')

    def test_quick_ball_cli_accepts_supported_combinations_without_installing(self):
        for extra in (['--quick-summary','--camera-steering','--interaction-probe'],[],['--camera-steering'],['--interaction-probe'],['--camera-steering','--interaction-probe'],['--camera-steering','--camera-probe']):
            with patch.object(nonroot,'package',return_value=({'outputs':[]},ROOT)) as package, \
                 patch.object(nonroot,'probe') as install, patch.object(nonroot,'command') as command, \
                 contextlib.redirect_stdout(io.StringIO()):
                nonroot.main(['--apk','game.apkm','--quick-ball','--skip-build']+extra)
            self.assertTrue(package.call_args.kwargs['quick_ball'])
            self.assertEqual(package.call_args.kwargs.get('camera_steering',False),'--camera-steering' in extra)
            self.assertEqual(package.call_args.kwargs.get('interaction_probe',False),'--interaction-probe' in extra)
            install.assert_not_called(); command.assert_not_called()

    def test_quick_ball_rejects_incompatible_cli_before_build(self):
        with patch.object(nonroot,'package') as package, patch.object(nonroot,'command') as command, \
             contextlib.redirect_stderr(io.StringIO()):
            for flag in ['--panel-only','--il2cpp-survey','--scene-probe','--scene-observe','--camera-probe','--camera-sample']:
                with self.assertRaises(SystemExit):
                    nonroot.main(['--apk','game.apk','--quick-ball',flag])
            for args in (['--apk','game.apk','--runtime-abi','x86_64'], ['--build-probe','--abi','x86_64']):
                with self.assertRaisesRegex(ValueError,'--quick-ball requires'):
                    nonroot.main(args+['--quick-ball'])
        package.assert_not_called(); command.assert_not_called(); self.cleanup.assert_not_called()

    def test_quick_ball_api_rejects_incompatible_modes_before_outputs(self):
        with patch.object(nonroot,'validated_build') as validated, patch.object(nonroot,'input_apks') as inputs:
            for kwargs in ({'panel_only':True}, {'game_runtime_abi':'x86_64'}, {'preview':True,'probe_abi':'x86_64'}, {'preview':True}):
                with self.assertRaisesRegex(ValueError,'--quick-ball requires'):
                    nonroot.package(Path('unused.apk'),quick_ball=True,**kwargs)
            for flag in ['il2cpp_survey','scene_probe','scene_observe','camera_probe','camera_sample']:
                with self.assertRaisesRegex(ValueError,'cannot be combined'):
                    nonroot.package(Path('unused.apk'),quick_ball=True,**{flag:True})
        validated.assert_not_called(); inputs.assert_not_called()

    def test_quick_ball_x64_probe_stops_after_readonly_architecture_check(self):
        android=Mock(); android.shell.return_value='x86_64\n'
        monitor=Mock(); monitor.select_device.return_value=android
        with patch.dict(sys.modules,{'monitor':monitor}), patch.object(nonroot,'package') as package, \
             patch.object(nonroot,'probe') as install, patch.object(nonroot,'command') as command:
            with self.assertRaisesRegex(ValueError,'--quick-ball requires'):
                nonroot.main(['--probe','--quick-ball'])
        package.assert_not_called(); install.assert_not_called(); command.assert_not_called()
        android.call.assert_not_called()
        android.shell.assert_called_once_with('getprop','ro.product.cpu.abi')

    def test_interaction_cli_allows_alone_or_camera_steering_without_installing(self):
        for extra in ([],['--camera-steering'],['--camera-steering','--camera-probe']):
            with patch.object(nonroot,'package',return_value=({'outputs':[]},ROOT)) as package, \
                 patch.object(nonroot,'probe') as install, patch.object(nonroot,'command') as command, \
                 contextlib.redirect_stdout(io.StringIO()):
                nonroot.main(['--apk','game.apkm','--interaction-probe','--skip-build']+extra)
            self.assertTrue(package.call_args.kwargs['interaction_probe'])
            self.assertEqual(package.call_args.kwargs.get('camera_steering',False),bool(extra))
            install.assert_not_called(); command.assert_not_called()

    def test_interaction_rejects_incompatible_cli_before_build(self):
        with patch.object(nonroot,'package') as package, patch.object(nonroot,'command') as command, \
             contextlib.redirect_stderr(io.StringIO()):
            for flag in ['--panel-only','--il2cpp-survey','--scene-probe','--scene-observe','--camera-probe','--camera-sample']:
                with self.assertRaises(SystemExit):
                    nonroot.main(['--apk','game.apk','--interaction-probe',flag])
            for args in (['--apk','game.apk','--runtime-abi','x86_64'], ['--build-probe','--abi','x86_64']):
                with self.assertRaisesRegex(ValueError,'--interaction-probe requires'):
                    nonroot.main(args+['--interaction-probe'])
        package.assert_not_called(); command.assert_not_called(); self.cleanup.assert_not_called()

    def test_interaction_api_rejects_incompatible_modes_before_outputs(self):
        with patch.object(nonroot,'validated_build') as validated, patch.object(nonroot,'input_apks') as inputs:
            for kwargs in ({'panel_only':True}, {'game_runtime_abi':'x86_64'}, {'preview':True,'probe_abi':'x86_64'}, {'preview':True}):
                with self.assertRaisesRegex(ValueError,'--interaction-probe requires'):
                    nonroot.package(Path('unused.apk'),interaction_probe=True,**kwargs)
            for flag in ['il2cpp_survey','scene_probe','scene_observe','camera_probe','camera_sample']:
                with self.assertRaisesRegex(ValueError,'cannot be combined'):
                    nonroot.package(Path('unused.apk'),interaction_probe=True,**{flag:True})
        validated.assert_not_called(); inputs.assert_not_called()

    def test_interaction_x64_probe_stops_after_readonly_architecture_check(self):
        android=Mock(); android.shell.return_value='x86_64\n'
        monitor=Mock(); monitor.select_device.return_value=android
        with patch.dict(sys.modules,{'monitor':monitor}), patch.object(nonroot,'package') as package, \
             patch.object(nonroot,'probe') as install, patch.object(nonroot,'command') as command:
            with self.assertRaisesRegex(ValueError,'--interaction-probe requires'):
                nonroot.main(['--probe','--interaction-probe'])
        package.assert_not_called(); install.assert_not_called(); command.assert_not_called()
        android.call.assert_not_called()
        android.shell.assert_called_once_with('getprop','ro.product.cpu.abi')

    def test_scene_observation_cli_is_opt_in_and_accepts_explicit_probe(self):
        for extra in ([],['--scene-probe']):
            with patch.object(nonroot,'package',return_value=({'outputs':[]},ROOT)) as package, \
                 patch.object(nonroot,'probe') as install, patch.object(nonroot,'command') as command, \
                 contextlib.redirect_stdout(io.StringIO()):
                nonroot.main(['--apk','game.apkm','--scene-observe','--skip-build']+extra)
            self.assertTrue(package.call_args.kwargs['scene_observe'])
            self.assertEqual(package.call_args.kwargs.get('scene_probe',False),bool(extra))
            install.assert_not_called(); command.assert_not_called()

    def test_scene_observation_rejects_invalid_cli_before_build(self):
        with patch.object(nonroot,'package') as package, patch.object(nonroot,'command') as command, \
             contextlib.redirect_stderr(io.StringIO()):
            for flag in ['--panel-only','--il2cpp-survey','--camera-probe','--camera-sample','--camera-steering']:
                with self.assertRaises(SystemExit):
                    nonroot.main(['--apk','game.apk','--scene-observe',flag])
            for args in (['--apk','game.apk','--runtime-abi','x86_64'], ['--build-probe','--abi','x86_64']):
                with self.assertRaisesRegex(ValueError,'--scene-probe requires'):
                    nonroot.main(args+['--scene-observe'])
        package.assert_not_called(); command.assert_not_called(); self.cleanup.assert_not_called()

    def test_scene_observation_api_rejects_incompatible_modes_before_outputs(self):
        with patch.object(nonroot,'validated_build') as validated, patch.object(nonroot,'input_apks') as inputs:
            for kwargs in ({'panel_only':True}, {'game_runtime_abi':'x86_64'}, {'preview':True,'probe_abi':'x86_64'}, {'preview':True}):
                with self.assertRaisesRegex(ValueError,'--scene-probe requires'):
                    nonroot.package(Path('unused.apk'),scene_observe=True,**kwargs)
            for flag in ['il2cpp_survey','camera_probe','camera_sample','camera_steering']:
                with self.assertRaisesRegex(ValueError,'cannot be combined'):
                    nonroot.package(Path('unused.apk'),scene_observe=True,**{flag:True})
        validated.assert_not_called(); inputs.assert_not_called()

    def test_scene_opt_in_is_forwarded_without_installing(self):
        with patch.object(nonroot,'package',return_value=({'outputs':[]},ROOT)) as package, \
             patch.object(nonroot,'probe') as install, patch.object(nonroot,'command') as command, \
             contextlib.redirect_stdout(io.StringIO()):
            nonroot.main(['--apk','game.apkm','--scene-probe','--skip-build'])
        self.assertTrue(package.call_args.kwargs['scene_probe'])
        self.assertNotIn('scene_observe',package.call_args.kwargs)
        self.assertNotIn('camera_probe',package.call_args.kwargs)
        install.assert_not_called(); command.assert_not_called()

    def test_scene_rejects_other_diagnostics_and_invalid_cli_before_build(self):
        with patch.object(nonroot,'package') as package, patch.object(nonroot,'command') as command, \
             contextlib.redirect_stderr(io.StringIO()):
            for flag in ['--panel-only','--il2cpp-survey','--camera-probe','--camera-sample','--camera-steering']:
                with self.assertRaises(SystemExit):
                    nonroot.main(['--apk','game.apk','--scene-probe',flag])
            for args in (['--apk','game.apk','--runtime-abi','x86_64'], ['--build-probe','--abi','x86_64']):
                with self.assertRaisesRegex(ValueError,'--scene-probe requires'):
                    nonroot.main(args+['--scene-probe'])
        package.assert_not_called(); command.assert_not_called()

    def test_scene_api_rejects_incompatible_modes_before_outputs(self):
        with patch.object(nonroot,'validated_build') as validated, patch.object(nonroot,'input_apks') as inputs:
            for kwargs in ({'panel_only':True}, {'game_runtime_abi':'x86_64'}, {'preview':True,'probe_abi':'x86_64'}, {'preview':True}):
                with self.assertRaisesRegex(ValueError,'--scene-probe requires'):
                    nonroot.package(Path('unused.apk'),scene_probe=True,**kwargs)
            for flag in ['il2cpp_survey','camera_probe','camera_sample','camera_steering']:
                with self.assertRaisesRegex(ValueError,'cannot be combined'):
                    nonroot.package(Path('unused.apk'),scene_probe=True,**{flag:True})
        validated.assert_not_called(); inputs.assert_not_called()

    def test_scene_x64_probe_stops_after_readonly_architecture_check(self):
        android=Mock(); android.shell.return_value='x86_64\n'
        monitor=Mock(); monitor.select_device.return_value=android
        with patch.dict(sys.modules,{'monitor':monitor}), patch.object(nonroot,'package') as package, \
             patch.object(nonroot,'probe') as install, patch.object(nonroot,'command') as command:
            with self.assertRaisesRegex(ValueError,'--scene-probe requires'):
                nonroot.main(['--probe','--scene-probe'])
        package.assert_not_called(); install.assert_not_called(); command.assert_not_called()
        android.call.assert_not_called()
        android.shell.assert_called_once_with('getprop','ro.product.cpu.abi')

    def test_camera_steering_cli_is_opt_in_and_accepts_explicit_probe(self):
        for extra in ([],['--camera-probe']):
            with patch.object(nonroot,'package',return_value=({'outputs':[]},ROOT)) as package, \
                 patch.object(nonroot,'probe') as install, patch.object(nonroot,'command') as command, \
                 contextlib.redirect_stdout(io.StringIO()):
                nonroot.main(['--apk','game.apkm','--camera-steering','--skip-build']+extra)
            self.assertTrue(package.call_args.kwargs['camera_steering'])
            self.assertNotIn('camera_sample',package.call_args.kwargs)
            self.assertEqual(package.call_args.kwargs.get('camera_probe',False),bool(extra))
            install.assert_not_called(); command.assert_not_called()

    def test_camera_steering_rejects_incompatible_cli_before_build(self):
        with patch.object(nonroot,'package') as package, patch.object(nonroot,'command') as command, \
             contextlib.redirect_stderr(io.StringIO()):
            for extra in (['--panel-only'],['--il2cpp-survey'],['--camera-sample']):
                with self.assertRaises(SystemExit):
                    nonroot.main(['--apk','game.apk','--camera-steering']+extra)
            for args in (['--apk','game.apk','--runtime-abi','x86_64'], ['--build-probe','--abi','x86_64']):
                with self.assertRaisesRegex(ValueError,'--camera-probe requires'):
                    nonroot.main(args+['--camera-steering'])
        package.assert_not_called(); command.assert_not_called()

    def test_camera_steering_api_rejects_incompatible_modes_before_outputs(self):
        with patch.object(nonroot,'validated_build') as validated, patch.object(nonroot,'input_apks') as inputs:
            for kwargs in ({'panel_only':True}, {'game_runtime_abi':'x86_64'}, {'preview':True,'probe_abi':'x86_64'}, {'preview':True}):
                with self.assertRaisesRegex(ValueError,'--camera-probe requires'):
                    nonroot.package(Path('unused.apk'),camera_steering=True,**kwargs)
            for kwargs in ({'il2cpp_survey':True},{'camera_sample':True}):
                with self.assertRaisesRegex(ValueError,'cannot be combined'):
                    nonroot.package(Path('unused.apk'),camera_steering=True,**kwargs)
        validated.assert_not_called(); inputs.assert_not_called()

    def test_camera_sampling_cli_is_opt_in_and_accepts_explicit_probe(self):
        for extra in ([],['--camera-probe']):
            with patch.object(nonroot,'package',return_value=({'outputs':[]},ROOT)) as package, \
                 patch.object(nonroot,'probe') as install, patch.object(nonroot,'command') as command, \
                 contextlib.redirect_stdout(io.StringIO()):
                nonroot.main(['--apk','game.apkm','--camera-sample','--skip-build']+extra)
            self.assertTrue(package.call_args.kwargs['camera_sample'])
            self.assertEqual(package.call_args.kwargs.get('camera_probe',False),bool(extra))
            install.assert_not_called(); command.assert_not_called()

    def test_camera_sampling_rejects_incompatible_cli_before_build(self):
        with patch.object(nonroot,'package') as package, patch.object(nonroot,'command') as command, \
             contextlib.redirect_stderr(io.StringIO()):
            for extra in (['--panel-only'],['--il2cpp-survey']):
                with self.assertRaises(SystemExit):
                    nonroot.main(['--apk','game.apk','--camera-sample']+extra)
            for args in (['--apk','game.apk','--runtime-abi','x86_64'], ['--build-probe','--abi','x86_64']):
                with self.assertRaisesRegex(ValueError,'--camera-probe requires'):
                    nonroot.main(args+['--camera-sample'])
        package.assert_not_called(); command.assert_not_called()

    def test_camera_sampling_api_rejects_incompatible_modes_before_outputs(self):
        with patch.object(nonroot,'validated_build') as validated, patch.object(nonroot,'input_apks') as inputs:
            for kwargs in ({'panel_only':True}, {'game_runtime_abi':'x86_64'}, {'preview':True,'probe_abi':'x86_64'}, {'preview':True}):
                with self.assertRaisesRegex(ValueError,'--camera-probe requires'):
                    nonroot.package(Path('unused.apk'),camera_sample=True,**kwargs)
            with self.assertRaisesRegex(ValueError,'cannot be combined'):
                nonroot.package(Path('unused.apk'),camera_sample=True,il2cpp_survey=True)
        validated.assert_not_called(); inputs.assert_not_called()

    def test_camera_opt_in_is_forwarded_without_installing(self):
        with patch.object(nonroot,'package',return_value=({'outputs':[]},ROOT)) as package, \
             patch.object(nonroot,'probe') as install, patch.object(nonroot,'command') as command, \
             contextlib.redirect_stdout(io.StringIO()):
            nonroot.main(['--apk','game.apkm','--camera-probe','--skip-build'])
        self.assertTrue(package.call_args.kwargs['camera_probe'])
        self.assertNotIn('camera_sample',package.call_args.kwargs)
        self.assertNotIn('camera_steering',package.call_args.kwargs)
        self.assertNotIn('il2cpp_survey',package.call_args.kwargs)
        install.assert_not_called(); command.assert_not_called()

    def test_camera_rejects_incompatible_cli_before_build(self):
        with patch.object(nonroot,'package') as package, patch.object(nonroot,'command') as command, \
             contextlib.redirect_stderr(io.StringIO()):
            for extra in (['--panel-only'],['--il2cpp-survey']):
                with self.assertRaises(SystemExit):
                    nonroot.main(['--apk','game.apk','--camera-probe']+extra)
            for args in (['--apk','game.apk','--runtime-abi','x86_64'], ['--build-probe','--abi','x86_64']):
                with self.assertRaisesRegex(ValueError,'--camera-probe requires'):
                    nonroot.main(args+['--camera-probe'])
        package.assert_not_called(); command.assert_not_called()

    def test_camera_direct_api_rejects_before_build_or_outputs(self):
        with patch.object(nonroot,'validated_build') as validated, patch.object(nonroot,'input_apks') as inputs:
            for kwargs in ({'panel_only':True}, {'game_runtime_abi':'x86_64'}, {'preview':True,'probe_abi':'x86_64'}, {'preview':True}):
                with self.assertRaisesRegex(ValueError,'--camera-probe requires'):
                    nonroot.package(Path('unused.apk'),camera_probe=True,**kwargs)
            with self.assertRaisesRegex(ValueError,'cannot be combined'):
                nonroot.package(Path('unused.apk'),camera_probe=True,il2cpp_survey=True)
        validated.assert_not_called(); inputs.assert_not_called()

    def test_camera_x64_probe_stops_after_readonly_architecture_check(self):
        android=Mock(); android.shell.return_value='x86_64\n'
        monitor=Mock(); monitor.select_device.return_value=android
        with patch.dict(sys.modules,{'monitor':monitor}), patch.object(nonroot,'package') as package, \
             patch.object(nonroot,'probe') as install, patch.object(nonroot,'command') as command:
            with self.assertRaisesRegex(ValueError,'--camera-probe requires'):
                nonroot.main(['--probe','--camera-probe'])
        package.assert_not_called(); install.assert_not_called(); command.assert_not_called()
        android.call.assert_not_called()
        android.shell.assert_called_once_with('getprop','ro.product.cpu.abi')

    def test_survey_opt_in_is_forwarded_without_installing(self):
        with patch.object(nonroot,'package',return_value=({'outputs':[]},ROOT)) as package, \
             patch.object(nonroot,'probe') as install, patch.object(nonroot,'command') as command, \
             contextlib.redirect_stdout(io.StringIO()):
            nonroot.main(['--apk','game.apkm','--il2cpp-survey','--skip-build'])
        self.assertTrue(package.call_args.kwargs['il2cpp_survey'])
        install.assert_not_called(); command.assert_not_called()

    def test_survey_rejects_incompatible_cli_before_build(self):
        with patch.object(nonroot,'package') as package, patch.object(nonroot,'command') as command, \
             contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit):
                nonroot.main(['--apk','game.apk','--panel-only','--il2cpp-survey'])
            for args in (['--apk','game.apk','--runtime-abi','x86_64'], ['--build-probe','--abi','x86_64']):
                with self.assertRaisesRegex(ValueError,'native arm64-v8a'):
                    nonroot.main(args+['--il2cpp-survey'])
        package.assert_not_called(); command.assert_not_called()

    def test_survey_direct_api_rejects_before_build_or_outputs(self):
        with patch.object(nonroot,'validated_build') as validated, patch.object(nonroot,'input_apks') as inputs:
            for kwargs in ({'panel_only':True}, {'game_runtime_abi':'x86_64'}, {'preview':True,'probe_abi':'x86_64'}, {'preview':True}):
                with self.assertRaisesRegex(ValueError,'native arm64-v8a'):
                    nonroot.package(Path('unused.apk'),il2cpp_survey=True,**kwargs)
        validated.assert_not_called(); inputs.assert_not_called()

    def test_survey_x64_probe_stops_after_readonly_architecture_check(self):
        android=Mock(); android.shell.return_value='x86_64\n'
        monitor=Mock(); monitor.select_device.return_value=android
        with patch.dict(sys.modules,{'monitor':monitor}), patch.object(nonroot,'package') as package, \
             patch.object(nonroot,'probe') as install, patch.object(nonroot,'command') as command:
            with self.assertRaisesRegex(ValueError,'native arm64-v8a'):
                nonroot.main(['--probe','--il2cpp-survey'])
        package.assert_not_called(); install.assert_not_called(); command.assert_not_called()
        android.call.assert_not_called()
        android.shell.assert_called_once_with('getprop','ro.product.cpu.abi')

    def test_diagnostic_assets_mode_and_receipt_are_strictly_opt_in(self):
        # Exercise actual ZIP rewriting and receipt hashes; SDK commands are
        # fixture copies so this neither builds, installs nor accesses a device.
        with tempfile.TemporaryDirectory(dir=ROOT/'build') as temp:
            work=Path(temp); (work/'diagnostics').mkdir(); (work/'vendor').mkdir()
            source=work/'game.apk'; dex=work/'module.dex'; dex.write_bytes(b'dex\nfixture')
            with zipfile.ZipFile(source,'w') as archive:
                archive.writestr('AndroidManifest.xml',b'manifest')
                archive.writestr('classes.dex',b'original-game-dex')
            survey=work/'diagnostics/il2cpp-survey.js'; survey.write_bytes(b'// diagnostic fixture\n')
            camera=work/'diagnostics/camera-probe.js'; camera.write_bytes(b'// camera diagnostic fixture\n')
            scene=work/'diagnostics/scene-probe.js'; scene.write_bytes(b'// scene metadata fixture\n')
            interaction=work/'diagnostics/interaction-probe.js'; interaction.write_bytes(b'// interaction metadata fixture\n')
            summary=work/'diagnostics/quick-summary.js'; summary.write_bytes(b'// summary feature fixture\n')
            presentation=work/'diagnostics/catch-presentation-bridge.js'; presentation.write_bytes(b'// presentation bridge fixture\n')
            close=work/'diagnostics/quick-close.js'; close.write_bytes(b'// summary close fixture\n')
            ball=work/'diagnostics/quick-ball.js'; ball.write_bytes(b'// ball feature fixture\n')
            agent=work/'embedded-agent.js'; agent.write_bytes(b'// embedded fixture\n')
            (work/'runtime').mkdir()
            shutil.copyfile(ROOT/'runtime/bundles.json', work/'runtime/bundles.json')
            (work/'runtime/location.js').write_bytes(b'// location fixture\n')
            (work/'runtime/translated.js').write_bytes(b'// translated fixture\n')
            for part in {part for parts in json.loads((ROOT/'runtime/bundles.json').read_text()).values() for part in parts}:
                if part.startswith('runtime/') and not (work/part).exists():
                    fixture=work/part; fixture.parent.mkdir(parents=True,exist_ok=True)
                    fixture.write_bytes(('// fixture '+part+'\n').encode())
            bridge=work/'bridge.js'; bridge.write_bytes(b'// bridge fixture\n')
            gadget=work/'gadget.so'; gadget.write_bytes(b'gadget fixture')
            for name in ['NOTICE.txt','COPYING.frida-tools','bridge-provenance.json']:
                (work/'vendor'/name).write_text('fixture',encoding='utf-8')
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
                 patch.object(runtime_assets,'gadget',return_value=(gadget,{'abi':'arm64-v8a'})), \
                 patch.object(runtime_assets,'java_bridge',return_value=bridge), \
                 patch.object(axml,'inspect_manifest',return_value=info), \
                 patch.object(axml,'patch_manifest',side_effect=lambda data:data), \
                 contextlib.redirect_stdout(io.StringIO()):
                cases=[{}]+[{name:True} for name in ('il2cpp_survey','camera_probe','camera_sample','camera_steering','scene_probe','scene_observe','interaction_probe','quick_summary','quick_ball')]
                cases.append({'camera_steering':True,'interaction_probe':True})
                cases.extend([{'quick_close':True},{'quick_close':True,'quick_ball':True,'camera_steering':True,'interaction_probe':True}])
                cases.extend([{'quick_ball':True,'quick_summary':True,'camera_steering':True,'interaction_probe':True},{'quick_ball':True,'camera_steering':True},{'quick_ball':True,'interaction_probe':True},{'quick_summary':True,'camera_steering':True},{'quick_summary':True,'interaction_probe':True},
                    {'quick_summary':True,'camera_steering':True,'interaction_probe':True}])
                for options in cases:
                    camera_enabled=any(options.get(name,False) for name in ('camera_probe','camera_sample','camera_steering'))
                    scene_enabled=any(options.get(name,False) for name in ('scene_probe','scene_observe'))
                    report,folder=nonroot.package(source,**options)
                    self.assertEqual(json.loads((folder/'report.json').read_text()),report)
                    with zipfile.ZipFile(report['outputs'][0]['file']) as archive:
                        mode=json.loads(archive.read('assets/pgrazor/mode.json'))
                        bundled=archive.read('assets/pgrazor/embedded-agent.js')
                        self.assertEqual(bundled,b'// location fixture\n\n;\n// translated fixture\n\n;\n// embedded fixture\n')
                        self.assertEqual(report['agent_sha256'],hashlib.sha256(bundled).hexdigest())
                        for name,receipt in report['script_bundles'].items():
                            self.assertEqual(receipt['sha256'],hashlib.sha256(archive.read('assets/pgrazor/'+name)).hexdigest())
                            self.assertEqual([part['path'] for part in receipt['sources']],
                                             json.loads((work/'runtime/bundles.json').read_text())[name])
                            for part in receipt['sources']:
                                self.assertEqual(part['sha256'],runtime_assets.sha256(work/part['path']))
                        self.assertEqual(mode['preferredAbi'],'')
                        self.assertEqual(mode['runtimeAbis'],['arm64-v8a','x86_64'])
                        for abi in mode['runtimeAbis']:
                            self.assertEqual(archive.read('assets/pgrazor/'+abi+'/libfrida-gadget.so'),b'gadget fixture')
                        self.assertEqual(mode.get('il2cppSurvey',False),options.get('il2cpp_survey',False))
                        self.assertEqual(mode.get('cameraProbe',False),camera_enabled)
                        self.assertEqual(mode.get('cameraSample',False),options.get('camera_sample',False))
                        self.assertEqual(mode.get('cameraSteering',False),options.get('camera_steering',False))
                        self.assertEqual(mode.get('sceneProbe',False),scene_enabled)
                        self.assertEqual(mode.get('sceneObserve',False),options.get('scene_observe',False))
                        self.assertEqual(mode.get('interactionProbe',False),options.get('interaction_probe',False))
                        self.assertEqual(mode.get('quickSummary',False),bool(options.get('quick_summary') or options.get('quick_close')))
                        self.assertEqual(mode.get('quickClose',False),options.get('quick_close',False))
                        self.assertEqual(mode.get('quickBall',False),options.get('quick_ball',False))
                        self.assertEqual('assets/pgrazor/il2cpp-survey.js' in archive.namelist(),options.get('il2cpp_survey',False))
                        self.assertEqual('assets/pgrazor/camera-probe.js' in archive.namelist(),camera_enabled)
                        self.assertEqual('assets/pgrazor/scene-probe.js' in archive.namelist(),scene_enabled)
                        self.assertEqual('assets/pgrazor/interaction-probe.js' in archive.namelist(),options.get('interaction_probe',False))
                        self.assertEqual('assets/pgrazor/quick-summary.js' in archive.namelist(),bool(options.get('quick_summary') or options.get('quick_close')))
                        self.assertEqual('assets/pgrazor/quick-close.js' in archive.namelist(),options.get('quick_close',False))
                        self.assertEqual('assets/pgrazor/quick-ball.js' in archive.namelist(),options.get('quick_ball',False))
                        self.assertEqual(archive.read('classes.dex'),b'original-game-dex')
                        if options:
                            expected_diagnostics={}
                            if options.get('il2cpp_survey'): expected_diagnostics['il2cpp_survey']=survey
                            if camera_enabled: expected_diagnostics['camera_probe']=camera
                            if scene_enabled: expected_diagnostics['scene_probe']=scene
                            if options.get('interaction_probe'): expected_diagnostics['interaction_probe']=interaction
                            if options.get('quick_summary'): expected_diagnostics['quick_summary']=summary
                            if options.get('quick_close'):
                                expected_diagnostics['quick_summary']=close
                                self.assertEqual(report['diagnostics']['quick_summary']['behavior'],'normal-summary-and-details-dismiss')
                            if options.get('quick_ball'): expected_diagnostics['quick_ball']=ball
                            self.assertEqual(set(report['diagnostics']),set(expected_diagnostics))
                            for key,expected in expected_diagnostics.items():
                                diagnostic=report['diagnostics'][key]
                                if key=='camera_probe':
                                    self.assertEqual(diagnostic['sampling'],options.get('camera_sample',False))
                                    self.assertEqual(diagnostic['steering'],options.get('camera_steering',False))
                                if key=='scene_probe': self.assertEqual(diagnostic['observe_runtime'],options.get('scene_observe',False))
                                if key=='interaction_probe': self.assertEqual(diagnostic['startup_mode'],'metadata-only')
                                if key in ('quick_summary','quick_ball'):
                                    self.assertEqual(diagnostic['startup_mode'],'metadata-validation')
                                    self.assertIs(diagnostic['acceleration_default'],False)
                                expected_bytes=expected.read_bytes()
                                if key=='quick_summary' and options.get('quick_close'):
                                    expected_bytes=b'\n;\n'.join((work/part).read_bytes() for part in json.loads((work/'runtime/bundles.json').read_text())['quick-close.js'])
                                if key=='quick_ball':
                                    expected_bytes=b'\n;\n'.join((work/part).read_bytes() for part in json.loads((work/'runtime/bundles.json').read_text())['quick-ball.js'])
                                self.assertEqual(diagnostic['sha256'],hashlib.sha256(expected_bytes).hexdigest())
                                self.assertEqual(diagnostic['source'],str(expected))
                                self.assertEqual(archive.read(diagnostic['asset']),expected_bytes)
                        else:
                            self.assertNotIn('diagnostics',report)
            self.assertEqual(runtime_assets.sha256(source),original_hash)

    def test_emulator_runtime_is_explicit_and_never_installs(self):
        with patch.object(nonroot,'package',return_value=({'outputs':[]},ROOT)) as package, patch.object(nonroot,'probe') as install, contextlib.redirect_stdout(io.StringIO()):
            nonroot.main(['--apk','game.apkm','--runtime-abi','x86_64','--skip-build'])
        self.assertEqual(package.call_args.kwargs['game_runtime_abi'],'x86_64')
        install.assert_not_called()
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            nonroot.main(['--probe','--runtime-abi','x86_64'])

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
            (work/'build/build-state.json').write_text(json.dumps({'sources':{}}))
            with self.assertRaisesRegex(RuntimeError,'stale'): nonroot.validated_build()

    def test_game_install_is_never_a_probe(self):
        with self.assertRaisesRegex(RuntimeError,'restricted'):
            nonroot.probe({'package':'com.nianticlabs.pokemongo','outputs':[{}]},ROOT,None)

    def test_phone_build_does_not_install_or_capture(self):
        report={'outputs':[{'file':'phone-preview.apk'}]}
        with patch.object(nonroot,'package',return_value=(report,ROOT)) as package, \
             patch.object(nonroot,'probe') as install, patch.object(nonroot,'command') as command, \
             contextlib.redirect_stdout(io.StringIO()):
            nonroot.main(['--build-probe','--abi','arm64-v8a','--skip-build'])
        package.assert_called_once_with(ROOT/'build/pgrazor-preview.apk',panel_only=False,download=False,preview=True,probe_abi='arm64-v8a')
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
            nonroot.main(['--probe','--serial','PHONE123','--skip-build'])
        monitor.select_device.assert_called_once_with(monitor.find_adb.return_value,'PHONE123')
        self.assertEqual(package.call_args.kwargs['probe_abi'],'arm64-v8a')
        probe.assert_called_once_with(report,ROOT,android,0)

    def test_missing_selected_phone_stops_before_packaging(self):
        monitor=Mock(); monitor.select_device.side_effect=RuntimeError('not online')
        with patch.dict(sys.modules,{'monitor':monitor}), patch.object(nonroot,'package') as package, patch.object(nonroot,'command') as command:
            with self.assertRaisesRegex(RuntimeError,'PHONE123'):
                nonroot.main(['--probe','--serial','PHONE123'])
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
            nonroot.main(['--probe','--serial','PHONE123','--skip-build'])
        self.assertEqual([call[0] for call in calls.mock_calls],['package','cleanup','probe'])

    def test_failed_package_does_not_clean(self):
        with patch.object(nonroot,'package',side_effect=RuntimeError('build failed')), \
             patch.object(nonroot,'cleanup_generated_builds') as cleanup:
            with self.assertRaisesRegex(RuntimeError,'build failed'):
                nonroot.main(['--apk','game.apk','--skip-build'])
        cleanup.assert_not_called()


if __name__=='__main__': unittest.main()
