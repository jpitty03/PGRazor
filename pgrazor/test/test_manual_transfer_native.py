"""Execute the production warning/eligibility policy without invoking game transfers."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import unittest
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT))
import native_build
class ManualTransferNativeTests(unittest.TestCase):
    def test_production_policy(self):
        if not shutil.which('node'):self.skipTest('Node unavailable')
        try:ndk,_=native_build.resolve_ndk(ROOT)
        except RuntimeError as exc:self.skipTest(str(exc))
        host={'win32':'windows-x86_64','linux':'linux-x86_64','darwin':'darwin-x86_64'}[sys.platform]
        compiler=ndk/'toolchains/llvm/prebuilt'/host/'bin'/('clang++.exe' if os.name=='nt' else 'clang++')
        work=ROOT/'build/manual-transfer-host';work.mkdir(parents=True,exist_ok=True);output=work/'policy.wasm'
        args=[compiler,'--target=wasm32','-O2','-nostdlib','-fno-exceptions','-fno-rtti','-fno-builtin',
              ROOT/'test/manual_transfer_policy_test.cpp','-Wl,--no-entry','-Wl,--export-all','-o',output]
        built=subprocess.run(list(map(str,args)),capture_output=True,text=True,timeout=90)
        self.assertEqual(built.returncode,0,built.stdout+built.stderr)
        script="const fs=require('fs');WebAssembly.instantiate(fs.readFileSync(process.argv[1])).then(({instance})=>{const line=instance.exports.run_manual_tests();if(line)throw Error('manual policy failed at C++ line '+line);console.log('PASS native manual policy: 128 eligibility combinations, favorites/eggs/gym/unknown, explicit manual intent, ordinary confirmation, 1200 button decisions, expiry, Off and consume-before-click');}).catch(e=>{console.error(e);process.exit(1);});"
        ran=subprocess.run(['node','-e',script,str(output)],capture_output=True,text=True,timeout=30)
        self.assertEqual(ran.returncode,0,ran.stdout+ran.stderr)
        print(ran.stdout.strip())
