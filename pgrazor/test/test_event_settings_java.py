"""Exercise real event preferences with an in-memory SharedPreferences shell."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_control_java import java_tools

ROOT = Path(__file__).resolve().parents[1]
STUBS = {
    'android/content/SharedPreferences.java': '''package android.content;
public interface SharedPreferences {
 boolean getBoolean(String key,boolean fallback); Editor edit();
 interface Editor { Editor putBoolean(String key,boolean value); boolean commit(); }
}''',
    'android/content/Context.java': '''package android.content;
public class Context {
 public static final int MODE_PRIVATE=0; public static boolean failWrites;
 private static final java.util.Map<String,Boolean> values=new java.util.HashMap<>();
 public Context getApplicationContext(){return this;}
 public SharedPreferences getSharedPreferences(String name,int mode){
  if(!name.equals("pgrazor-events-v1"))throw new AssertionError("separate event namespace");
  return new SharedPreferences(){
   public boolean getBoolean(String key,boolean fallback){return values.getOrDefault(key,fallback);}
   public Editor edit(){return new Editor(){java.util.Map<String,Boolean> pending=new java.util.HashMap<>();
    public Editor putBoolean(String k,boolean v){pending.put(k,v);return this;}
    public boolean commit(){values.putAll(pending);return !failWrites;}
   };}
  };
 }
}''',
}

class EventSettingsJavaTests(unittest.TestCase):
    def test_capability_revocation_and_persistent_intent(self):
        compiler, runtime = java_tools()
        with tempfile.TemporaryDirectory(prefix='pgrazor-events-') as directory:
            work = Path(directory)
            sources = []
            for name, text in STUBS.items():
                source = work / name
                source.parent.mkdir(parents=True, exist_ok=True)
                source.write_text(text, encoding='utf-8')
                sources.append(source)
            sources += [ROOT/'src/dev/pgrazor/events/EventSettings.java', ROOT/'test/EventSettingsTest.java']
            compiled = subprocess.run([str(compiler), '-encoding', 'UTF-8', '-d', str(work/'classes'),
                                       *map(str, sources)], capture_output=True, text=True, timeout=60)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            tested = subprocess.run([str(runtime), '-ea', '-cp', str(work/'classes'),
                                     'dev.pgrazor.events.EventSettingsTest'],
                                    capture_output=True, text=True, timeout=20)
            self.assertEqual(tested.returncode, 0, tested.stdout + tested.stderr)
            self.assertIn('PASS EventSettings', tested.stdout)

if __name__ == '__main__': unittest.main()
