"""Independent actual-class auth lifecycle/queue regressions; no browser/network/credentials."""
import os
import pathlib
import re
import shutil
import subprocess
import tempfile
import unittest

STUBS = {
    'android/app/Activity.java': 'package android.app; public class Activity {}',
    'android/os/Looper.java': '''package android.os; public class Looper {
 static final Thread OWNER=Thread.currentThread();static final Looper MAIN=new Looper();
 public static Looper getMainLooper(){return MAIN;}public static Looper myLooper(){return Thread.currentThread()==OWNER?MAIN:null;}
 public Thread getThread(){return OWNER;} }''',
    'android/os/Handler.java': '''package android.os; import java.util.*;public class Handler {
 public static final List<Runnable> queue=new ArrayList<>();public static boolean acceptPost=true,acceptDelayed=true;
 public Handler(Looper l){}public Looper getLooper(){return Looper.getMainLooper();}
 public boolean post(Runnable r){if(!acceptPost)return false;queue.add(r);return true;}
 public boolean postDelayed(Runnable r,long ms){if(ms!=250)throw new AssertionError("250ms poll");if(!acceptDelayed)return false;queue.add(r);return true;}
 public void removeCallbacks(Runnable r){queue.removeIf(x->x==r);}
 public static void one(){if(queue.isEmpty())throw new AssertionError("expected queued work");queue.remove(0).run();}
 public static void drain(){int bound=30;while(!queue.isEmpty()){if(--bound==0)throw new AssertionError("unbounded queued work");one();}}
 }''',
    'kotlin/jvm/functions/Function1.java': 'package kotlin.jvm.functions;public interface Function1 {Object invoke(Object value);}',
    'com/nianticlabs/platform/authgooglesignin/SignInResponse.java': '''package com.nianticlabs.platform.authgooglesignin;
 public class SignInResponse {public final String status,email,idToken;public SignInResponse(String s,String e,String t){status=s;email=e;idToken=t;}}''',
    'dev/pgrazor/Control.java': '''package dev.pgrazor;import java.util.*;public class Control {
 public static final List<String> logs=new ArrayList<>();public static void event(String k,String v){logs.add(k+":"+v);} }''',
    'dev/pgrazor/GoogleBrowserAuth.java': '''package dev.pgrazor;public class GoogleBrowserAuth {
 public static int begins,cancels,takes;public static String current,result,id;public static boolean overrideId;public static Runnable onTake;
 static void main(){if(android.os.Looper.myLooper()!=android.os.Looper.getMainLooper())throw new AssertionError("foreign browser operation");}
 public static String begin(android.app.Activity a){main();begins++;current=overrideId?id:"request-"+begins;return current;}
 public static String takeResult(String requested){main();takes++;String raw=result;Runnable cb=onTake;onTake=null;if(cb!=null)cb.run();
  if(current==null||!current.equals(requested))return null;if(raw!=null){current=null;result=null;}return raw;}
 public static void cancel(String requested){main();cancels++;if(current!=null&&current.equals(requested)&&result==null)result="{\\"status\\":\\"Cancelled\\"}";}
 }''',
}

