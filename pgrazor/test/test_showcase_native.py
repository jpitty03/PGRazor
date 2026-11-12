"""Host-compile and run the pure C++ Showcase keeper policy test.

Compiles pgrazor/native/showcase_policy_test.cpp against the real
showcase_policy.h header (no stub implementation) into a throwaway temp
directory and runs it, expecting the binary to print a "PASS" line.

On Windows the MSVC x64 toolchain is located with vswhere and configured with
vcvars64.bat; elsewhere any host c++/g++/clang++ is used. The test skips when no
host C++ toolchain is available (e.g. Android NDK-only CI).
"""
import os
import platform
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]  # pgrazor/
TEST_SOURCE = ROOT / "native" / "showcase_policy_test.cpp"
SCORE_SOURCES = [ROOT / "native" / "showcase_score.cpp", ROOT / "native" / "showcase_score_test.cpp", ROOT / "native" / "showcase_tags_test.cpp"]
SCORE_SOURCES.append(ROOT / "native" / "showcase_bindings_test.cpp")
SCORE_SOURCES.append(ROOT / "native" / "showcase_retention_test.cpp")
HEADER = ROOT / "native" / "showcase_policy.h"


def _vcvars_env():
    """Environment dict with the MSVC x64 toolchain on PATH, or None."""
    program_files = os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")
    vswhere = Path(program_files) / "Microsoft Visual Studio/Installer/vswhere.exe"
    if not vswhere.is_file():
        return None
    located = subprocess.run(
        [str(vswhere), "-latest", "-products", "*",
         "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
         "-property", "installationPath"],
        capture_output=True, text=True)
    paths = located.stdout.strip().splitlines()
    if located.returncode != 0 or not paths:
        return None
    vcvars = Path(paths[0]) / "VC/Auxiliary/Build/vcvars64.bat"
    if not vcvars.is_file():
        return None
    # Pass the command line intact: list2cmdline's escaped quotes are not cmd.exe
    # quoting and break vcvars paths containing spaces. This is a discovered local
    # toolchain path, never user-supplied command text.
    dumped = subprocess.run(
        'cmd /d /c call "{}" >nul && set'.format(vcvars),
        capture_output=True, text=True)
    if dumped.returncode != 0:
        return None
    env = {}
    for line in dumped.stdout.splitlines():
        key, sep, value = line.partition("=")
        if sep and key:
            env[key.upper()] = value
    return env if "PATH" in env else None


class ShowcaseNativeTests(unittest.TestCase):
    def test_header_present(self):
        self.assertTrue(HEADER.is_file(), HEADER)
        self.assertTrue(TEST_SOURCE.is_file(), TEST_SOURCE)

    def test_policy_host_compile_and_run(self):
        with tempfile.TemporaryDirectory(prefix="pgrazor-showcase-") as work:
            work = Path(work)
            if platform.system() == "Windows":
                # Resolver fixture substitutes clock/TID itself; Windows has no
                # unistd header. Never replace any production game operation.
                (work / "unistd.h").write_text("#pragma once\n")
                env = _vcvars_env()
                if env is None:
                    self.skipTest("MSVC x64 toolchain (vswhere/vcvars64) unavailable")
                compiler = shutil.which("cl.exe", path=env["PATH"])
                self.assertIsNotNone(compiler, "vcvars succeeded but cl.exe is missing")
                executable = work / "showcase_policy_test.exe"
                compile_cmd = [
                    compiler, "/nologo", "/std:c++17", "/EHsc", "/O2", "/W4", "/I"+str(work),
                    str(TEST_SOURCE), *map(str, SCORE_SOURCES),
                    "/Fe" + str(executable), "/Fo" + str(work) + os.sep,
                ]
            else:
                env = None
                compiler = (shutil.which("c++") or shutil.which("g++")
                            or shutil.which("clang++"))
                if not compiler:
                    self.skipTest("No host C++ compiler available")
                executable = work / "showcase_policy_test"
                compile_cmd = [compiler, "-O2", "-std=c++17", "-Wall",
                               str(TEST_SOURCE), *map(str, SCORE_SOURCES), "-o", str(executable)]

            compiled = subprocess.run(compile_cmd, cwd=str(work), env=env,
                                      capture_output=True, text=True, timeout=240)
            self.assertEqual(compiled.returncode, 0,
                             "compile failed:\n" + compiled.stdout + compiled.stderr)
            self.assertTrue(executable.is_file(), "no executable produced")

            ran = subprocess.run([str(executable)], cwd=str(work),
                                 capture_output=True, text=True, timeout=120)
            self.assertEqual(ran.returncode, 0,
                             "test binary failed:\n" + ran.stdout + ran.stderr)
            self.assertIn("PASS", ran.stdout,
                          "missing PASS marker:\n" + ran.stdout + ran.stderr)
            print(ran.stdout.strip())


if __name__ == "__main__":
    unittest.main()
