"""Execute the production Rocket flow policy on the host, without game hooks."""
from pathlib import Path
import sys,os,shutil,subprocess,unittest
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT))
import native_build
class RocketAutoTests(unittest.TestCase):
 def test_flow_and_authorization(self):
  if not shutil.which('node'):self.skipTest('Node required')
  try:ndk,_=native_build.resolve_ndk(ROOT)
  except RuntimeError as e:self.skipTest(str(e))
  host={'win32':'windows-x86_64','linux':'linux-x86_64','darwin':'darwin-x86_64'}[sys.platform]
  clang=ndk/'toolchains/llvm/prebuilt'/host/'bin'/('clang++.exe' if os.name=='nt' else 'clang++')
  work=ROOT/'build/rocket-auto-host';work.mkdir(parents=True,exist_ok=True);out=work/'flow.wasm'
  args=[clang,'--target=wasm32','-O2','-nostdlib','-fno-exceptions','-fno-rtti','-fno-builtin','-I'+str(ROOT/'native'),ROOT/'test/rocket_auto_wasm.cpp','-Wl,--no-entry','-Wl,--export-all','-o',out]
  result=subprocess.run(list(map(str,args)),capture_output=True,text=True,timeout=60);self.assertEqual(result.returncode,0,result.stdout+result.stderr)
  code="const fs=require('fs');WebAssembly.instantiate(fs.readFileSync(process.argv[1]),{}).then(({instance})=>{const n=instance.exports.run();if(n)throw Error('Rocket policy check '+n);console.log('PASS native Auto Rocket: masks 0..63, stop cooldown, leader wait, dialogue/party settle, one submit, rescue/Done, Premier selection, in-flight ball and swipe cadence, revocation, bounded cache and hidden party');}).catch(e=>{console.error(e);process.exit(1);});"
  result=subprocess.run(['node','-e',code,str(out)],capture_output=True,text=True,timeout=30);self.assertEqual(result.returncode,0,result.stdout+result.stderr);print(result.stdout.strip())
