import os
import platform
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from test_showcase_native import _vcvars_env

ROOT=Path(__file__).resolve().parents[1]
class ShowcaseTagRuntimeTests(unittest.TestCase):
    SOURCE = 'showcase_tag_runtime_test.cpp'
    MARKER = 'PASS tag RPC primitive guards'
    HEADERS = {}
    def test_primitive_guards(self):
        with tempfile.TemporaryDirectory(prefix='showcase-tag-rpc-') as folder:
            work=Path(folder)
            (work/'dlfcn.h').write_text('#pragma once\n')
            for name, contents in self.HEADERS.items():
                header=work/name
                header.parent.mkdir(parents=True,exist_ok=True)
                header.write_text(contents)
            source=ROOT/'native'/self.SOURCE
            if platform.system()=='Windows':
                if not (work/'unistd.h').exists():
                    (work/'unistd.h').write_text('#pragma once\n')
                env=_vcvars_env()
                if env is None:self.skipTest('MSVC unavailable')
                compiler=shutil.which('cl.exe',path=env['PATH']);self.assertIsNotNone(compiler)
                exe=work/'test.exe'
                command=[compiler,'/nologo','/std:c++17','/EHsc','/I'+str(work),str(source),'/Fe'+str(exe),'/Fo'+str(work)+os.sep]
            else:
                env=None;compiler=shutil.which('c++')
                if not compiler:self.skipTest('C++ compiler unavailable')
                exe=work/'test';command=[compiler,'-std=c++17','-I'+str(work),str(source),'-o',str(exe)]
            built=subprocess.run(command,cwd=work,env=env,capture_output=True,text=True,timeout=120)
            self.assertEqual(built.returncode,0,built.stdout+built.stderr)
            ran=subprocess.run([str(exe)],capture_output=True,text=True,timeout=30)
            self.assertEqual(ran.returncode,0,ran.stdout+ran.stderr)
            self.assertIn(self.MARKER,ran.stdout)
            print(ran.stdout.strip())
if __name__=='__main__':unittest.main()
