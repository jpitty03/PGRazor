"""Actual radar state on the JVM with Android's JSON implementation."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from test_control_java import java_tools
ROOT=Path(__file__).resolve().parents[1]
class DeepRadarJavaTests(unittest.TestCase):
    def test_state(self):
        sdk=Path(os.environ.get('ANDROID_HOME',str(Path(os.environ.get('LOCALAPPDATA',str(Path.home())))/'Android/Sdk')))
        candidates=sorted((sdk/'sources').glob('android-*/org/json'),reverse=True)
        self.assertTrue(candidates,'Android JSON sources required for radar fixtures')
        compiler,runtime=java_tools()
        with tempfile.TemporaryDirectory(prefix='pgrazor-radar-java-') as directory:
            work=Path(directory);sources=[]
            for original in candidates[0].glob('*.java'):
                text=original.read_text(encoding='utf-8');text=re.sub(r'^import (?:static )?(?:android\.|libcore\.).*;\s*$','',text,flags=re.M);text=re.sub(r'@(?:UnsupportedAppUsage|SystemApi|NonNull|Nullable)(?:\([^)]*\))?\s*','',text)
                target=work/original.name;target.write_text(text,encoding='utf-8');sources.append(target)
            sources += [ROOT/'src/dev/pgrazor/DeepRadarState.java',ROOT/'test/DeepRadarStateTest.java']
            p=subprocess.run([str(compiler),'-encoding','UTF-8','-d',str(work),*map(str,sources)],capture_output=True,text=True,timeout=30);self.assertEqual(p.returncode,0,p.stdout+p.stderr)
            p=subprocess.run([str(runtime),'-cp',str(work),'dev.pgrazor.DeepRadarStateTest'],capture_output=True,text=True,timeout=30);self.assertEqual(p.returncode,0,p.stdout+p.stderr)
            self.assertIn('PASS',p.stdout)
if __name__=='__main__':unittest.main()
