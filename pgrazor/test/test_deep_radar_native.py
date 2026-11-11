"""Compile and execute the production scalar radar and read-export fixture, device-free."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]

class DeepRadarNativeTests(unittest.TestCase):
    def test_native_modules(self):
        with tempfile.TemporaryDirectory(prefix='pgrazor-radar-native-') as directory:
            work=Path(directory)
            (work/'dlfcn.h').write_text('extern "C" void *dlsym(void*,const char*);\n')
            for test,units in [('s2_geometry',['s2_geometry']),('deep_radar_store',['s2_geometry','deep_radar_store']),('deep_radar_reader',['s2_geometry','deep_radar_reader']),('deep_radar_scene',['s2_geometry','deep_radar_store','deep_radar_dex'])]:
                sources=[ROOT/'native'/(s+'.cpp') for s in units]+[ROOT/'native'/(test+'_test.cpp')]
                exe=work/(test+('.exe' if os.name=='nt' else ''))
                if os.name=='nt':
                    vswhere=Path(os.environ.get('ProgramFiles(x86)','C:/Program Files (x86)'))/'Microsoft Visual Studio/Installer/vswhere.exe'
                    self.assertTrue(vswhere.is_file(),'MSVC C++ Build Tools required for native radar fixtures')
                    found=subprocess.run([str(vswhere),'-latest','-products','*','-requires','Microsoft.VisualStudio.Component.VC.Tools.x86.x64','-property','installationPath'],capture_output=True,text=True,check=True)
                    vcvars=Path(found.stdout.strip())/'VC/Auxiliary/Build/vcvars64.bat'
                    self.assertTrue(vcvars.is_file(),'MSVC C++ tools unavailable')
                    script=work/'compile.cmd'
                    command=subprocess.list2cmdline(['cl.exe','/nologo','/std:c++17','/EHsc','/W4','/I'+str(work),'/Fe:'+str(exe),*map(str,sources)])
                    script.write_text('@echo off\ncall "'+str(vcvars)+'" >nul\nif errorlevel 1 exit /b 1\n'+command+'\nexit /b %errorlevel%\n')
                    result=subprocess.run(['cmd.exe','/d','/c',str(script)],cwd=work,capture_output=True,text=True,timeout=90)
                else:
                    compiler=shutil.which('c++')
                    self.assertIsNotNone(compiler,'C++17 compiler required')
                    result=subprocess.run([compiler,'-std=c++17','-Wall','-Wextra','-fno-exceptions','-fno-rtti',*map(str,sources),'-o',str(exe)],cwd=work,capture_output=True,text=True,timeout=90)
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)
                run=subprocess.run([str(exe)],cwd=work,capture_output=True,text=True,timeout=30)
                self.assertEqual(run.returncode,0,run.stdout+run.stderr)
                self.assertIn('PASS',run.stdout)

if __name__=='__main__':unittest.main()