HARNESS = r'''package dev.pgrazor;
import android.app.Activity;import android.os.*;import java.util.*;import java.util.concurrent.atomic.AtomicReference;
import com.nianticlabs.platform.authgooglesignin.SignInResponse;import kotlin.jvm.functions.Function1;
public class GoogleSignInReentryRegression {
 static class Consumer implements Function1 {
  final List<SignInResponse> responses=new ArrayList<>();Runnable callback;
  public Object invoke(Object response){check(Looper.myLooper()==Looper.getMainLooper(),"consumer must run on main");
   responses.add((SignInResponse)response);if(callback!=null)callback.run();return null;}
  void only(String status){check(responses.size()==1,"exactly one consumer callback");SignInResponse r=responses.get(0);
   check(status.equals(r.status),"expected "+status+" response");check(r.email==null&&r.idToken==null,"no credentials on non-success");}
 }
 static void check(boolean ok,String why){if(!ok)throw new AssertionError(why);}
 static void begin(GameGoogleSignIn helper,Consumer c){helper.signIn(new Activity(),c);Handler.one();check(GoogleBrowserAuth.begins==1,"first browser begin");}
 public static void main(String[] args) throws Exception {
  GameGoogleSignIn helper=GameGoogleSignIn.create();Consumer first=new Consumer();
  switch(args[0]){
  case "replacement-shutdown":{
   begin(helper,first);first.callback=helper::shutdown;Consumer next=new Consumer();helper.signIn(new Activity(),next);
   // The old poll precedes the queued replacement once; it sees no result and reposts.
   Handler.one();Handler.one();
   check(GoogleBrowserAuth.begins==1,"old Cancelled callback shutdown must prevent second browser begin");
   Handler.drain();first.only("Cancelled");next.only("Cancelled");break;
  }
  case "take-shutdown":{
   begin(helper,first);GoogleBrowserAuth.result="{\"status\":\"Success\",\"email\":\"fake-email-marker\",\"idToken\":\"fake-token-marker\"}";
   GoogleBrowserAuth.onTake=helper::shutdown;Handler.one();Handler.drain();first.only("Cancelled");break;
  }
  case "initial-delay-rejected":{
   helper.signIn(new Activity(),first);Handler.acceptDelayed=false;Handler.one();
   first.only("Fail");check(GoogleBrowserAuth.current==null&&GoogleBrowserAuth.cancels==1,"failed initial poll cancels/drains");
   check(Handler.queue.isEmpty(),"no orphan poll");break;
  }
  case "repost-rejected":{
   begin(helper,first);Handler.acceptDelayed=false;Handler.one();first.only("Fail");
   check(GoogleBrowserAuth.current==null&&GoogleBrowserAuth.cancels==1,"failed repost cancels/drains");check(Handler.queue.isEmpty(),"no orphan poll");break;
  }
  case "shutdown-post-rejected":{
   begin(helper,first);Handler.acceptPost=false;helper.shutdown();
   if(!Handler.queue.isEmpty())Handler.one();first.only("Cancelled");
   check(GoogleBrowserAuth.current==null&&Handler.queue.isEmpty(),"main-thread rejected shutdown post cleans immediately or via poll");break;
  }
  case "foreign-shutdown-post-rejected":{
   begin(helper,first);Handler.acceptPost=false;AtomicReference<Throwable> thrown=new AtomicReference<>();
   Thread thread=new Thread(()->{try{helper.shutdown();}catch(Throwable error){thrown.set(error);}},"auth-shutdown-host");
   thread.start();thread.join(3000);check(!thread.isAlive()&&thrown.get()==null,"bounded foreign shutdown");
   check(first.responses.isEmpty()&&GoogleBrowserAuth.current!=null,"foreign shutdown performs no browser/callback operation");
   Handler.one();first.only("Cancelled");
   check(GoogleBrowserAuth.current==null&&Handler.queue.isEmpty(),"existing main poll drains rejected foreign shutdown post");break;
  }
  case "foreign-post-rejected":{
   Handler.acceptPost=false;AtomicReference<Throwable> thrown=new AtomicReference<>();
   Thread thread=new Thread(()->{try{helper.signIn(new Activity(),first);}catch(Throwable error){thrown.set(error);}},"auth-host");
   thread.start();thread.join(3000);check(!thread.isAlive(),"bounded host call");
   check(thrown.get() instanceof IllegalStateException,"rejected foreign queue must signal explicit constant failure");
   String message=thrown.get().getMessage();check(message!=null&&!message.isEmpty()&&!message.contains("fake-"),"constant failure message");
   check(first.responses.isEmpty()&&GoogleBrowserAuth.begins==0&&Handler.queue.isEmpty(),"never callback/browser on foreign rejected queue");break;
  }
  case "null-id":case "empty-id":{
   GoogleBrowserAuth.overrideId=true;GoogleBrowserAuth.id=args[0].equals("null-id")?null:"";
   helper.signIn(new Activity(),first);Handler.one();first.only("Fail");check(Handler.queue.isEmpty(),"invalid id must not poll forever");break;
  }
  default:throw new AssertionError("unknown case");
  }
  Handler.acceptPost=true;Handler.acceptDelayed=true;helper.shutdown();Handler.drain();
  for(String log:Control.logs)check(!log.contains("fake-email-marker")&&!log.contains("fake-token-marker"),"logs never contain result material");
  System.out.println("PASS "+args[0]);
 }
}
'''


class GameGoogleSignInReentryTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.javac, cls.java = shutil.which('javac'), shutil.which('java')
        if not cls.javac or not cls.java:
            raise unittest.SkipTest('javac/java unavailable')
        sdk = pathlib.Path(os.environ.get('ANDROID_HOME', str(pathlib.Path(os.environ.get('LOCALAPPDATA', '')) / 'Android/Sdk')))
        json_dirs = sorted((sdk / 'sources').glob('android-*/org/json'), reverse=True)
        if not json_dirs:
            raise unittest.SkipTest('Android SDK JSON sources unavailable')
        cls.directory = tempfile.TemporaryDirectory(prefix='pgrazor-google-reentry-')
        cls.root = pathlib.Path(cls.directory.name)
        for relative, content in STUBS.items():
            target = cls.root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(content, encoding='utf-8')
        for original in json_dirs[0].glob('*.java'):
            content = original.read_text(encoding='utf-8')
            content = re.sub(r'^import (?:static )?(?:android\.|libcore\.).*;\s*$', '', content, flags=re.M)
            content = re.sub(r'@(?:UnsupportedAppUsage|SystemApi|NonNull|Nullable)(?:\([^)]*\))?\s*', '', content)
            (cls.root / original.name).write_text(content, encoding='utf-8')
        production = pathlib.Path(__file__).resolve().parents[1] / 'src/dev/pgrazor/GameGoogleSignIn.java'
        shutil.copyfile(production, cls.root / 'dev/pgrazor/GameGoogleSignIn.java')
        (cls.root / 'dev/pgrazor/GoogleSignInReentryRegression.java').write_text(HARNESS, encoding='utf-8')
        result = subprocess.run([cls.javac, '-encoding', 'UTF-8', '-d', str(cls.root)] +
                                [str(p) for p in cls.root.rglob('*.java')], capture_output=True, text=True, timeout=60)
        if result.returncode:
            cls.directory.cleanup()
            raise AssertionError(result.stdout + result.stderr)

    @classmethod
    def tearDownClass(cls):
        if hasattr(cls, 'directory'):
            cls.directory.cleanup()

    def run_case(self, case):
        result = subprocess.run([self.java, '-cp', str(self.root), 'dev.pgrazor.GoogleSignInReentryRegression', case],
                                capture_output=True, text=True, timeout=12)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_replacement_shutdown(self): self.run_case('replacement-shutdown')
    def test_take_shutdown(self): self.run_case('take-shutdown')
    def test_initial_delay_rejected(self): self.run_case('initial-delay-rejected')
    def test_repost_rejected(self): self.run_case('repost-rejected')
    def test_shutdown_post_rejected(self): self.run_case('shutdown-post-rejected')
    def test_foreign_shutdown_post_rejected(self): self.run_case('foreign-shutdown-post-rejected')
    def test_foreign_post_rejected(self): self.run_case('foreign-post-rejected')
    def test_null_id(self): self.run_case('null-id')
    def test_empty_id(self): self.run_case('empty-id')


if __name__ == '__main__':
    unittest.main()
