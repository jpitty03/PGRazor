"""Exercise the actual NativeRuntimeTransport on a JVM with a narrow Control shell.

The transport is a typed binary boundary with no JSON/JS and no Android dependency, so this harness needs
only a JDK -- no Android SDK sources or device. The Control shell is generated from the legacy mailbox
allowlist (the shared source of truth) and cross-checked against the real Control.java at generation time.
"""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from test_control_java import java_tools

ROOT = Path(__file__).resolve().parents[1]


def control_shell():
    """Build a Control shell whose signatures match production Control.java exactly."""
    source = (ROOT / 'src/dev/pgrazor/Control.java').read_text(encoding='utf-8')
    mailbox = (ROOT / 'src/dev/pgrazor/RuntimeMailbox.java').read_text(encoding='utf-8')
    specs = re.findall(r'^\s*"(\w+):([^":]*):([^":]+)"', mailbox, re.M)
    specs.append(('eventBoxesAllowed', '', 'boolean'))
    specs.append(('smartCatchConfig', '', 'String'))
    specs.append(('gymAutomationConfig', '', 'String'))
    specs.append(('buddyEmotionConfig', '', 'String'))
    specs.append(('buddyEmotionHistory', '', 'String'))
    specs.append(('batchEvolutionConfig', '', 'String'))
    methods = []
    for name, params, expected_result in specs:
        actual = re.search(r'public static\s+(?:synchronized\s+)?([\w\[\]]+)\s+' + name + r'\(([^)]*)\)', source)
        assert actual, name
        types = [p.strip().split()[0] for p in actual[2].split(',') if p.strip()]
        assert ','.join(types) == params and actual[1] == expected_result, name
        signature = ','.join(t + ' a' + str(i) for i, t in enumerate(types))
        if name == 'accountCommandSnapshot':
            body = 'reads++;return snapshot;'
        elif name == 'gymAutomationConfig':
            body = 'reads++;return "1;1;0;0;50;10";'
        elif name in ('accountIdentity', 'accountSessionSaved'):
            body = 'mutations++;if(onMutation!=null)onMutation.run();if(throwMutation)throw new RuntimeException("id-secret");'
        elif name == 'event':
            body = 'lastDiagnostic=a0;'
        elif name == 'effectiveBerryPriority':
            body = 'reads++;return badBerry;'  # settable so the fixture can drive a malformed-UTF-16 string
        elif name == 'updateCameraHeadingAt':
            body = 'timestamp=a3;'
        elif name.startswith('take'):
            body = 'boolean result=pulse;pulse=false;return result;'
        elif not types and expected_result != 'void':
            default = {'boolean': 'false', 'String': '""', 'int': '0', 'double': 'Double.NaN'}[expected_result]
            body = 'reads++;return ' + default + ';'
        else:
            body = 'return true;' if expected_result == 'boolean' else ''
        methods.append('public static ' + expected_result + ' ' + name + '(' + signature + '){' + body + '}')
    return '''package dev.pgrazor;
public final class Control {
 public static int connects, renewals, disconnects;
 public static void connect(){connects++;}
 public static void renewLease(){renewals++;}
 public static void disconnect(){disconnects++;}
 public static int reads,mutations; public static boolean pulse,throwMutation;
 public static long timestamp;
 public static String[] snapshot={"command","18446744073709551615"};
 public static Runnable onMutation; public static String lastDiagnostic=""; public static String badBerry="";
 public static void reset(){reads=mutations=0;pulse=throwMutation=false;timestamp=0;onMutation=null;lastDiagnostic="";badBerry="";snapshot=new String[]{"command","18446744073709551615"};}
''' + '\n'.join(methods) + '\n}\n'


class NativeRuntimeTransportJavaTests(unittest.TestCase):
    def test_transport(self):
        compiler, runtime = java_tools()
        with tempfile.TemporaryDirectory(prefix='pgrazor-native-transport-') as directory:
            work = Path(directory)
            (work / 'Control.java').write_text(control_shell(), encoding='utf-8')
            sources = [
                work / 'Control.java',
                ROOT / 'src/dev/pgrazor/NativeRuntimeTransport.java',
                ROOT / 'test/NativeRuntimeTransportTest.java',
            ]
            compiled = subprocess.run([str(compiler), '-encoding', 'UTF-8', '-d', str(work), *map(str, sources)],
                                      capture_output=True, text=True, timeout=45)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            tested = subprocess.run([str(runtime), '-ea', '-cp', str(work), 'dev.pgrazor.NativeRuntimeTransportTest'],
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(tested.returncode, 0, tested.stdout + tested.stderr)
            self.assertIn('PASS NativeRuntimeTransport:', tested.stdout)


if __name__ == '__main__':
    unittest.main()
