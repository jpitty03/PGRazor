"""Exercise backup test recovery under failed and delayed real helper dispatch."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from test_control_java import java_tools

ROOT = Path(__file__).resolve().parents[1]
STUBS = {
    'android/app/Application.java': '''package android.app;import java.io.File;
public class Application {public File dir;public Application(File f){dir=f;}public File getFilesDir(){return dir;}}''',
    'android/os/SystemClock.java': '''package android.os;public class SystemClock {public static long uptimeMillis(){return System.nanoTime()/1000000;}}''',
    'android/os/Looper.java': '''package android.os;public class Looper {public static Looper getMainLooper(){return new Looper();}}''',
    'android/os/Handler.java': '''package android.os;public class Handler {
public static boolean drop;public Handler(Looper l){}public boolean post(Runnable r){if(!drop)new Thread(r).start();return true;}
public void removeCallbacks(Runnable r){}}''',
    'android/util/AtomicFile.java': '''package android.util;import java.io.*;import java.nio.file.*;
public class AtomicFile {File f;public AtomicFile(File f){this.f=f;}public File getBaseFile(){return f;}
public FileOutputStream startWrite()throws Exception{return new FileOutputStream(f);}
public void finishWrite(FileOutputStream s)throws Exception{s.close();}public void failWrite(FileOutputStream s)throws Exception{s.close();}
public byte[] readFully()throws Exception{return Files.readAllBytes(f.toPath());}public void delete(){f.delete();}}''',
    'dev/pgrazor/Control.java': '''package dev.pgrazor;public class Control {
static boolean joystick,hit,active,fail,slow;public static boolean regressionAutomationActive(){return active;}
public static boolean joystickVisible(){return joystick;}public static boolean throwAlwaysHit(){return hit;}
public static void setJoystickVisible(boolean v){joystick=v;if(fail)throw new RuntimeException();
if(slow){slow=false;try{Thread.sleep(2700);}catch(Exception e){throw new RuntimeException(e);}}}
public static void setThrowAlwaysHit(boolean v){hit=v;}}''',
    'dev/pgrazor/SettingsBackup.java': '''package dev.pgrazor;import android.app.Application;import org.json.*;
class SettingsBackup {static boolean failRestore,failSnapshot;static int mutations;
static synchronized JSONObject snapshot(Application a)throws Exception{if(failSnapshot)throw new Exception();
return new JSONObject().put("settings",new JSONObject().put("joystickVisible",""+Control.joystick).put("throwAlwaysHit",""+Control.hit))
.put("preferences",new JSONObject().put("nested",new JSONArray().put(1).put(true))).put("speedKmh",9.0);}
static synchronized void restore(Application a,JSONObject d)throws Exception{mutations++;if(failRestore)throw new Exception();
JSONObject s=d.getJSONObject("settings");Control.joystick=Boolean.parseBoolean(s.getString("joystickVisible"));
Control.hit=Boolean.parseBoolean(s.getString("throwAlwaysHit"));}}''',
    'dev/pgrazor/BackupRegressionTest.java': '''package dev.pgrazor;import java.io.*;import android.app.Application;import org.json.*;
public class BackupRegressionTest {
static void check(boolean b){if(!b)throw new AssertionError();}
public static void main(String[] args)throws Exception{
Application app=new Application(new File(args[0]));new File(app.dir,"pgrazor-regression").mkdirs();
File recovery=new File(app.dir,"pgrazor-regression/backup-recovery.json");
JSONObject r=SettingsBackupRegression.run(app);check(r.getBoolean("ok")&&!Control.joystick&&!Control.hit&&!recovery.exists());
Control.active=true;try{SettingsBackupRegression.run(app);throw new AssertionError();}catch(IllegalStateException expected){}
check(!recovery.exists());Control.active=false;
SettingsBackup.failSnapshot=true;try{SettingsBackupRegression.run(app);throw new AssertionError();}catch(Exception expected){}
check(!recovery.exists());SettingsBackup.failSnapshot=false;
Control.fail=true;r=SettingsBackupRegression.run(app);check(!r.getBoolean("ok")&&r.getBoolean("restored")&&!Control.joystick&&!Control.hit);Control.fail=false;
SettingsBackup.failRestore=true;r=SettingsBackupRegression.run(app);check(!r.getBoolean("ok")&&!r.getBoolean("restored")&&recovery.exists());
try{SettingsBackupRegression.run(app);throw new AssertionError();}catch(IllegalStateException expected){}
SettingsBackup.failRestore=false;check(SettingsBackupRegression.recover(app).getBoolean("restored")&&!Control.joystick&&!Control.hit&&!recovery.exists());
android.os.Handler.drop=true;r=SettingsBackupRegression.run(app);check(!r.getBoolean("ok")&&!Control.joystick&&!Control.hit);android.os.Handler.drop=false;
Control.slow=true;r=SettingsBackupRegression.run(app);check(!r.getBoolean("ok")&&!r.getBoolean("restored")&&recovery.exists());
try{SettingsBackupRegression.recover(app);throw new AssertionError();}catch(IllegalStateException expected){}
Thread.sleep(700);check(!Control.joystick&&!Control.hit);check(SettingsBackupRegression.recover(app).getBoolean("restored")&&!recovery.exists());
System.out.println("PASS success, preflight, capture failure, partial mutation, restore failure, pending cancellation, late mutation compensation, recovery");}}
''',
}


class BackupRegressionTests(unittest.TestCase):
    def test_recovery_and_timeout_paths(self):
        sdk = Path(os.environ.get('ANDROID_HOME', str(Path(os.environ['LOCALAPPDATA']) / 'Android/Sdk')))
        candidates = sorted((sdk / 'sources').glob('android-*/org/json'), reverse=True)
        if not candidates:
            self.skipTest('Android JSON sources unavailable')
        compiler, runtime = java_tools()
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            sources = [ROOT / 'src/dev/pgrazor/SettingsBackupRegression.java']
            for name, content in STUBS.items():
                p = work / name
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_text(content, encoding='utf-8')
                sources.append(p)
            for original in candidates[0].glob('*.java'):
                text = original.read_text(encoding='utf-8')
                text = re.sub(r'^import (?:static )?(?:android\.|libcore\.).*;\s*$', '', text, flags=re.M)
                text = re.sub(r'@(?:UnsupportedAppUsage|SystemApi|NonNull|Nullable)(?:\([^)]*\))?\s*', '', text)
                p = work / original.name
                p.write_text(text, encoding='utf-8')
                sources.append(p)
            result = subprocess.run([str(compiler),'-encoding','UTF-8','-d',str(work),*map(str,sources)],capture_output=True,text=True,timeout=30)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            result = subprocess.run([str(runtime),'-cp',str(work),'dev.pgrazor.BackupRegressionTest',str(work/'data')],capture_output=True,text=True,timeout=20)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)


if __name__ == '__main__':
    unittest.main()
