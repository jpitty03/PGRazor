import subprocess
import tempfile
import unittest
from pathlib import Path
from test_control_java import java_tools
ROOT=Path(__file__).resolve().parents[1]
class RootDiagnosticsJavaTests(unittest.TestCase):
    def test_passive_root_indicators(self):
        compiler,runtime=java_tools()
        with tempfile.TemporaryDirectory(prefix='pgrazor-root-diagnostics-') as out:
            subprocess.run([str(compiler),'-d',out,str(ROOT/'src/dev/pgrazor/RootDiagnostics.java'),str(ROOT/'test/RootDiagnosticsTest.java')],check=True,capture_output=True,text=True,timeout=60)
            result=subprocess.run([str(runtime),'-cp',out,'dev.pgrazor.RootDiagnosticsTest'],capture_output=True,text=True,timeout=15)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
