"""Scheduler progress diagnostics are read-only and must distinguish background pauses."""
import subprocess
import tempfile
import unittest
from pathlib import Path
from test_control_java import java_tools
ROOT=Path(__file__).resolve().parents[1]
class RuntimeHealthJavaTests(unittest.TestCase):
    def test_progress_diagnostics(self):
        compiler,runtime=java_tools()
        with tempfile.TemporaryDirectory(prefix='pgrazor-health-') as out:
            subprocess.run([str(compiler),'-d',out,str(ROOT/'src/dev/pgrazor/RuntimeHealth.java'),str(ROOT/'test/RuntimeHealthTest.java')],check=True,capture_output=True,text=True)
            result=subprocess.run([str(runtime),'-cp',out,'dev.pgrazor.RuntimeHealthTest'],capture_output=True,text=True,timeout=15)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
