"""Exercise actual Control feature policy on a JVM, with narrow Android API shells.

These shells do not simulate Android UI/lifecycle behavior. Tests drive foreground
and clock inputs explicitly; device tests remain responsible for scrolling/touch.
Temporary compiler outputs are removed automatically. No Android SDK/device needed.
"""
import os
import re
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
STUBS = {
    'dev/pgrazor/events/EventSettings.java': 'package dev.pgrazor.events; public final class EventSettings {public static boolean boxCollectorEnabled(android.app.Application app){return false;} public static void publishBoxCollectorCapability(boolean value){}}',
    'android/content/Context.java': 'package android.content; public class Context {public static final String NOTIFICATION_SERVICE="notification";}',
    'android/app/Activity.java': '''package android.app;
public class Activity extends android.content.Context {
 public boolean finishing, destroyed;
 public boolean isFinishing(){return finishing;} public boolean isDestroyed(){return destroyed;}
}''',
    'android/app/Application.java': '''package android.app;
public class Application {
 public java.io.File getFilesDir(){throw new UnsupportedOperationException("Files not exercised");}
 public java.io.File getExternalFilesDir(String type){return null;}
 public int checkSelfPermission(String permission){return 0;}
 public Object getSystemService(String name){return null;}
 public void registerActivityLifecycleCallbacks(ActivityLifecycleCallbacks c){}
 public interface ActivityLifecycleCallbacks {
  void onActivityResumed(Activity a); void onActivityPaused(Activity a); void onActivityDestroyed(Activity a);
  void onActivityCreated(Activity a,android.os.Bundle b); void onActivityStarted(Activity a);
  void onActivityStopped(Activity a); void onActivitySaveInstanceState(Activity a,android.os.Bundle b);
 }
}''',
    'android/os/Bundle.java': 'package android.os; public class Bundle {}',
    'android/os/Looper.java': 'package android.os; public class Looper {public static Looper getMainLooper(){return new Looper();}}',
    'android/os/Handler.java': '''package android.os;
public class Handler {
 // Posted work runs only when a test drains it explicitly.
 public static final java.util.List<Runnable> queue=new java.util.ArrayList<>();
 public static boolean accept=true;
 public Handler(Looper l){}
 public boolean post(Runnable r){if(accept)queue.add(r);return accept;}
 public static void drain(){while(!queue.isEmpty())queue.remove(0).run();}
}''',
    'android/widget/Toast.java': '''package android.widget;
public class Toast {
 public static final int LENGTH_SHORT=0, LENGTH_LONG=1;
 public static final java.util.List<Toast> made=new java.util.ArrayList<>();
 public static boolean fail;
 public final android.content.Context context; public final CharSequence text; public boolean shown, cancelled;
 private Toast(android.content.Context c,CharSequence t){context=c;text=t;}
 public static Toast makeText(android.content.Context c,CharSequence t,int d){
  if(fail)throw new IllegalStateException("toast unavailable");
  if(c==null)throw new NullPointerException("context");
  Toast toast=new Toast(c,t);made.add(toast);return toast;
 }
 public void show(){shown=true;} public void cancel(){cancelled=true;}
}''',
    'android/os/SystemClock.java': '''package android.os;
public class SystemClock {public static long now=1000; public static long elapsedRealtime(){return now;}}''',
    'android/os/StrictMode.java': '''package android.os;
public class StrictMode {
 public static class ThreadPolicy {}
 public static ThreadPolicy allowThreadDiskWrites(){return new ThreadPolicy();}
 public static ThreadPolicy allowThreadDiskReads(){return new ThreadPolicy();}
 public static void setThreadPolicy(ThreadPolicy p){}
}''',
    'android/util/Log.java': '''package android.util;
public class Log {
 public static final java.util.List<String> lines=new java.util.ArrayList<>();
 public static int i(String tag,String text){lines.add(text);return 0;}
 public static int e(String tag,String text,Throwable error){throw new AssertionError(text,error);}
}''',
    'android/util/AtomicFile.java': '''package android.util;
public class AtomicFile {
 private final java.io.File file; public AtomicFile(java.io.File file){this.file=file;}
 public java.io.FileOutputStream startWrite() throws java.io.IOException {return new java.io.FileOutputStream(file);}
 public java.io.FileInputStream openRead() throws java.io.IOException {return new java.io.FileInputStream(file);}
 public void finishWrite(java.io.FileOutputStream out){try{out.close();}catch(java.io.IOException e){throw new RuntimeException(e);}} public void failWrite(java.io.FileOutputStream out){}
}''',
    'android/os/Build.java': '''package android.os;
public class Build {public static class VERSION {public static final int SDK_INT=34;}}''',
    'android/content/pm/PackageManager.java': '''package android.content.pm;
public class PackageManager {public static final int PERMISSION_GRANTED=0;}''',
    'android/view/ViewConfiguration.java': '''package android.view;
public class ViewConfiguration {public static int getDoubleTapTimeout(){return 300;}}''',
    'android/R.java': '''package android;
public final class R {public static final class drawable {public static final int stat_notify_sync=0x1080000;}}''',
    'android/app/NotificationChannel.java': '''package android.app;
public class NotificationChannel {public NotificationChannel(String id,CharSequence name,int importance){}}''',
    'android/app/Notification.java': '''package android.app;
public class Notification {
 public static class Builder {
  public Builder(Application context,String channelId){}
  public Builder setSmallIcon(int icon){return this;}
  public Builder setContentTitle(CharSequence title){return this;}
  public Builder setContentText(CharSequence text){return this;}
  public Builder setAutoCancel(boolean value){return this;}
  public Notification build(){return new Notification();}
 }
}''',
    'android/app/NotificationManager.java': '''package android.app;
public class NotificationManager {
 public static final int IMPORTANCE_LOW=2;
 public void createNotificationChannel(NotificationChannel channel){}
 public void notify(int id,Notification notification){}
}''',
    'dev/pgrazor/AccountProfiles.java': """package dev.pgrazor;
public class AccountProfiles {
 public static void init(android.app.Application app){}
 public static boolean allowed(){return true;}
 public static java.io.File file(android.app.Application app,String name){return new java.io.File(app.getFilesDir(),name);}
 public static String command(){return "";}
 public static String[] commandSnapshot(){return new String[]{"", "0"};}
 public static void identity(String id,String name){}
 public static void support(boolean ready){}
 public static void sessionEnded(){}
 public static void logoutComplete(){}
 public static void logoutFailed(){} static void signInResetComplete(){} static void sessionSaved(){}
}""",
    'dev/pgrazor/Pad.java': '''package dev.pgrazor;
public class Pad {public final android.app.Activity activity; public Pad(android.app.Activity a){activity=a;} public void close(){} public void confirmStopRoute(){} public void confirmMapTeleport(double lat,double lon,int mode,int epoch){}}''',
    'dev/pgrazor/LogActivity.java': 'package dev.pgrazor; public class LogActivity extends android.app.Activity {}',
    # Shells for collaborators whose real sources pull in Android UI/network far beyond Control's
    # narrow use of them. Control only references the members shelled here; policy is never exercised
    # through these, so they return inert values. BerryPriority/MapTapSequence/CooldownState compile
    # from real source below because they are pure Java with exercised behavior.
    'dev/pgrazor/Cooldown.java': '''package dev.pgrazor;
public final class Cooldown {
 // Tests drive the estimated cooldown explicitly; defaults match the previous inert shell.
 public static long remaining=0;
 public static final java.util.List<String> recorded=new java.util.ArrayList<>();
 public static void init(android.app.Application app){}
 public static void flush() throws java.io.IOException {}
 public static long remaining(){return remaining;}
 public static void record(String action){recorded.add(action);}
}''',
    'dev/pgrazor/GoogleBrowserAuth.java': '''package dev.pgrazor;
public final class GoogleBrowserAuth {
 public static void onHostResumed(android.app.Activity activity){}
 public static void onHostPaused(android.app.Activity activity){}
}''',
    'dev/pgrazor/GoogleAuthActivity.java': 'package dev.pgrazor; public class GoogleAuthActivity extends android.app.Activity {}',
}


