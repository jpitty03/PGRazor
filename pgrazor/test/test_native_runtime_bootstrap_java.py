"""Compile the actual bootstrap helper; native resolution and event sink are JVM stubs."""
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_control_java import java_tools

ROOT = Path(__file__).resolve().parents[1]
STUBS = {
    'NativeCore.java': '''package dev.pgrazor;
public final class NativeCore {
 public static volatile int calls; public static volatile long value;
 public static int gameplayCalls,gameplayStatus;public static Runnable gameplayCallback;
 public static int runtimeStart(int owner){if(owner!=1)throw new AssertionError();gameplayCalls++;if(gameplayCallback!=null)gameplayCallback.run();return gameplayStatus;}
 public static Throwable failure; public static Runnable callback;
 public static long bootstrapProbe(){calls++;if(callback!=null)callback.run();
  if(failure instanceof Error)throw (Error)failure;
  if(failure instanceof RuntimeException)throw (RuntimeException)failure;
  return value;}
}''',
    'Control.java': '''package dev.pgrazor;
public final class Control {
 public static final java.util.List<String> events=new java.util.ArrayList<>();
 public static Error failure; public static Runnable callback;
 public static void event(String kind,String detail){events.add(kind+"|"+detail);
  if(callback!=null)callback.run();if(failure!=null)throw failure;}
}''',
}


class NativeRuntimeBootstrapJavaTests(unittest.TestCase):
    def test_actual_bootstrap_state_machine(self):
        compiler, runtime = java_tools()
        with tempfile.TemporaryDirectory(prefix='pgrazor-bootstrap-') as directory:
            work = Path(directory)
            sources = []
            for name, text in STUBS.items():
                source = work / name
                source.write_text(text, encoding='utf-8')
                sources.append(source)
            sources += [ROOT / 'src/dev/pgrazor/NativeRuntimeBootstrap.java',
                        ROOT / 'test/NativeRuntimeBootstrapTest.java']
            classes = work / 'classes'
            compiled = subprocess.run([str(compiler), '-encoding', 'UTF-8', '-d', str(classes),
                                       *map(str, sources)], capture_output=True, text=True, timeout=60)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            for scenario in ('unarmed', 'ready', 'retry', 'error', 'throwable',
                             'event-throwable', 'reentry', 'concurrent', 'gameplay', 'gameplay-error'):
                with self.subTest(scenario=scenario):
                    tested = subprocess.run([str(runtime), '-ea', '-cp', str(classes),
                                             'dev.pgrazor.NativeRuntimeBootstrapTest', scenario],
                                            capture_output=True, text=True, timeout=20)
                    self.assertEqual(tested.returncode, 0, tested.stdout + tested.stderr)
                    self.assertIn('PASS NativeRuntimeBootstrap ' + scenario, tested.stdout)


if __name__ == '__main__':
    unittest.main()
