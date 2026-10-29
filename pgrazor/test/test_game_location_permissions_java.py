"""Exercise the compiled GameLocationPermissions policy on a JVM with narrow stubs.

Real Java enum fixtures exercise reflection without Frida's field wrappers. Dialogs and Android
Activity use narrow shells. No Android SDK or device is required; no credentials are touched.
"""
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_control_java import java_tools

ROOT = Path(__file__).resolve().parents[1]

STUBS = {
    # Fake Control: the helper only reads desiredEnabled() and records via event().
    'dev/pgrazor/Control.java': '''package dev.pgrazor;
public class Control {
 public static boolean desired=true;
 public static final java.util.List<String> events=new java.util.ArrayList<>();
 public static boolean desiredEnabled(){return desired;}
 public static void event(String kind,String detail){events.add(kind+"|"+detail);}
}''',
    'android/view/View.java': 'package android.view; public class View {}',
    'android/app/Activity.java': '''package android.app;
public class Activity {
 public boolean finishing=false;
 // -1 denied, 0 granted; the helper only reads these, never changes them.
 public int fine=-1, coarse=-1;
 public boolean isFinishing(){return finishing;}
 public int checkSelfPermission(String permission){return permission.endsWith("ACCESS_FINE_LOCATION")?fine:coarse;}
}''',
    # Java enum constants are the objects themselves; .value belongs to Frida's field wrapper.
    'com/nianticlabs/platform/niaadvsynccore/permission/Permissions.java': '''package com.nianticlabs.platform.niaadvsynccore.permission;
public enum Permissions {
 LOCATION(1L), PRECISE_LOCATION(9007199254740993L);
 private final long value;
 Permissions(long value){this.value=value;}
 public long getValue(){return value;}
}''',
    'com/nianticlabs/platform/niaadvsynccore/permission/PermissionStatus.java': '''package com.nianticlabs.platform.niaadvsynccore.permission;
public enum PermissionStatus {
 GRANTED_IN_USE, GRANTED_ALWAYS;
}''',
    'com/nianticlabs/background/configuration/InitialDialog.java': '''package com.nianticlabs.background.configuration;
public class InitialDialog extends android.app.Activity {
 public boolean cancelled=false;
 public void onClickCancel(android.view.View v){cancelled=true;}
}''',
    'com/nianticlabs/background/configuration/ManualPermissionDialog.java': '''package com.nianticlabs.background.configuration;
public class ManualPermissionDialog extends android.app.Activity {
 public boolean cancelled=false;
 public void onClickCancel(android.view.View v){cancelled=true;}
}''',
    'com/nianticlabs/background/configuration/FinalDialog.java': '''package com.nianticlabs.background.configuration;
public class FinalDialog extends android.app.Activity {
 public boolean closed=false;
 public void onClickClose(android.view.View v){closed=true;}
}''',
    # A whitelisted dialog whose OS check throws, to prove reflection faults are contained.
    'com/nianticlabs/background/configuration/FaultyDialog.java': '''package com.nianticlabs.background.configuration;
public class FaultyDialog extends InitialDialog {
 public int checkSelfPermission(String permission){throw new RuntimeException("sensor-unavailable");}
}''',
}


class GameLocationPermissionsJavaTests(unittest.TestCase):
    def test_permission_policy(self):
        compiler, runtime = java_tools()
        with tempfile.TemporaryDirectory(prefix='pgrazor-location-perm-test-') as directory:
            work = Path(directory)
            sources = []
            for relative, text in STUBS.items():
                source = work / relative
                source.parent.mkdir(parents=True, exist_ok=True)
                source.write_text(text, encoding='utf-8')
                sources.append(source)
            sources.append(ROOT / 'src/dev/pgrazor/GameLocationPermissions.java')
            sources.append(ROOT / 'test/GameLocationPermissionsTest.java')
            classes = work / 'classes'
            compiled = subprocess.run([str(compiler), '-encoding', 'UTF-8', '-d', str(classes),
                                       *map(str, sources)], capture_output=True, text=True, timeout=30)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            tested = subprocess.run([str(runtime), '-cp', str(classes), 'GameLocationPermissionsTest'],
                                    capture_output=True, text=True, timeout=15)
            self.assertEqual(tested.returncode, 0, tested.stdout + tested.stderr)
            self.assertIn('PASS game location permissions', tested.stdout)


if __name__ == '__main__':
    unittest.main()
