"""Exercise actual one-shot lifecycle and MotionEngine with boundary-only shells.

Android notifications and JSONObject are deliberately narrow boundary shells;
these tests do not validate Android rendering or general JSON parsing.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_control_java import java_tools

ROOT = Path(__file__).resolve().parents[1]
STUBS = {
    "dev/pgrazor/AutoRaid.java": "package dev.pgrazor; public class AutoRaid {public static boolean active;public static boolean running(){return active;}}",
    'android/os/SystemClock.java': 'package android.os; public class SystemClock {public static long now=1000; public static long elapsedRealtime(){return now;}}',
    'android/os/Looper.java': 'package android.os; public class Looper {public static Looper getMainLooper(){return new Looper();}}',
    'android/os/Handler.java': 'package android.os; public class Handler {public Handler(Looper l){} public boolean post(Runnable r){r.run();return true;} public boolean postDelayed(Runnable r,long ms){r.run();return true;}}',
    'android/content/Context.java': 'package android.content; public class Context {public static final String VIBRATOR_SERVICE="vibrator"; public Object getSystemService(String s){return null;}}',
    'android/app/Activity.java': 'package android.app; public class Activity extends android.content.Context {public boolean isFinishing(){return false;}}',
    'android/widget/Toast.java': 'package android.widget; public class Toast {public static final int LENGTH_LONG=1; public static int count; public static Toast makeText(android.content.Context c,String t,int n){return new Toast();} public void show(){count++;}}',
    'android/media/AudioManager.java': 'package android.media; public class AudioManager {public static final int STREAM_NOTIFICATION=1;}',
    'android/media/ToneGenerator.java': 'package android.media; public class ToneGenerator {public static final int TONE_PROP_BEEP2=1;public ToneGenerator(int s,int v){} public void startTone(int n,int ms){} public void release(){}}',
    'android/os/Vibrator.java': 'package android.os; public class Vibrator {public boolean hasVibrator(){return false;} public void vibrate(long ms){}}',
    'org/json/JSONObject.java': r'''package org.json;
// Boundary shell: only flat JSON objects containing unescaped string pairs.
public class JSONObject {
 private final java.util.Map<String,String> values=new java.util.HashMap<>();
 public JSONObject(String text){
  if(!text.startsWith("{")||!text.endsWith("}"))throw new IllegalArgumentException("object");
  java.util.regex.Matcher m=java.util.regex.Pattern.compile("\"([^\"]*)\"\\s*:\\s*\"([^\"]*)\"").matcher(text);
  while(m.find())values.put(m.group(1),m.group(2));
 }
 public long optLong(String key,long fallback){try{return Long.parseLong(values.get(key));}catch(Exception e){return fallback;}}
 public String optString(String key){return optString(key,"");}
 public String optString(String key,String fallback){return values.getOrDefault(key,fallback);}
}''',
    'dev/pgrazor/BatchEvolutionSettings.java': 'package dev.pgrazor; public class BatchEvolutionSettings {public static boolean active;public static boolean enabled(){return active;}}',
    'dev/pgrazor/GymAutomationSettings.java': 'package dev.pgrazor; public class GymAutomationSettings {public static boolean active;public static boolean running(){return active;}}',
    'dev/pgrazor/SmartCatchSettings.java': 'package dev.pgrazor; public class SmartCatchSettings {public static boolean active;public static boolean gymSpinActive(){return active;}}',
    'dev/pgrazor/Control.java': '''package dev.pgrazor;
public class Control {
 public static MotionEngine engine=new MotionEngine();
 public static boolean ready=true,gameRoute,planning,vgp=true;
 public static int saves,walks;
 public static boolean automationReady(){return ready;}
 public static boolean giftsAllowed(){return ready;}
 public static boolean gameRoutesRunning(){return gameRoute;}
 public static boolean automationPlanning(){return planning;}
 public static double currentLatitude(){return engine.latitude;}
 public static double currentLongitude(){return engine.longitude;}
 public static void walkRoute(double[][] points){walks++;engine.walkRoute(points);}
 public static void saveState(){saves++;}
 public static void setVgpEnabled(boolean value){vgp=value;}
 public static android.app.Activity accountActivity(){return new android.app.Activity();}
 public static boolean overlaysHidden(){return false;}
}''',
}


class OneShotAutomationJavaTests(unittest.TestCase):
    def test_lifecycle(self):
        compiler, runtime = java_tools()
        with tempfile.TemporaryDirectory(prefix='pgrazor-one-shot-') as directory:
            work = Path(directory)
            sources = []
            for relative, text in STUBS.items():
                path = work / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text, encoding='utf-8')
                sources.append(path)
            sources += [ROOT / 'src/dev/pgrazor/GiftBatchSettings.java', ROOT / 'test/GiftBatchSettingsTest.java', ROOT / 'src/dev/pgrazor/OneShotAutomation.java',
                        ROOT / 'src/dev/pgrazor/MotionEngine.java',
                        ROOT / 'test/OneShotAutomationTest.java']
            compiled = subprocess.run([str(compiler), '-encoding', 'UTF-8', '-d', str(work),
                                       *map(str, sources)], capture_output=True, text=True, timeout=45)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            tested = subprocess.run([str(runtime), '-ea', '-cp', str(work),
                                     'dev.pgrazor.OneShotAutomationTest'],
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(tested.returncode, 0, tested.stdout + tested.stderr)
            self.assertIn('PASS OneShotAutomation:', tested.stdout)
            tested = subprocess.run([str(runtime), '-ea', '-cp', str(work), 'dev.pgrazor.GiftBatchSettingsTest'], capture_output=True, text=True, timeout=30)
            self.assertEqual(tested.returncode, 0, tested.stdout + tested.stderr)



if __name__ == '__main__':
    unittest.main()
