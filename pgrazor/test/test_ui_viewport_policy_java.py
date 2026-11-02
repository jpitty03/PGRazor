"""Exercise the production viewport policy without requiring an Android runtime."""
from pathlib import Path
import os,shutil,subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class UiViewportPolicyTests(unittest.TestCase):
    def test_window_matrix(self):
        home=Path(os.environ.get('JAVA_HOME',''))/'bin'
        suffix='.exe' if os.name=='nt' else ''
        javac=str(home/('javac'+suffix)) if (home/('javac'+suffix)).is_file() else shutil.which('javac')
        java=str(home/('java'+suffix)) if (home/('java'+suffix)).is_file() else shutil.which('java')
        if not javac or not java:self.skipTest('JDK required')
        with tempfile.TemporaryDirectory(prefix='pgrazor-viewport-') as work:
            subprocess.run([javac,'-encoding','UTF-8','-d',work,str(ROOT/'src/dev/pgrazor/UiViewportPolicy.java'),str(ROOT/'test/UiViewportPolicyTest.java')],check=True,timeout=45)
            result=subprocess.run([java,'-cp',work,'dev.pgrazor.UiViewportPolicyTest'],capture_output=True,text=True,timeout=20)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertIn('3360 combinations',result.stdout)
            print(result.stdout.strip())
