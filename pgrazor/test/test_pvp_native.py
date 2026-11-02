"""Run the actual portable C++ rank engine on the host; no Android device or game hooks."""
import os
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import unittest
import tempfile
import zipfile

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT))
import native_build

class NativePvpTests(unittest.TestCase):
    def test_pvp_notices_are_written_to_apk(self):
        import build
        import nonroot
        with tempfile.TemporaryDirectory(dir=ROOT/'build') as temp:
            source=Path(temp)/'source.apk';output=Path(temp)/'with-notices.apk'
            with zipfile.ZipFile(source,'w') as z:z.writestr('classes.dex',b'game dex fixture')
            assets={'assets/pgrazor/'+name:path for name,path in build.pvp_notice_assets().items()}
            nonroot.rewrite_apk(source,output,base=True,preview=True,assets=assets)
            with zipfile.ZipFile(output) as z:
                license=z.read('assets/pgrazor/native-notices/PvPoke-LICENSE.txt').decode('utf8')
                self.assertIn('Copyright (c) 2019 pvpoke',license)
                provenance=json.loads(z.read('assets/pgrazor/native-notices/PvPoke-provenance.json'))
                self.assertEqual(provenance['data_sha256'],hashlib.sha256((ROOT/'assets/pvp/data.json').read_bytes().replace(b'\r\n',b'\n')).hexdigest())
    def test_cpp_against_original_patch_oracle(self):
        if not shutil.which('node'):self.skipTest('Node unavailable')
        try:ndk,_=native_build.resolve_ndk(ROOT)
        except RuntimeError as e:self.skipTest(str(e))
        host={'win32':'windows-x86_64','linux':'linux-x86_64','darwin':'darwin-x86_64'}[sys.platform]
        compiler=ndk/'toolchains/llvm/prebuilt'/host/'bin'/('clang++.exe' if os.name=='nt' else 'clang++')
        work=ROOT/'build/pvp-native-host';work.mkdir(parents=True,exist_ok=True);output=work/'pvp.wasm'
        command=[compiler,'--target=wasm32','-O2','-nostdlib','-fno-exceptions','-fno-rtti','-fno-builtin','-fno-math-errno','-ffp-contract=off','-I'+str(ROOT/'native'),ROOT/'test/pvp_wasm_wrapper.cpp','-Wl,--no-entry','-Wl,--export-all','-Wl,--initial-memory=33554432','-o',output]
        compiled=subprocess.run(list(map(str,command)),capture_output=True,text=True,timeout=90)
        self.assertEqual(compiled.returncode,0,compiled.stdout+compiled.stderr)
        tested=subprocess.run(['node',str(ROOT/'test/pvp_native_host.cjs'),str(output)],capture_output=True,text=True,timeout=90)
        self.assertEqual(tested.returncode,0,tested.stdout+tested.stderr)
        print(tested.stdout.strip())
