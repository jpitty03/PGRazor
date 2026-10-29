"""Compile and rewrite actual DEX fixtures; optionally execute with research-device ART."""
from pathlib import Path
import argparse
import datetime
import json
import subprocess
import sys
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import build
import dex_bridges

SOURCES = {
    'dev/pgrazor/NativeLocationValues.java': '''package dev.pgrazor;
public final class NativeLocationValues {
 private static void receiver(android.location.Location value){if(value==null)throw new NullPointerException("fixture-location");}
 public static double getLatitude(android.location.Location value){receiver(value);return 12.25;}
 public static double getLongitude(android.location.Location value){receiver(value);return -42.5;}
 public static float getSpeed(android.location.Location value){receiver(value);return 3.5f;}
 public static float getBearing(android.location.Location value){receiver(value);return 123.5f;}
}''',
    'dev/pgrazor/NativeRuntimeBootstrap.java': '''package dev.pgrazor;
public class NativeRuntimeBootstrap {public static void onRendered(){GameBridgeDispatch.log+="bootstrap;";}}''',
    'com/unity3d/player/UnityPlayerForActivityOrService.java': '''package com.unity3d.player;
public class UnityPlayerForActivityOrService {public boolean result,fail; public boolean nativeRender(){
 dev.pgrazor.GameBridgeDispatch.log+="render;";if(fail)throw new IllegalStateException("render");return result;}}''',
    'com/unity3d/player/W.java': '''package com.unity3d.player;
public class W {public UnityPlayerForActivityOrService player=new UnityPlayerForActivityOrService();
 public boolean handleMessage(android.os.Message m){if(m==null)return false;
 boolean result=player.nativeRender();dev.pgrazor.GameBridgeDispatch.log+="after;";return result;}}''',
    'dev/pgrazor/GameBridgeDispatch.java': '''package dev.pgrazor;
public final class GameBridgeDispatch {
 public static String log=""; public static Object consumer; public static long wide;public static boolean browser=true;
 public static boolean browserSignInEnabled(){return browser;}
 public static void signIn(android.app.Activity a,Object c){consumer=c;log+="signin;";}
 public static boolean locationPermission(String p){return "location".equals(p);}
 public static Object permissionStatus(long p){wide=p;return p==42?com.nianticlabs.platform.niaadvsynccore.permission.PermissionStatus.GRANTED:null;}
 public static void afterStart(Object x){log+="afterStart;";} public static void afterResume(Object x){log+="afterResume;";}
 public static void beforeStop(Object x){log+="beforeStop;";} public static void beforePause(Object x){log+="beforePause;";}
 public static void dismissDialog(Object x,String action){log+=action+";";}
}''',
    'kotlin/jvm/functions/Function1.java': 'package kotlin.jvm.functions; public interface Function1 { Object invoke(Object x); }',
    'com/google/android/gms/auth/api/signin/GoogleSignInClient.java': 'package com.google.android.gms.auth.api.signin; public class GoogleSignInClient {}',
    'com/nianticlabs/platform/authgooglesignin/SignInManager.java': '''package com.nianticlabs.platform.authgooglesignin;
public class SignInManager { public static class Companion {
 public final void signIn(android.app.Activity a,com.google.android.gms.auth.api.signin.GoogleSignInClient c,kotlin.jvm.functions.Function1 f){dev.pgrazor.GameBridgeDispatch.log+="originalSignIn;";}
}}''',
    'com/unity3d/player/UnityPermissions.java': '''package com.unity3d.player;
public class UnityPermissions { public static boolean hasUserAuthorizedPermission(android.content.Context c,String p){dev.pgrazor.GameBridgeDispatch.log+="permission-original;";return false;} }''',
    'com/unity3d/plugin/UnityAndroidPermissions.java': '''package com.unity3d.plugin;
public class UnityAndroidPermissions {public boolean IsPermissionGranted(android.app.Activity a,String p){dev.pgrazor.GameBridgeDispatch.log+="activity-original;";return false;}}''',
    'com/nianticlabs/platform/niaadvsynccore/permission/PermissionStatus.java': '''package com.nianticlabs.platform.niaadvsynccore.permission;
public enum PermissionStatus {GRANTED,DENIED}''',
    'com/nianticlabs/platform/niaadvsyncpermissions/internal/InternalPermissionManager.java': '''package com.nianticlabs.platform.niaadvsyncpermissions.internal;
import com.nianticlabs.platform.niaadvsynccore.permission.PermissionStatus;
public class InternalPermissionManager {public long original;
 public PermissionStatus permissionStatus(long p){original=p;return PermissionStatus.DENIED;}}''',
    'com/nianticlabs/nia/contextservice/ContextService.java': '''package com.nianticlabs.nia.contextservice;
public class ContextService {public boolean fail;
 private void invokeOnStart(){dev.pgrazor.GameBridgeDispatch.log+="originalStart;";if(fail)throw new IllegalStateException("original");}
 private void invokeOnResume(){dev.pgrazor.GameBridgeDispatch.log+="originalResume;";}
 private void invokeOnStop(){dev.pgrazor.GameBridgeDispatch.log+="originalStop;";}
 private void invokeOnPause(){dev.pgrazor.GameBridgeDispatch.log+="originalPause;";}
 public void run(){invokeOnStart();invokeOnResume();invokeOnPause();invokeOnStop();}
}''',
    'BridgeFixture.java': '''import dev.pgrazor.GameBridgeDispatch;
import com.nianticlabs.platform.niaadvsynccore.permission.PermissionStatus;
public class BridgeFixture {
 static void check(boolean b){if(!b)throw new AssertionError(GameBridgeDispatch.log);}
 public static void main(String[] args)throws Exception{
  android.os.Looper.prepare();
  android.location.Location location=new android.location.Location("synthetic-rewrite-fixture");
  check(location.getLatitude()==12.25&&location.getLongitude()==-42.5&&location.getSpeed()==3.5f&&location.getBearing()==123.5f);
  try{((android.location.Location)null).getLatitude();throw new AssertionError();}
  catch(NullPointerException expected){check(expected.getMessage().equals("fixture-location"));}
  com.unity3d.player.W render=new com.unity3d.player.W();
  check(!render.handleMessage(null)&&GameBridgeDispatch.log.equals(""));
  android.os.Message message=android.os.Message.obtain();
  check(!render.handleMessage(message)&&GameBridgeDispatch.log.equals("render;bootstrap;after;"));
  GameBridgeDispatch.log="";render.player.result=true;
  check(render.handleMessage(message)&&GameBridgeDispatch.log.equals("render;bootstrap;after;"));
  GameBridgeDispatch.log="";render.player.fail=true;
  try{render.handleMessage(message);throw new AssertionError();}catch(IllegalStateException e){check(e.getMessage().equals("render"));}
  check(GameBridgeDispatch.log.equals("render;"));GameBridgeDispatch.log="";
  kotlin.jvm.functions.Function1 f=x->x;
  new com.nianticlabs.platform.authgooglesignin.SignInManager.Companion().signIn(null,null,f);
  check(GameBridgeDispatch.consumer==f&&GameBridgeDispatch.log.equals("signin;"));
  GameBridgeDispatch.browser=false;new com.nianticlabs.platform.authgooglesignin.SignInManager.Companion().signIn(null,null,f);
  check(GameBridgeDispatch.log.equals("signin;originalSignIn;"));GameBridgeDispatch.browser=true;
  check(com.unity3d.player.UnityPermissions.hasUserAuthorizedPermission(null,"location"));
  check(!com.unity3d.player.UnityPermissions.hasUserAuthorizedPermission(null,"camera"));
  com.unity3d.plugin.UnityAndroidPermissions plugin=new com.unity3d.plugin.UnityAndroidPermissions();
  check(!plugin.IsPermissionGranted(null,"location"));
  check(plugin.IsPermissionGranted(new android.app.Activity(),"location"));
  com.nianticlabs.platform.niaadvsyncpermissions.internal.InternalPermissionManager p=new com.nianticlabs.platform.niaadvsyncpermissions.internal.InternalPermissionManager();
  check(p.permissionStatus(42)==PermissionStatus.GRANTED&&p.original==0);
  long exact=0x7123456789abcdefL;check(p.permissionStatus(exact)==PermissionStatus.DENIED&&p.original==exact&&GameBridgeDispatch.wide==exact);
  GameBridgeDispatch.log="";com.nianticlabs.nia.contextservice.ContextService s=new com.nianticlabs.nia.contextservice.ContextService();s.run();
  check(GameBridgeDispatch.log.equals("originalStart;afterStart;originalResume;afterResume;beforePause;originalPause;beforeStop;originalStop;"));
  s.fail=true;GameBridgeDispatch.log="";try{s.run();throw new AssertionError();}catch(IllegalStateException expected){check(expected.getMessage().equals("original"));}
  check(GameBridgeDispatch.log.equals("originalStart;"));
  GameBridgeDispatch.log="";
  for(String n:new String[]{"InitialDialog","ManualPermissionDialog","FinalDialog"}){
   Class<?> c=Class.forName("com.nianticlabs.background.configuration."+n);Object o=c.newInstance();
   java.lang.reflect.Method m=c.getDeclaredMethod("onCreate",android.os.Bundle.class);m.setAccessible(true);m.invoke(o,new Object[]{null});
  }
  check(GameBridgeDispatch.log.equals("originalDialog;onClickCancel;originalDialog;onClickCancel;originalDialog;onClickClose;"));
  System.out.println("PASS ART DEX bridges: auth identity, permission fallback/null Activity, exact64, lifecycle order/exceptions, private dispatch, dialogs");
  System.out.flush();System.exit(0);
 }
}''',
}
for name in ('InitialDialog', 'ManualPermissionDialog', 'FinalDialog'):
    SOURCES['com/nianticlabs/background/configuration/' + name + '.java'] = (
        'package com.nianticlabs.background.configuration; public class ' + name + ' {'
        'protected void onCreate(android.os.Bundle b){dev.pgrazor.GameBridgeDispatch.log+="originalDialog;";}}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial')
    parser.add_argument('--adb', default='adb')
    args = parser.parse_args()
    if args.serial and args.serial != '127.0.0.1:16416':
        parser.error('This research fixture is restricted to the authorized MuMu serial')
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%d-%H%M%S-%f')
    work = build.ROOT / 'build/dex-bridge-fixtures' / stamp
    work.mkdir(parents=True)
    receipt = {'success': False, 'serial': args.serial, 'runtime_verified': False}

    def run(command):
        p = subprocess.run(list(map(str, command)), capture_output=True, text=True, timeout=180)
        if p.returncode:
            raise RuntimeError(p.stdout + p.stderr)
        return p.stdout + p.stderr

    try:
        sources = []
        for name, content in SOURCES.items():
            path = work / 'src' / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding='utf-8')
            sources.append(path)
        classes = work / 'classes'
        classes.mkdir()
        run(['javac', '-source', '8', '-target', '8', '-classpath', build.ANDROID, '-d', classes, *sources])
        jar = work / 'fixture.jar'
        with zipfile.ZipFile(jar, 'w') as out:
            for p in classes.rglob('*.class'):
                out.write(p, p.relative_to(classes).as_posix())
        dex = work / 'dex'
        dex.mkdir()
        run([build.TOOLS / 'd8.bat', '--min-api', '28', '--lib', build.ANDROID, '--output', dex, jar])
        original = work / 'original.apk'
        with zipfile.ZipFile(original, 'w') as out:
            out.write(dex / 'classes.dex', 'classes.dex')
        replacements, rewrite_receipt = dex_bridges.rewrite(original, work / 'rewrite')
        receipt['rewrite'] = rewrite_receipt
        if rewrite_receipt['location_counts']['rewritten'] < 5:
            raise RuntimeError('Location fixture calls were not rewritten')
        patched = work / 'patched.jar'
        with zipfile.ZipFile(patched, 'w') as out:
            for name, path in replacements.items():
                out.write(path, name)
        if args.serial:
            remote = '/data/local/tmp/pgrazor-dex-bridge-' + stamp + '.jar'
            adb = [args.adb, '-s', args.serial]
            run(adb + ['push', patched, remote])
            output = run(adb + ['shell', 'env', 'CLASSPATH=' + remote, 'app_process', '/system/bin', 'BridgeFixture'])
            receipt['output'] = output
            if 'PASS ART DEX bridges:' not in output:
                raise RuntimeError('Missing ART completion marker')
            receipt['runtime_verified'] = True
            print(output.strip())
        receipt['success'] = True
    except Exception as error:
        receipt['error'] = str(error)
        print(str(error))
    finally:
        (work / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
        print('Receipt: ' + str(work / 'receipt.json'))
    return 0 if receipt['success'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
