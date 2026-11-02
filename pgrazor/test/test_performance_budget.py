from pathlib import Path
import os,shutil,subprocess,unittest
ROOT=Path(__file__).resolve().parents[1]
class PerformanceBudgetTests(unittest.TestCase):
 def test_production_policy(self):
  work=ROOT/'build/performance-test';work.mkdir(parents=True,exist_ok=True)
  source=ROOT/'test/performance_budget_test.cpp'
  if os.name=='nt':
   versions=sorted(Path('C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC').glob('*'),reverse=True)
   kits=Path('C:/Program Files (x86)/Windows Kits/10');sdks=sorted((kits/'Include').glob('*'),reverse=True)
   if not versions or not sdks:self.skipTest('MSVC C++ host test compiler unavailable')
   vc=versions[0];sdk=sdks[0].name;binary=vc/'bin/Hostx64/x64';env=os.environ.copy()
   env['PATH']=str(binary)+os.pathsep+env.get('PATH','')
   env['INCLUDE']=';'.join(map(str,[vc/'include',kits/f'Include/{sdk}/ucrt',kits/f'Include/{sdk}/shared',kits/f'Include/{sdk}/um']))
   env['LIB']=';'.join(map(str,[vc/'lib/x64',kits/f'Lib/{sdk}/ucrt/x64',kits/f'Lib/{sdk}/um/x64']))
   exe=work/'policy.exe';cmd=[str(binary/'cl.exe'),'/nologo','/std:c++17','/EHsc','/O2',str(source),'/Fe:'+str(exe),'/Fo:'+str(work/'policy.obj')]
  else:
   compiler=shutil.which('c++')
   if not compiler:self.skipTest('C++ host test compiler unavailable')
   env=None;exe=work/'policy';cmd=[compiler,'-std=c++17','-O2',str(source),'-o',str(exe)]
  result=subprocess.run(cmd,env=env,cwd=work,capture_output=True,text=True,timeout=60);self.assertEqual(result.returncode,0,result.stdout+result.stderr)
  result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=15);self.assertEqual(result.returncode,0,result.stdout+result.stderr);print(result.stdout.strip())
