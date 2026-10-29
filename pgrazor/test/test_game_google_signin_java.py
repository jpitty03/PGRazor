"""Exercise the actual GameGoogleSignIn adapter on a JVM with narrow Android/game shells.

GameGoogleSignIn ports the runtime browser sign-in adapter (embedded-agent.js) to a compiled Java
class that owns the pending consumer, the poll timer and the result/token parsing, and reaches the
game only by reflection. These shells provide fake classes whose names and reflection signatures
match the runtime (SignInResponse(String,String,String), kotlin Function1.invoke(Object)) plus a fake
GoogleBrowserAuth with the real begin/takeResult/cancel contract and a controllable Handler whose
queue is drained explicitly by the Java driver. The real GoogleBrowserAuth (PKCE/network/storage) is
deliberately excluded so this test isolates the adapter's state machine, and a narrow org.json stub
avoids any network download. The fake Control has no foreground accessor, so the production class
compiling at all proves it applies no foreground gate. No Android SDK, device or game binary is
required; a JDK is.
"""
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_control_java import java_tools

ROOT = Path(__file__).resolve().parents[1]

STUBS = {
    'android/app/Activity.java': 'package android.app; public class Activity {}',
    'android/os/Looper.java': '''package android.os;
public class Looper {
 private static final Looper MAIN=new Looper();
 public static Looper getMainLooper(){return MAIN;}
 public static Looper myLooper(){return MAIN;}
}''',
    # Controllable main handler: posted work runs only when the driver drains it, and entries are kept
    # ordered by delay (stable among equal delays) so an immediate post runs ahead of a delayed poll.
    'android/os/Handler.java': '''package android.os;
import java.util.ArrayList;
import java.util.List;
public class Handler {
 private static final class Entry { final Runnable r; final long delay; Entry(Runnable r,long d){this.r=r;this.delay=d;} }
 private static final List<Entry> queue=new ArrayList<>();
 public Handler(Looper l){}
 public boolean post(Runnable r){return postDelayed(r,0);}
 public boolean postDelayed(Runnable r,long delay){
  int idx=0; while(idx<queue.size() && queue.get(idx).delay<=delay) idx++;
  queue.add(idx,new Entry(r,delay)); return true;
 }
 public void removeCallbacks(Runnable r){ for(int k=queue.size()-1;k>=0;k--) if(queue.get(k).r==r) queue.remove(k); }
 public static void runNext(){ if(!queue.isEmpty()){ Entry e=queue.remove(0); e.r.run(); } }
 public static int size(){ return queue.size(); }
 public static long delayAt(int index){ return queue.get(index).delay; }
 public static void reset(){ queue.clear(); }
}''',
    # Kotlin single-argument function: the game consumer the hook hands the adapter.
    'kotlin/jvm/functions/Function1.java': '''package kotlin.jvm.functions;
public interface Function1 { Object invoke(Object argument); }''',
    # Game response type: exactly the (status,email,idToken) constructor the adapter resolves by
    # reflection. Argument types matter -- a non-String constructor would not resolve.
    'com/nianticlabs/platform/authgooglesignin/SignInResponse.java': '''package com.nianticlabs.platform.authgooglesignin;
public final class SignInResponse {
 public final String status, email, idToken;
 public SignInResponse(String status, String email, String idToken){ this.status=status; this.email=email; this.idToken=idToken; }
}''',
    # Fake browser auth with the real public contract. Scriptable so the driver controls begin success,
    # the result stream and a shutdown that races begin. The real class (PKCE/network) is excluded.
    'dev/pgrazor/GoogleBrowserAuth.java': '''package dev.pgrazor;
import java.util.ArrayList;
import java.util.List;
public final class GoogleBrowserAuth {
 public static String nextId="id-1";
 public static String lastId;
 public static boolean beginThrows, takeThrows;
 public static int beginCount, takeCount;
 public static final List<String> results=new ArrayList<>();
 public static final List<String> cancelled=new ArrayList<>();
 public static Runnable onBegin;
 private GoogleBrowserAuth(){}
 public static String begin(android.app.Activity activity){
  beginCount++;
  if(onBegin!=null){ Runnable r=onBegin; onBegin=null; r.run(); }
  if(beginThrows) throw new RuntimeException("no browser available");
  lastId=nextId; return nextId;
 }
 public static String takeResult(String id){
  takeCount++;
  if(takeThrows) throw new RuntimeException("poll failed");
  return results.isEmpty()?null:results.remove(0);
 }
 public static void cancel(String id){ cancelled.add(id); }
 public static void reset(){
  nextId="id-1"; lastId=null; beginThrows=false; takeThrows=false;
  beginCount=0; takeCount=0; results.clear(); cancelled.clear(); onBegin=null;
 }
}''',
    # Test-only Control: an events log the driver inspects for sensitive leakage. It intentionally has
    # no active()/foreground accessor, so the production class referencing one would fail to compile.
    'dev/pgrazor/Control.java': '''package dev.pgrazor;
import java.util.ArrayList;
import java.util.List;
public final class Control {
 public static final List<String> events=new ArrayList<>();
 public static void event(String kind,String detail){ events.add(kind+"|"+detail); }
}''',
    # Narrow org.json stub: a flat-object parser supporting only what the adapter reads (string, null,
    # boolean, number values), with get() returning the raw value so the adapter can reject non-strings
    # and a JSON null (the NULL sentinel) without any toString coercion. No network download.
    'org/json/JSONException.java': '''package org.json;
public class JSONException extends Exception { public JSONException(String message){ super(message); } }''',
    'org/json/JSONObject.java': '''package org.json;
import java.util.HashMap;
import java.util.Map;
public class JSONObject {
 public static final Object NULL=new Object(){ public String toString(){ return "null"; } };
 private final Map<String,Object> values=new HashMap<>();
 public JSONObject(){}
 public JSONObject(String source) throws JSONException {
  Parser parser=new Parser(source);
  parser.skipWhitespace();
  parser.readObject(values);
  parser.skipWhitespace();
  if(!parser.atEnd()) throw new JSONException("trailing characters");
 }
 public boolean has(String key){ return values.containsKey(key); }
 public Object get(String key) throws JSONException {
  if(!values.containsKey(key)) throw new JSONException("missing "+key);
  return values.get(key);
 }
 private static final class Parser {
  private final String s; private int i;
  Parser(String s){ this.s=s; }
  boolean atEnd(){ return i>=s.length(); }
  void skipWhitespace(){ while(i<s.length() && Character.isWhitespace(s.charAt(i))) i++; }
  void readObject(Map<String,Object> into) throws JSONException {
   if(i>=s.length() || s.charAt(i)!='{') throw new JSONException("expected object");
   i++; skipWhitespace();
   if(i<s.length() && s.charAt(i)=='}'){ i++; return; }
   while(true){
    skipWhitespace();
    String key=readString();
    skipWhitespace();
    if(i>=s.length() || s.charAt(i)!=':') throw new JSONException("expected colon");
    i++; skipWhitespace();
    into.put(key, readValue());
    skipWhitespace();
    if(i>=s.length()) throw new JSONException("unterminated object");
    char c=s.charAt(i++);
    if(c==',') continue;
    if(c=='}') return;
    throw new JSONException("expected , or }");
   }
  }
  Object readValue() throws JSONException {
   if(i>=s.length()) throw new JSONException("missing value");
   char c=s.charAt(i);
   if(c=='"') return readString();
   if(s.regionMatches(i,"null",0,4)){ i+=4; return NULL; }
   if(s.regionMatches(i,"true",0,4)){ i+=4; return Boolean.TRUE; }
   if(s.regionMatches(i,"false",0,5)){ i+=5; return Boolean.FALSE; }
   return readNumber();
  }
  String readString() throws JSONException {
   if(i>=s.length() || s.charAt(i)!='"') throw new JSONException("expected string");
   i++; StringBuilder sb=new StringBuilder();
   while(i<s.length()){
    char c=s.charAt(i++);
    if(c=='"') return sb.toString();
    if(c=='\\\\'){
     if(i>=s.length()) throw new JSONException("bad escape");
     char e=s.charAt(i++);
     switch(e){
      case '"': sb.append('"'); break;
      case '\\\\': sb.append('\\\\'); break;
      case '/': sb.append('/'); break;
      case 'n': sb.append('\\n'); break;
      case 't': sb.append('\\t'); break;
      case 'r': sb.append('\\r'); break;
      case 'b': sb.append('\\b'); break;
      case 'f': sb.append('\\f'); break;
      case 'u':
       if(i+4>s.length()) throw new JSONException("bad unicode");
       sb.append((char)Integer.parseInt(s.substring(i,i+4),16)); i+=4; break;
      default: throw new JSONException("bad escape");
     }
    } else sb.append(c);
   }
   throw new JSONException("unterminated string");
  }
  Object readNumber() throws JSONException {
   int start=i;
   while(i<s.length() && "+-0123456789.eE".indexOf(s.charAt(i))>=0) i++;
   if(i==start) throw new JSONException("invalid value");
   String num=s.substring(start,i);
   try {
    if(num.indexOf('.')>=0 || num.indexOf('e')>=0 || num.indexOf('E')>=0) return Double.parseDouble(num);
    return Long.parseLong(num);
   } catch(NumberFormatException ex){ throw new JSONException("invalid number"); }
  }
 }
}''',
}


class GameGoogleSignInJavaTests(unittest.TestCase):
    def test_sign_in_policy(self):
        compiler, runtime = java_tools()
        with tempfile.TemporaryDirectory(prefix='pgrazor-google-signin-test-') as directory:
            work = Path(directory)
            sources = []
            for relative, text in STUBS.items():
                source = work / relative
                source.parent.mkdir(parents=True, exist_ok=True)
                source.write_text(text, encoding='utf-8')
                sources.append(source)
            sources.append(ROOT / 'src/dev/pgrazor/GameGoogleSignIn.java')
            sources.append(ROOT / 'test/GameGoogleSignInTest.java')
            classes = work / 'classes'
            compiled = subprocess.run([str(compiler), '-encoding', 'UTF-8', '-d', str(classes),
                                       *map(str, sources)], capture_output=True, text=True, timeout=60)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            tested = subprocess.run([str(runtime), '-cp', str(classes), 'dev.pgrazor.GameGoogleSignInTest'],
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(tested.returncode, 0, tested.stdout + tested.stderr)
            self.assertIn('PASS: GameGoogleSignIn', tested.stdout)


if __name__ == '__main__':
    unittest.main()
