"""Exercise the actual GameLocationDelivery policy on a JVM with narrow Android/game shells.

GameLocationDelivery ports the runtime installLocationDelivery adapter to a compiled Java class that
reaches the game only by reflection. These shells provide fake game classes whose names and reflection
signatures match the runtime (NianticLocationManager, ContextService,
NianticLocationProvider$ProviderListener) plus the small set of framework types the class uses
directly. They do not simulate Android lifecycle; the Java driver posts ticks and callbacks explicitly.
No Android SDK, device, or game binary is required; a JDK is.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

STUBS = {
    'android/content/Context.java': 'package android.content; public class Context {}',
    'android/location/Location.java': '''package android.location;
public class Location {
 public final String provider;
 public double latitude, longitude, altitude, accuracy;
 public float speed, bearing;
 public long time, elapsedRealtimeNanos;
 public Location(String provider){this.provider=provider;}
 public void setLatitude(double v){latitude=v;}
 public void setLongitude(double v){longitude=v;}
 public void setSpeed(float v){speed=v;}
 public void setBearing(float v){bearing=v;}
 public void setAltitude(double v){altitude=v;}
 public void setAccuracy(float v){accuracy=v;}
 public void setTime(long v){time=v;}
 public void setElapsedRealtimeNanos(long v){elapsedRealtimeNanos=v;}
}''',
    'android/os/Looper.java': '''package android.os;
public class Looper {
 public static final Looper main=new Looper();
 public static Looper getMainLooper(){return main;}
 public static Looper myLooper(){return main;}
}''',
    'android/os/Handler.java': '''package android.os;
public class Handler {
 // Posted work runs only when the test drains it explicitly; at most one tick is queued at a time.
 public final java.util.List<Runnable> pending=new java.util.ArrayList<>();
 public Handler(Looper l){}
 public boolean post(Runnable r){pending.add(r);return true;}
 public boolean postDelayed(Runnable r,long delay){pending.add(r);return true;}
 public void removeCallbacks(Runnable r){pending.removeIf(x->x==r);}
 public void runNext(){if(!pending.isEmpty())pending.remove(0).run();}
}''',
    'android/os/SystemClock.java': '''package android.os;
public class SystemClock {
 public static final long NANOS=123456789L;
 public static long elapsedRealtimeNanos(){return NANOS;}
}''',
    # Test-only Control: coords/activity inputs and an events log the driver inspects. The real
    # Control is intentionally excluded so this test isolates GameLocationDelivery's reflection policy.
    'dev/pgrazor/Control.java': '''package dev.pgrazor;
public final class Control {
 public static volatile boolean active=true;
 public static volatile double lat=1.0, lon=1.0;
 public static volatile float spd=0f, brg=0f;
 public static final java.util.List<String> events=new java.util.ArrayList<>();
 public static boolean active(){return active;}
 public static double latitude(){return lat;}
 public static double longitude(){return lon;}
 public static float speed(){return spd;}
 public static float bearing(){return brg;}
 public static void event(String kind,String detail){events.add(kind+"|"+detail);}
}''',
    # Fake game classes: exact names and reflection signatures the production class resolves. The
    # manager asserts its own delivery invariants (status 3, non-null context, callbackLock held).
    'com/nianticlabs/nia/contextservice/ContextService.java': '''package com.nianticlabs.nia.contextservice;
public class ContextService {
 public static final android.os.Handler handler=new android.os.Handler(android.os.Looper.getMainLooper());
 public static android.os.Handler getServiceHandler(){return handler;}
 public android.content.Context context=new android.content.Context();
 public android.content.Context getContext(){return context;}
}''',
    'com/nianticlabs/nia/location/NianticLocationProvider.java': '''package com.nianticlabs.nia.location;
public class NianticLocationProvider {
 public interface ProviderListener {
  void onProviderStatus(int status);
  void onProviderLocation(android.location.Location fix);
  void onProviderSatellites(long time,int satellites);
 }
 private final String name;
 private ProviderListener providerListener;
 public boolean failSetListener;
 public Runnable onSetListener;
 public int setCount;
 public NianticLocationProvider(String name,ProviderListener listener){this.name=name;this.providerListener=listener;}
 public String getProviderName(){return name;}
 public ProviderListener getListener(){return providerListener;}
 public void setListener(ProviderListener l){
  setCount++;
  if(failSetListener)throw new RuntimeException("setListener blocked");
  if(onSetListener!=null){Runnable r=onSetListener;onSetListener=null;r.run();}
  this.providerListener=l;
 }
 @Override public boolean equals(Object o){return o instanceof NianticLocationProvider&&((NianticLocationProvider)o).name.equals(name);}
 @Override public int hashCode(){return name.hashCode();}
}''',
    'com/nianticlabs/nia/location/NianticLocationManager.java': '''package com.nianticlabs.nia.location;
public class NianticLocationManager extends com.nianticlabs.nia.contextservice.ContextService {
 public java.util.Map<String,NianticLocationProvider> providers=new java.util.LinkedHashMap<>();
 public final Object callbackLock=new Object();
 public long nativeClassPointer=1;
 public boolean failNative;
 public final java.util.List<android.location.Location> delivered=new java.util.ArrayList<>();
 public final java.util.List<String> deliveredProvider=new java.util.ArrayList<>();
 public void nativeLocationUpdate(String provider,android.location.Location fix,int status,android.content.Context context){
  if(!Thread.holdsLock(callbackLock))throw new IllegalStateException("native update not under callbackLock");
  if(status!=3)throw new IllegalStateException("unexpected status "+status);
  if(context==null)throw new IllegalStateException("null context");
  if(failNative)throw new RuntimeException("native failure");
  delivered.add(fix);deliveredProvider.add(provider);
 }
}''',
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
    raise unittest.SkipTest('A JDK is required to run actual GameLocationDelivery policy tests')


class GameLocationDeliveryJavaTests(unittest.TestCase):
    def test_delivery_policy(self):
        compiler, runtime = java_tools()
        with tempfile.TemporaryDirectory(prefix='pgrazor-location-test-') as directory:
            work = Path(directory)
            sources = []
            for relative, text in STUBS.items():
                source = work / relative
                source.parent.mkdir(parents=True, exist_ok=True)
                source.write_text(text, encoding='utf-8')
                sources.append(source)
            sources.append(ROOT / 'src/dev/pgrazor/GameLocationDelivery.java')
            sources.append(ROOT / 'test/GameLocationDeliveryTest.java')
            classes = work / 'classes'
            compiled = subprocess.run([str(compiler), '-encoding', 'UTF-8', '-d', str(classes),
                                       *map(str, sources)], capture_output=True, text=True, timeout=60)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            tested = subprocess.run([str(runtime), '-cp', str(classes), 'dev.pgrazor.GameLocationDeliveryTest'],
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(tested.returncode, 0, tested.stdout + tested.stderr)
            self.assertIn('PASS: GameLocationDelivery', tested.stdout)


if __name__ == '__main__':
    unittest.main()
