"""Exercise actual RuntimeMailbox with offline Android org.json and a narrow Control shell."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from test_control_java import java_tools

ROOT = Path(__file__).resolve().parents[1]


def control_shell():
    source = (ROOT / 'src/dev/pgrazor/Control.java').read_text(encoding='utf-8')
    mailbox = (ROOT / 'src/dev/pgrazor/RuntimeMailbox.java').read_text(encoding='utf-8')
    specs = re.findall(r'^\s*"(\w+):([^":]*):([^":]+)"', mailbox, re.M)
    methods = []
    for name, params, expected_result in specs:
        actual = re.search(r'public static\s+(?:synchronized\s+)?([\w\[\]]+)\s+' + name + r'\(([^)]*)\)', source)
        assert actual, name
        types = [p.strip().split()[0] for p in actual[2].split(',') if p.strip()]
        assert ','.join(types) == params and actual[1] == expected_result, name
        signature = ','.join(t + ' a' + str(i) for i, t in enumerate(types))
        if name == 'accountCommandSnapshot':
            body = 'reads++;return snapshot;'
        elif name in ('accountIdentity', 'accountSessionSaved'):
            body = 'mutations++;if(onMutation!=null)onMutation.run();if(throwMutation)throw new RuntimeException("id-secret");'
        elif name == 'event':
            body = 'lastDiagnostic=a1;'
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
 public static int reads,mutations; public static boolean pulse,throwMutation;
 public static long timestamp; public static RuntimeMailbox mailbox;
 public static String[] snapshot={"command","18446744073709551615"};
 public static Runnable onMutation; public static String lastDiagnostic="";
 public static void reset(){reads=mutations=0;pulse=throwMutation=false;timestamp=0;onMutation=null;lastDiagnostic="";snapshot=new String[]{"command","18446744073709551615"};}
''' + '\n'.join(methods) + '\n}\n'


class RuntimeMailboxJavaTests(unittest.TestCase):
    def test_transport(self):
        sdk = Path(os.environ.get('ANDROID_HOME', str(Path(os.environ.get('LOCALAPPDATA', '')) / 'Android/Sdk')))
        candidates = sorted((sdk / 'sources').glob('android-*/org/json'), reverse=True)
        if not candidates:
            self.skipTest('Android SDK JSON sources unavailable')
        compiler, runtime = java_tools()
        with tempfile.TemporaryDirectory(prefix='pgrazor-runtime-mailbox-') as directory:
            work = Path(directory)
            sources = []
            for original in candidates[0].glob('*.java'):
                text = original.read_text(encoding='utf-8')
                text = re.sub(r'^import (?:static )?(?:android\.|libcore\.).*;\s*$', '', text, flags=re.M)
                text = re.sub(r'@(?:UnsupportedAppUsage|SystemApi|NonNull|Nullable)(?:\([^)]*\))?\s*', '', text)
                target = work / original.name
                target.write_text(text, encoding='utf-8')
                sources.append(target)
            shell = work / 'Control.java'
            shell.write_text(control_shell(), encoding='utf-8')
            # Factory exceptions stay constant in production. A fixture-only source
            # copy includes the cause, allowing local filesystem/JVM failures to be diagnosed.
            mailbox_source = work / 'RuntimeMailbox.java'
            mailbox_source.write_text((ROOT / 'src/dev/pgrazor/RuntimeMailbox.java').read_text(encoding='utf-8').replace(
                'new IllegalArgumentException("Runtime mailbox configuration invalid")',
                'new IllegalArgumentException("Runtime mailbox configuration invalid",error)'), encoding='utf-8')
            sources += [shell, mailbox_source, ROOT / 'test/RuntimeMailboxTest.java']
            compiled = subprocess.run([str(compiler), '-encoding', 'UTF-8', '-d', str(work), *map(str, sources)],
                                      capture_output=True, text=True, timeout=45)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            fixture = work / 'mailboxes'
            fixture.mkdir()
            tested = subprocess.run([str(runtime), '-ea', '-cp', str(work), 'dev.pgrazor.RuntimeMailboxTest', str(fixture)],
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(tested.returncode, 0, tested.stdout + tested.stderr)
            self.assertIn('PASS RuntimeMailbox:', tested.stdout)
            if 'LIMITATION:' in tested.stdout:
                print(tested.stdout.strip())


if __name__ == '__main__':
    unittest.main()
