"""Exercise the production account restart handoff across a paused/resumed activity gap."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_control_java import java_tools

ROOT = Path(__file__).resolve().parents[1]
STUBS = {
    'android/content/Context.java': '''package android.content;
public class Context { public static final int MODE_PRIVATE=0;
public java.io.File getFilesDir(){return null;}
public SharedPreferences getSharedPreferences(String n,int m){return null;}
public Context getApplicationContext(){return this;}}''',
    'android/content/Intent.java': 'package android.content; public class Intent {}',
    'android/content/SharedPreferences.java': '''package android.content;
public interface SharedPreferences {Editor edit(); interface Editor {boolean commit();}}''',
    'android/app/Application.java': 'package android.app; public class Application extends android.content.Context {}',
    'android/app/Activity.java': '''package android.app; public class Activity extends android.content.Context {
public boolean finishing,destroyed; public boolean isFinishing(){return finishing;}
public boolean isDestroyed(){return destroyed;}}''',
    'android/os/Looper.java': 'package android.os; public class Looper {public static Looper getMainLooper(){return null;}}',
    'android/os/Handler.java': '''package android.os; public class Handler {
public static final java.util.ArrayDeque<Runnable> queue=new java.util.ArrayDeque<>();
public Handler(Looper l){} public boolean post(Runnable r){queue.add(r);return true;}
public boolean postDelayed(Runnable r,long ms){if(ms!=250&&ms!=20000)throw new AssertionError(ms);queue.add(r);return true;}
public static void next(){queue.remove().run();}}''',
    'android/os/StrictMode.java': '''package android.os; public class StrictMode {
public static class ThreadPolicy{} public static ThreadPolicy allowThreadDiskWrites(){return null;}
public static void setThreadPolicy(ThreadPolicy p){}}''',
    'dev/pgrazor/Control.java': '''package dev.pgrazor; public class Control {
static android.app.Activity activity; static java.util.List<String> events=new java.util.ArrayList<>();
static android.app.Activity accountActivity(){return activity;}
static void event(String k,String d){events.add(k+":"+d);}
static void flushAccountWrites() throws java.io.IOException{}
static void quiesceAccounts() throws java.io.IOException{}}''',
    'dev/pgrazor/AccountProfileStore.java': '''package dev.pgrazor; class AccountProfileStore {
static class Profile {String id="default",label="Default";}
AccountProfileStore(java.io.File f){} Profile selected(){return new Profile();}
void cancelPending() throws java.io.IOException{} String pendingId(){return "";}
Profile get(String id){return new Profile();} Profile observeIdentity(String id,String trainer){return new Profile();}
void rename(String id,String name){} void selectVerified(String id){} void activatePending(){}
void prepareSwitch(String id){} Profile add(String name){return new Profile();}
java.io.File dataFile(String id,String name) throws java.io.IOException{return null;}
String preferenceName(String id,String scope){return scope;}}''',
    'dev/pgrazor/AccountSessions.java': '''package dev.pgrazor; class AccountSessions {
static void deleteProfile(android.content.Context c,AccountProfileStore s,String id){}}''',
    'dev/pgrazor/AccountRestartActivity.java': '''package dev.pgrazor; class AccountRestartActivity {
static int launches; static boolean result=true,lastSwitch;
static boolean launch(android.app.Activity a,boolean s){launches++;lastSwitch=s;return result;}}''',
    'dev/pgrazor/RestartLifecycleTest.java': '''package dev.pgrazor;
import android.os.Handler; import android.app.Activity; import java.lang.reflect.*;
public class RestartLifecycleTest {
static void require(boolean ok){if(!ok)throw new AssertionError();}
static void field(String name,boolean value)throws Exception{Field f=AccountProfiles.class.getDeclaredField(name);f.setAccessible(true);f.set(null,value);}
static void restart(boolean session)throws Exception{Method m=AccountProfiles.class.getDeclaredMethod("restart",boolean.class);m.setAccessible(true);m.invoke(null,session);}
static void reset()throws Exception{field("initialized",true);field("verified",false);field("switching",true);field("restarting",false);Handler.queue.clear();Control.activity=null;Control.events.clear();AccountRestartActivity.launches=0;AccountRestartActivity.result=true;}
public static void main(String[] args)throws Exception{
reset();restart(true);require(!AccountProfiles.allowed()&&AccountRestartActivity.launches==0&&Handler.queue.size()==1);
restart(false);require(Handler.queue.size()==1); // duplicate request cannot replace session intent
Control.activity=new Activity();Handler.next();require(AccountRestartActivity.launches==1&&AccountRestartActivity.lastSwitch&&Handler.queue.isEmpty());
require(!AccountProfiles.allowed()); // only the fresh process may verify the new namespace
reset();restart(false);for(int i=0;i<20;i++)Handler.next();
require(Handler.queue.isEmpty()&&AccountRestartActivity.launches==0&&AccountProfiles.status().contains("Close and reopen"));
require(!AccountProfiles.allowed()&&Control.events.size()==2); // bounded retry/logging
reset();Control.activity=new Activity();Control.activity.destroyed=true;restart(false);
require(AccountRestartActivity.launches==0);Control.activity=new Activity();Handler.next();require(AccountRestartActivity.launches==1);
reset();Control.activity=new Activity();AccountRestartActivity.result=false;restart(false);
require(AccountRestartActivity.launches==1&&Handler.queue.isEmpty()&&!AccountProfiles.allowed()&&AccountProfiles.status().contains("Close and reopen"));
reset();Control.activity=new Activity();restart(false);require(AccountRestartActivity.launches==1&&!AccountRestartActivity.lastSwitch&&Handler.queue.isEmpty());
reset();restart(true);Field generation=AccountProfiles.class.getDeclaredField("generation");generation.setAccessible(true);generation.setLong(null,generation.getLong(null)+1);
Control.activity=new Activity();Handler.next();require(AccountRestartActivity.launches==0&&Handler.queue.isEmpty()&&!AccountProfiles.allowed());
reset();restart(false);generation.setLong(null,generation.getLong(null)+1);restart(true);require(Handler.queue.size()==2);
Handler.next();Field restarting=AccountProfiles.class.getDeclaredField("restarting");restarting.setAccessible(true);require(restarting.getBoolean(null));
Control.activity=new Activity();Handler.next();require(AccountRestartActivity.launches==1&&AccountRestartActivity.lastSwitch&&Handler.queue.isEmpty());
System.out.println("PASS account restart lifecycle: resume gap, duplicate request, timeout, destroyed activity, unavailable helper, immediate handoff");
}}
''',
}


class AccountRestartLifecycleTests(unittest.TestCase):
    def test_restart_during_activity_transition(self):
        javac, java = java_tools()
        with tempfile.TemporaryDirectory(prefix='pgrazor-restart-lifecycle-') as directory:
            temp = Path(directory)
            sources = []
            for name, content in STUBS.items():
                path = temp / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content, encoding='utf-8')
                sources.append(path)
            sources += [ROOT / 'src/dev/pgrazor/AccountProfiles.java', ROOT / 'src/dev/pgrazor/AccountCommandState.java']
            result = subprocess.run([str(javac), '-encoding', 'UTF-8', '-d', directory, *map(str, sources)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(java), '-cp', directory, 'dev.pgrazor.RestartLifecycleTest'], capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('PASS account restart lifecycle', result.stdout)
