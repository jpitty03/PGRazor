"""Portable prerequisite discovery and refusal to overwrite pinned dependencies."""
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'pgrazor'))
import art_build

spec = importlib.util.spec_from_file_location('prepare_native', ROOT / 'tools/prepare_native.py')
prepare_native = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prepare_native)


class NativePrerequisiteTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='pgrazor-native-prerequisites-')
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name).resolve()
        self.root = self.base / 'checkout/pgrazor'
        self.root.mkdir(parents=True)

    def ndk(self, path, revision):
        path.mkdir(parents=True)
        (path / 'source.properties').write_text('Pkg.Revision = ' + revision + '\n')
        toolchain = path / 'build/cmake/android.toolchain.cmake'
        toolchain.parent.mkdir(parents=True)
        toolchain.write_text('# fixture\n')
        return path.resolve()

    def repository(self):
        path = self.base / 'dependency'
        path.mkdir()
        art_build.run(['git', 'init', path])
        (path / 'input.cpp').write_text('original\n')
        art_build.run(['git', '-C', path, 'add', 'input.cpp'])
        art_build.run(['git', '-C', path, '-c', 'user.name=Fixture',
            '-c', 'user.email=fixture@example.invalid', '-c', 'commit.gpgsign=false',
            'commit', '-m', 'fixture'])
        return path, art_build.run(['git', '-C', path, 'rev-parse', 'HEAD'])

    def test_sdk_discovers_art_without_confusing_gameplay_ndk(self):
        gameplay = self.ndk(self.base / 'gameplay', '27.3.13750724')
        sdk = self.base / 'sdk'
        art = self.ndk(sdk / 'ndk' / art_build.NDK_VERSION, art_build.NDK_VERSION)
        found, revision = art_build.resolve_art_ndk(self.root,
            {'ANDROID_NDK_HOME': str(gameplay), 'ANDROID_SDK_ROOT': str(sdk)})
        self.assertEqual((found, revision), (art, art_build.NDK_VERSION))

    def test_explicit_art_override_is_strict(self):
        wrong = self.ndk(self.base / 'wrong', '27.3.13750724')
        sdk = self.base / 'sdk'
        self.ndk(sdk / 'ndk' / art_build.NDK_VERSION, art_build.NDK_VERSION)
        with self.assertRaisesRegex(RuntimeError, 'Pinned NDK'):
            art_build.resolve_art_ndk(self.root, {'PGRAZOR_ART_NDK': str(wrong), 'ANDROID_SDK_ROOT': str(sdk)})

    def test_android_home_and_explicit_art_selection(self):
        sdk = self.base / 'sdk'
        art = self.ndk(sdk / 'ndk' / art_build.NDK_VERSION, art_build.NDK_VERSION)
        self.assertEqual(art_build.resolve_art_ndk(self.root, {'ANDROID_HOME': str(sdk)})[0], art)
        self.assertEqual(art_build.resolve_art_ndk(self.root, {'PGRAZOR_ART_NDK': str(art)})[0], art)

    def test_existing_workspace_artifact_fallback(self):
        art = self.ndk(self.root.parent / '.artifacts/native-migration/android-ndk-r29', art_build.NDK_VERSION)
        self.assertEqual(art_build.resolve_art_ndk(self.root, {})[0], art)

    def test_revision_without_toolchain_is_rejected(self):
        path = self.base / 'incomplete'
        path.mkdir()
        (path / 'source.properties').write_text('Pkg.Revision = ' + art_build.NDK_VERSION)
        with self.assertRaisesRegex(RuntimeError, 'toolchain missing'):
            art_build.resolve_art_ndk(self.root, {'PGRAZOR_ART_NDK': str(path)})

    def test_dependency_cache_precedence_and_legacy_compatibility(self):
        portable = self.root / 'build/native/deps'
        self.assertEqual(art_build.dependency_paths(self.root, environ={})['lsplant'].parent, portable)
        legacy = self.root.parent / '.artifacts/native-migration'
        (legacy / 'LSPlant-runtime').mkdir(parents=True)
        self.assertEqual(art_build.dependency_paths(self.root, environ={})['lsplant'].parent, legacy)
        portable.mkdir(parents=True)
        self.assertEqual(art_build.dependency_paths(self.root, environ={})['lsplant'].parent, portable)
        selected = self.base / 'cache'
        self.assertEqual(art_build.dependency_paths(self.root, environ={'PGRAZOR_NATIVE_DEPS': str(selected)})['lsplant'].parent, selected)
        explicit = self.base / 'explicit'
        self.assertEqual(art_build.dependency_paths(self.root, explicit,
            {'PGRAZOR_NATIVE_DEPS': str(selected)})['lsplant'].parent, explicit)

    def test_modified_dependency_refused_without_reset(self):
        path, commit = self.repository()
        self.assertEqual(art_build.verified_git(path, commit)['commit'], commit)
        (path / 'input.cpp').write_text('local modification\n')
        with self.assertRaisesRegex(RuntimeError, 'modified'):
            prepare_native.clone_pinned(path, 'https://example.invalid/repo', commit, True)
        self.assertEqual((path / 'input.cpp').read_text(), 'local modification\n')
        self.assertEqual(art_build.run(['git', '-C', path, 'rev-parse', 'HEAD']), commit)

    def test_untracked_compiler_input_rejected(self):
        path, commit = self.repository()
        (path / 'injected.cpp').write_text('unexpected compiler input\n')
        with self.assertRaisesRegex(RuntimeError, 'modified'):
            art_build.verified_git(path, commit)

    def test_wrong_pin_and_parent_repository_rejected(self):
        path, commit = self.repository()
        with self.assertRaisesRegex(RuntimeError, 'HEAD mismatch'):
            art_build.verified_git(path, '0' * 40)
        child = path / 'not-a-submodule'
        child.mkdir()
        with self.assertRaisesRegex(RuntimeError, 'own repository'):
            art_build.verified_git(child, commit)

    def test_missing_dependency_never_downloads_implicitly(self):
        target = self.base / 'missing'
        with patch.object(art_build, 'run') as run, self.assertRaisesRegex(RuntimeError, '--download'):
            prepare_native.clone_pinned(target, art_build.LSPLANT_URL, art_build.LSPLANT, False)
        run.assert_not_called()
        self.assertFalse(target.exists())

    def test_new_clone_checks_out_exact_pin_not_branch_tip(self):
        source, pinned = self.repository()
        (source / 'input.cpp').write_text('newer branch tip\n')
        art_build.run(['git', '-C', source, 'add', 'input.cpp'])
        art_build.run(['git', '-C', source, '-c', 'user.name=Fixture',
            '-c', 'user.email=fixture@example.invalid', '-c', 'commit.gpgsign=false',
            'commit', '-m', 'newer'])
        target = self.base / 'cloned'
        # Local Git transport exercises the real clone/checkout sequence offline.
        result = prepare_native.clone_pinned(target, str(source), pinned, True)
        self.assertEqual(result['commit'], pinned)
        self.assertEqual((target / 'input.cpp').read_text(), 'original\n')

    def test_gitlink_is_verified_independently(self):
        path, commit = self.repository()
        art_build.run(['git', '-C', path, 'update-index', '--add', '--cacheinfo', '160000,' + commit + ',child'])
        art_build.run(['git', '-C', path, '-c', 'user.name=Fixture',
            '-c', 'user.email=fixture@example.invalid', '-c', 'commit.gpgsign=false',
            'commit', '-m', 'gitlink'])
        art_build.gitlink(path, 'child', commit)
        with self.assertRaisesRegex(RuntimeError, 'gitlink mismatch'):
            art_build.gitlink(path, 'child', '0' * 40)


if __name__ == '__main__':
    unittest.main()
