"""Independent production delivery regressions; no Android runtime or game binary."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest

STUBS = {
    'android/content/Context.java': 'package android.content; public class Context {}',
    'android/location/Location.java': '''package android.location; public class Location {
 public Location(String p){} public void setLatitude(double v){} public void setLongitude(double v){}
 public void setSpeed(float v){} public void setBearing(float v){} public void setAltitude(double v){}
 public void setAccuracy(float v){} public void setTime(long v){} public void setElapsedRealtimeNanos(long v){} }''',
    'android/os/Looper.java': '''package android.os; public class Looper {
 static final Looper MAIN=new Looper(); static final Thread OWNER=Thread.currentThread();
 public static Looper myLooper(){return Thread.currentThread()==OWNER?MAIN:null;}
 public static Looper getMainLooper(){return MAIN;} public Thread getThread(){return OWNER;} }''',
    'android/os/Handler.java': '''package android.os; import java.util.*; public class Handler {
 public final List<Runnable> pending=new ArrayList<>(); public Looper getLooper(){return Looper.getMainLooper();}
 public synchronized boolean post(Runnable r){pending.add(r);return true;}
 public synchronized boolean postDelayed(Runnable r,long ms){pending.add(r);return true;}
 public synchronized void removeCallbacks(Runnable r){pending.removeIf(x->x==r);}
 public void one(){Runnable r; synchronized(this){if(pending.isEmpty())return;r=pending.remove(0);}r.run();} }''',
    'android/os/SystemClock.java': 'package android.os; public class SystemClock {public static long elapsedRealtimeNanos(){return 1234567L;}}',
    'dev/pgrazor/Control.java': '''package dev.pgrazor; public class Control {
 public static boolean active(){return true;} public static double latitude(){return 1;} public static double longitude(){return 2;}
 public static float speed(){return 3;} public static float bearing(){return 4;} public static void event(String k,String v){} }''',
    'com/nianticlabs/nia/contextservice/ContextService.java': '''package com.nianticlabs.nia.contextservice;
 public class ContextService {public static final android.os.Handler HANDLER=new android.os.Handler();
 public static android.os.Handler getServiceHandler(){return HANDLER;} }''',
    'com/nianticlabs/nia/location/NianticLocationProvider.java': '''package com.nianticlabs.nia.location;
 public class NianticLocationProvider {
 public interface ProviderListener {void onProviderStatus(int s);void onProviderLocation(android.location.Location l);void onProviderSatellites(long t,int n);}
 public ProviderListener providerListener; public final ProviderListener original;
 public Runnable onSet;public boolean failAfter;public int exactCalls;
 public NianticLocationProvider(ProviderListener l){providerListener=original=l;}
 public String getProviderName(){return "gps";}
 public void setListener(ProviderListener l){exactCalls++;providerListener=l;Runnable cb=onSet;onSet=null;if(cb!=null)cb.run();
  if(failAfter){failAfter=false;throw new IllegalStateException("changed-then-threw");}}
 }''',
    'com/nianticlabs/nia/location/NianticLocationManager.java': '''package com.nianticlabs.nia.location;
 import java.util.*; public class NianticLocationManager extends com.nianticlabs.nia.contextservice.ContextService {
 public final Map<Integer,NianticLocationProvider> providers=new LinkedHashMap<>(); public final Object callbackLock=new Object();
 public long nativeClassPointer=42; public volatile int calls; public Runnable onContext;
 public android.content.Context getContext(){Runnable cb=onContext;onContext=null;if(cb!=null)cb.run();return new android.content.Context();}
 private void nativeLocationUpdate(String p,android.location.Location l,int status,android.content.Context c){
  if(!Thread.holdsLock(callbackLock)||status!=3||c==null)throw new AssertionError("native call contract");calls++;}
 }''',
}

HARNESS = r'''package dev.pgrazor;
import android.location.Location;
import com.nianticlabs.nia.location.*;
import com.nianticlabs.nia.contextservice.ContextService;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicReference;
public class LocationReentryRegression {
 static class Recorder implements NianticLocationProvider.ProviderListener {
  volatile int locations;public void onProviderStatus(int s){}public void onProviderLocation(Location l){locations++;}public void onProviderSatellites(long t,int n){}
 }
 // An Object overload in the subclass must not mask the inherited exact listener signature.
 static class Overloaded extends NianticLocationProvider {
  int objectCalls;Overloaded(Recorder r){super(r);}public void setListener(Object o){objectCalls++;throw new AssertionError("Object overload selected");}
 }
 static void check(boolean ok,String why){if(!ok)throw new AssertionError(why);}
 static NianticLocationManager manager(NianticLocationProvider p){NianticLocationManager m=new NianticLocationManager();m.providers.put(1,p);return m;}
 static void tick(){ContextService.HANDLER.one();}
 static void await(CountDownLatch l) throws Exception {check(l.await(3,TimeUnit.SECONDS),"bounded latch timeout");}
 public static void main(String[] args) throws Exception {
  GameLocationDelivery d=GameLocationDelivery.create();Recorder original=new Recorder();
  NianticLocationProvider p=new NianticLocationProvider(original);NianticLocationManager m=manager(p);
  switch(args[0]) {
  case "restore-reentry": {
   d.start(m);tick();Recorder otherOriginal=new Recorder();NianticLocationManager next=manager(new NianticLocationProvider(otherOriginal));
   p.onSet=()->d.start(next);d.stop(m);tick();
   check(next.calls==1,"start(newOwner) during restore must survive outer release");
   check(p.providerListener==original,"old provider restored");break;
  }
  case "setter-mutates-throws": {
   p.failAfter=true;d.start(m);tick();
   check(p.providerListener==original,"setter mutated then threw: proxy must be conditionally restored");
   d.shutdown();check(p.providerListener==original,"failed attach must not retain untracked proxy");break;
  }
  case "retired-lock-waiter": {
   d.start(m);tick();NianticLocationProvider.ProviderListener old=p.providerListener;int before=m.calls;
   CountDownLatch entered=new CountDownLatch(1);AtomicReference<Throwable> failure=new AtomicReference<>();
   Thread callback=new Thread(()->{try{entered.countDown();old.onProviderLocation(new Location("real"));}catch(Throwable t){failure.set(t);}},"retired-provider");
   synchronized(m.callbackLock){
    callback.start();await(entered);
    // Observe the monitor waiter without sleeps; the deadline only bounds a broken fixture.
    long deadline=System.nanoTime()+TimeUnit.SECONDS.toNanos(3);
    while(callback.getState()!=Thread.State.BLOCKED&&callback.isAlive()&&System.nanoTime()<deadline)Thread.yield();
    check(callback.getState()==Thread.State.BLOCKED,"provider must be awaiting callbackLock");
    d.stop(m);d.start(m);
   }
   callback.join(3000);check(!callback.isAlive(),"callback completed");if(failure.get()!=null)throw new AssertionError(failure.get());
   check(m.calls==before,"retired proxy cannot send after SAME owner restart");
   check(original.locations==1,"retired callback forwards original exactly once");break;
  }
  case "context-reentry": {
   d.start(m);tick();int before=m.calls;m.onContext=()->d.stop(m);
   p.providerListener.onProviderLocation(new Location("real"));
   check(m.calls==before,"getContext reentrant stop must prevent native update");
   check(original.locations==1,"stopped during getContext forwards original");break;
  }
  case "exact-overload": {
   Overloaded overloaded=new Overloaded(original);NianticLocationManager overloadedManager=manager(overloaded);d.start(overloadedManager);tick();
   check(overloaded.objectCalls==0,"must never choose setListener(Object)");
   check(overloaded.exactCalls==1&&overloaded.providerListener!=original,"exact inherited interface setter used");break;
  }
  default:throw new AssertionError("unknown case");
  }
  d.shutdown();System.out.println("PASS "+args[0]);
 }
}
'''


class GameLocationReentryTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.javac = shutil.which('javac')
        cls.java = shutil.which('java')
        if not cls.javac or not cls.java:
            raise unittest.SkipTest('javac/java unavailable')
        cls.directory = tempfile.TemporaryDirectory(prefix='pgrazor-location-reentry-')
        cls.root = pathlib.Path(cls.directory.name)
        for relative, content in STUBS.items():
            target = cls.root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(content, encoding='utf-8')
        production = pathlib.Path(__file__).resolve().parents[1] / 'src/dev/pgrazor/GameLocationDelivery.java'
        shutil.copyfile(production, cls.root / 'dev/pgrazor/GameLocationDelivery.java')
        (cls.root / 'dev/pgrazor/LocationReentryRegression.java').write_text(HARNESS, encoding='utf-8')
        result = subprocess.run([cls.javac, '-source', '8', '-target', '8', '-d', str(cls.root)] +
                                [str(p) for p in cls.root.rglob('*.java')], capture_output=True, text=True, timeout=60)
        if result.returncode:
            cls.directory.cleanup()
            raise AssertionError(result.stdout + result.stderr)

    @classmethod
    def tearDownClass(cls):
        if hasattr(cls, 'directory'):
            cls.directory.cleanup()

    def run_case(self, case):
        result = subprocess.run([self.java, '-cp', str(self.root), 'dev.pgrazor.LocationReentryRegression', case],
                                capture_output=True, text=True, timeout=12)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_restore_reentry(self): self.run_case('restore-reentry')
    def test_setter_mutates_throws(self): self.run_case('setter-mutates-throws')
    def test_retired_lock_waiter(self): self.run_case('retired-lock-waiter')
    def test_context_reentry(self): self.run_case('context-reentry')
    def test_exact_overload(self): self.run_case('exact-overload')


if __name__ == '__main__':
    unittest.main()