def java_tools():
    candidates = []
    if os.environ.get('JAVA_HOME'):
        candidates.append(Path(os.environ['JAVA_HOME']) / 'bin')
    found = shutil.which('javac')
    if found:
        candidates.append(Path(found).parent)
    if os.name == 'nt':
        candidates.extend(sorted(Path('C:/Program Files/Eclipse Adoptium').glob('jdk-*/bin'), reverse=True))
    suffix = '.exe' if os.name == 'nt' else ''
    for folder in candidates:
        compiler, runtime = folder / ('javac' + suffix), folder / ('java' + suffix)
        if compiler.is_file() and runtime.is_file():
            return compiler, runtime
    raise unittest.SkipTest('A JDK is required to run actual Control policy tests')


class ControlJavaTests(unittest.TestCase):
    def test_quick_summary_policy_and_location_isolation(self):
        compiler, runtime = java_tools()
        with tempfile.TemporaryDirectory(prefix='pgrazor-control-test-') as directory:
            work = Path(directory)
            sources = []
            for relative, text in STUBS.items():
                source = work / relative
                source.parent.mkdir(parents=True, exist_ok=True)
                source.write_text(text, encoding='utf-8')
                sources.append(source)
            sources.extend(ROOT / 'src/dev/pgrazor' / name for name in (
                'Control.java', 'MotionEngine.java', 'LocationState.java', 'GameRoutesController.java',
                'CameraHeadingState.java', 'CameraSteering.java',
                'BerryPriority.java', 'MapTapSequence.java', 'CooldownState.java',
                'VgpController.java', 'CatchOutcomeController.java', 'SettingsWriter.java'))
            sources.extend([ROOT / 'test/ControlQuickSummaryTest.java',ROOT / 'test/ControlQuickBallTest.java',
                            ROOT / 'test/ControlQuickBallOutcomeTest.java',ROOT / 'test/ControlSettingsTest.java',
                            ROOT / 'test/VgpControllerTest.java',ROOT / 'test/CooldownTest.java',ROOT / 'test/ControlCooldownTest.java',
                            ROOT / 'test/ControlSettingsReloadTest.java',ROOT / 'test/ControlItemCatalogTest.java'])
            classes = work / 'classes'
            sdk = Path(os.environ.get('ANDROID_HOME', str(Path(os.environ['LOCALAPPDATA']) / 'Android/Sdk')))
            json_sources = sorted((sdk / 'sources').glob('android-*/org/json'), reverse=True)
            if not json_sources:
                self.skipTest('Android SDK JSON sources unavailable')
            for original in json_sources[0].glob('*.java'):
                text = original.read_text(encoding='utf-8')
                text = re.sub(r'^import (?:static )?(?:android\.|libcore\.).*;\s*$', '', text, flags=re.M)
                text = re.sub(r'@(?:UnsupportedAppUsage|SystemApi|NonNull|Nullable)(?:\([^)]*\))?\s*', '', text)
                target = work / original.name
                target.write_text(text, encoding='utf-8')
                sources.append(target)
            compiled = subprocess.run([str(compiler), '-encoding', 'UTF-8', '-d', str(classes),
                                       *map(str, sources)], capture_output=True, text=True, timeout=30)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            tested = subprocess.run([str(runtime), '-cp', str(classes), 'dev.pgrazor.ControlQuickSummaryTest'],
                                    capture_output=True, text=True, timeout=15)
            self.assertEqual(tested.returncode, 0, tested.stdout + tested.stderr)
            self.assertIn('PASS: real Control summary', tested.stdout)
            tested = subprocess.run([str(runtime), '-cp', str(classes), 'dev.pgrazor.ControlQuickBallTest'],
                                    capture_output=True, text=True, timeout=15)
            self.assertEqual(tested.returncode, 0, tested.stdout + tested.stderr)
            self.assertIn('PASS: real Control ball', tested.stdout)
            tested = subprocess.run([str(runtime), '-cp', str(classes), 'dev.pgrazor.ControlQuickBallOutcomeTest'],
                                    capture_output=True, text=True, timeout=15)
            self.assertEqual(tested.returncode, 0, tested.stdout + tested.stderr)
            self.assertIn('PASS: real Control ball outcome', tested.stdout)
            tested = subprocess.run([str(runtime), '-cp', str(classes), 'dev.pgrazor.VgpControllerTest'],
                                    capture_output=True, text=True, timeout=15)
            self.assertEqual(tested.returncode, 0, tested.stdout + tested.stderr)
            self.assertIn('PASS: real Control VGP', tested.stdout)
            tested = subprocess.run([str(runtime), '-cp', str(classes), 'dev.pgrazor.CooldownTest'],
                                    capture_output=True, text=True, timeout=15)
            self.assertEqual(tested.returncode, 0, tested.stdout + tested.stderr)
            self.assertIn('PASS cooldown', tested.stdout)
            tested = subprocess.run([str(runtime), '-cp', str(classes), 'dev.pgrazor.ControlCooldownTest'],
                                    capture_output=True, text=True, timeout=15)
            self.assertEqual(tested.returncode, 0, tested.stdout + tested.stderr)
            tested = subprocess.run([str(runtime), '-cp', str(classes), 'dev.pgrazor.ControlItemCatalogTest'],
                                    capture_output=True, text=True, timeout=15)
            self.assertEqual(tested.returncode, 0, tested.stdout + tested.stderr)
            for phase in ('save','load'):
                tested=subprocess.run([str(runtime),'-cp',str(classes),'dev.pgrazor.ControlSettingsTest',phase,str(work)],capture_output=True,text=True,timeout=15)
                self.assertEqual(tested.returncode,0,tested.stdout+tested.stderr)
            tested = subprocess.run([str(runtime), '-cp', str(classes), 'dev.pgrazor.ControlSettingsReloadTest',str(work)],
                                    capture_output=True, text=True, timeout=15)
            self.assertEqual(tested.returncode, 0, tested.stdout + tested.stderr)


if __name__ == '__main__':
    unittest.main()
