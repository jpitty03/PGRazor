import os
import platform
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from test_showcase_native import _vcvars_env

ROOT=Path(__file__).resolve().parents[1]

class ShowcaseInventoryNativeTests(unittest.TestCase):
    def test_reader(self):
        with tempfile.TemporaryDirectory(prefix='showcase-inventory-') as folder:
            work=Path(folder)
            # The production include calls the injected dlsym in the fixture.
            (work/'dlfcn.h').write_text('#pragma once\n')
            sources=[ROOT/'native/showcase_inventory_test.cpp',ROOT/'native/showcase_score.cpp']
            if platform.system()=='Windows':
                env=_vcvars_env()
                if env is None:self.skipTest('MSVC unavailable')
                compiler=shutil.which('cl.exe',path=env['PATH'])
                self.assertIsNotNone(compiler)
                exe=work/'test.exe'
                command=[compiler,'/nologo','/std:c++17','/EHsc','/W4','/I'+str(work),*map(str,sources),'/Fe'+str(exe),'/Fo'+str(work)+os.sep]
            else:
                env=None;compiler=shutil.which('c++')
                if not compiler:self.skipTest('C++ compiler unavailable')
                exe=work/'test'
                command=[compiler,'-std=c++17','-I'+str(work),*map(str,sources),'-o',str(exe)]
            built=subprocess.run(command,cwd=work,env=env,capture_output=True,text=True,timeout=120)
            self.assertEqual(built.returncode,0,built.stdout+built.stderr)
            ran=subprocess.run([str(exe)],capture_output=True,text=True,timeout=30)
            self.assertEqual(ran.returncode,0,ran.stdout+ran.stderr)
            self.assertIn('PASS production inventory reader',ran.stdout)
            print(ran.stdout.strip())

if __name__=='__main__':unittest.main()
