"""Exercise actual dispatch + all three actual helpers using existing offline JVM shells.

No DEX rewriting, Android lifecycle instrumentation, browser networking or device is simulated.
Each scenario gets a fresh JVM because dispatch shutdown is intentionally process-terminal.
"""
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_control_java import java_tools
from test_game_google_signin_java import STUBS as GOOGLE
from test_game_location_delivery_java import STUBS as DELIVERY
from test_game_location_permissions_java import STUBS as PERMISSIONS

ROOT = Path(__file__).resolve().parents[1]


def stubs():
    result = {**GOOGLE, **PERMISSIONS, **DELIVERY}
    result['android/os/Looper.java'] = '''package android.os;
public final class Looper {
 private static final Looper MAIN=new Looper(),OTHER=new Looper();
 private static final ThreadLocal<Boolean> main=ThreadLocal.withInitial(()->true);
 public static Looper getMainLooper(){return MAIN;}
 public static Looper myLooper(){return main.get()?MAIN:OTHER;}
 public static void setMain(boolean value){main.set(value);}
}'''
    result['android/os/Handler.java'] = '''package android.os;
public class Handler {
 private static final Object LOCK=new Object();
 private static final class Entry {final Handler owner;final Runnable action;final long delay;
  Entry(Handler h,Runnable r,long d){owner=h;action=r;delay=d;}}
 private static final java.util.List<Entry> all=new java.util.ArrayList<>();
 public static boolean accept=true;
 public final java.util.List<Runnable> pending=new java.util.ArrayList<>();
 public Handler(Looper ignored){}
 public boolean post(Runnable action){return postDelayed(action,0);}
 public boolean postDelayed(Runnable action,long delay){synchronized(LOCK){
  if(!accept)return false;all.add(new Entry(this,action,delay));pending.add(action);return true;}}
 public void removeCallbacks(Runnable action){synchronized(LOCK){all.removeIf(e->e.owner==this&&e.action==action);pending.removeIf(r->r==action);}}
 public void runNext(){Entry next=null;synchronized(LOCK){for(Entry e:all)if(e.owner==this){next=e;break;}if(next!=null){all.remove(next);pending.remove(next.action);}}if(next!=null)next.action.run();}
 public static void runGlobalNext(){Entry next=null;synchronized(LOCK){for(Entry e:all)if(next==null||e.delay<next.delay)next=e;if(next!=null){all.remove(next);next.owner.pending.remove(next.action);}}if(next!=null)next.action.run();}
 public static void drain(int limit){for(int i=0;i<limit;i++){synchronized(LOCK){if(all.isEmpty())return;}runGlobalNext();}}
}'''
    result['dev/pgrazor/Control.java'] = '''package dev.pgrazor;
public final class Control {
 public static volatile boolean active=true,desired=true;
 public static volatile double lat=1.0,lon=1.0;public static volatile float spd,brg;
 public static final java.util.List<String> events=java.util.Collections.synchronizedList(new java.util.ArrayList<>());
 public static Runnable onEvent;
 public static boolean active(){return active;}public static boolean desiredEnabled(){return desired;}
 public static double latitude(){return lat;}public static double longitude(){return lon;}
 public static float speed(){return spd;}public static float bearing(){return brg;}
 public static void event(String kind,String detail){events.add(kind+"|"+detail);if(onEvent!=null){Runnable r=onEvent;onEvent=null;r.run();}}
}'''
    result['com/nianticlabs/nia/contextservice/ContextService.java'] = '''package com.nianticlabs.nia.contextservice;
public class ContextService {
 public static final android.os.Handler handler=new android.os.Handler(android.os.Looper.getMainLooper());
 public static int factoryCalls;public static boolean factoryFails;
 public static android.os.Handler getServiceHandler(){factoryCalls++;if(factoryFails)throw new IllegalStateException("fixture-sensitive-failure");return handler;}
 public android.content.Context context=new android.content.Context();
 public android.content.Context getContext(){return context;}
}'''
    return result


class GameBridgeDispatchJavaTests(unittest.TestCase):
    def test_dispatch_and_actual_helpers(self):
        compiler, runtime = java_tools()
        with tempfile.TemporaryDirectory(prefix='pgrazor-game-bridge-') as directory:
            work = Path(directory)
            sources = []
            for relative, text in stubs().items():
                source = work / relative
                source.parent.mkdir(parents=True, exist_ok=True)
                source.write_text(text, encoding='utf-8')
                sources.append(source)
            sources += [ROOT / 'src/dev/pgrazor' / name for name in (
                'GameBridgeDispatch.java', 'GameGoogleSignIn.java',
                'GameLocationDelivery.java', 'GameLocationPermissions.java')]
            sources.append(ROOT / 'test/GameBridgeDispatchTest.java')
            classes = work / 'classes'
            compiled = subprocess.run([str(compiler), '-encoding', 'UTF-8', '-d', str(classes), *map(str, sources)],
                                      capture_output=True, text=True, timeout=60)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            for scenario in ('basic', 'isolation', 'shutdown-during-create', 'queue-failure',
                             'inert-shutdown', 'concurrent-create', 'reentrant-create', 'shutdown-callback',
                             'prepared-service-thread', 'browser-option'):
                with self.subTest(scenario=scenario):
                    tested = subprocess.run([str(runtime), '-ea', '-cp', str(classes),
                                             'dev.pgrazor.GameBridgeDispatchTest', scenario],
                                            capture_output=True, text=True, timeout=20)
                    self.assertEqual(tested.returncode, 0, tested.stdout + tested.stderr)
                    self.assertIn('PASS GameBridgeDispatch ' + scenario, tested.stdout)


if __name__ == '__main__':
    unittest.main()
