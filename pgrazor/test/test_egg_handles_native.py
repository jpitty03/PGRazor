from pathlib import Path
import os,sys,shutil,subprocess,unittest
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT))
import native_build
class EggHandlesTests(unittest.TestCase):
 def test_high_bits_and_reentrant_release(self):
  if not shutil.which('node'):self.skipTest('Node required')
  try:ndk,_=native_build.resolve_ndk(ROOT)
  except RuntimeError as e:self.skipTest(str(e))
  host={'win32':'windows-x86_64','linux':'linux-x86_64','darwin':'darwin-x86_64'}[sys.platform]
  clang=ndk/'toolchains/llvm/prebuilt'/host/'bin'/('clang++.exe' if os.name=='nt' else 'clang++')
  work=ROOT/'build/egg-handle-test';work.mkdir(parents=True,exist_ok=True);out=work/'handles.wasm'
  args=[clang,'--target=wasm32','-O2','-nostdlib','-fno-exceptions','-fno-rtti','-I'+str(ROOT/'native'),ROOT/'test/egg_handles_wasm.cpp','-Wl,--no-entry','-Wl,--export-all','-o',out]
  result=subprocess.run(list(map(str,args)),capture_output=True,text=True,timeout=60);self.assertEqual(result.returncode,0,result.stdout+result.stderr)
  code="const fs=require('fs');WebAssembly.instantiate(fs.readFileSync(process.argv[1]),{}).then(({instance})=>{if(instance.exports.run()!==1)process.exit(1);console.log('PASS egg handles: high 64 bits preserved, pinned create ABI, detach before free and no double free on reentry');}).catch(e=>{console.error(e);process.exit(1);});"
  result=subprocess.run(['node','-e',code,str(out)],capture_output=True,text=True,timeout=30);self.assertEqual(result.returncode,0,result.stdout+result.stderr);print(result.stdout.strip())
