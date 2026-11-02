package dev.pgrazor;
public final class ControlRocketAutoTest {
 static void require(boolean ok,String why){if(!ok)throw new AssertionError(why);}
 static void set(String n,Object v)throws Exception{java.lang.reflect.Field f=Control.class.getDeclaredField(n);f.setAccessible(true);f.set(null,v);}
 static long command(){return Long.parseLong(Control.rocketAutoConfig().split(":")[1]);}
 public static void main(String[]args)throws Exception{
  android.app.Application app=new android.app.Application(){public java.io.File getFilesDir(){return new java.io.File(args[0]);}};
  if(args[1].equals("load")){java.io.File file=AccountProfiles.file(app,"pgrazor-settings-v1.properties");java.util.Properties legacy=new java.util.Properties();try(java.io.FileInputStream in=new java.io.FileInputStream(file)){legacy.load(in);}legacy.setProperty("rocketOptions","63");try(java.io.FileOutputStream out=new java.io.FileOutputStream(file)){legacy.store(out,"legacy six-option settings");}}
  java.lang.reflect.Method load=Control.class.getDeclaredMethod("loadSettings",android.app.Application.class);load.setAccessible(true);load.invoke(null,app);
  if(args[1].equals("load")){require(Control.rocketOptions()==31,"removed option stripped on settings load");require(!Control.rocketAutoAllowed(),"readiness not restored from disk");return;}
  require(Control.rocketOptions()==0&&!Control.rocketAutoAllowed(),"default off");
  Control.setRocketOptions(5);require(Control.rocketCombatOptions()==5,"manual mask unchanged");Control.setRocketOptions(13);require(Control.rocketOptions()==13&&Control.rocketCombatOptions()==7,"full auto implies all attacks without overwriting manual mask");
  Control.setRocketOptions(64);Control.setRocketOptions(-1);require(Control.rocketOptions()==13,"invalid mask rejected");
  Control.configureQuickSummary(true);Control.setQuickSummaryReady(true,"Ready");Control.setRocketReady(true,"Ready");set("foreground",true);Control.connect();set("pad",new Pad(new android.app.Activity()));android.os.SystemClock.now+=1500;android.os.Handler.queue.clear();
  require(Control.rocketAutoAllowed(),"live host, ready combat, settled settings");long now=android.os.SystemClock.now;
  Control.rocketAutoSwipe(command(),now+200);android.os.Handler.drain();require(Pad.rocketSwipes==1,"valid request dispatched once");
  Control.rocketAutoSwipe(command(),now+200);Control.setRocketOptions(5);android.os.Handler.drain();require(Pad.rocketSwipes==1&&Control.rocketCombatOptions()==5,"Off cancels queued gesture, restores manual mask");
  Control.setRocketOptions(13);android.os.SystemClock.now+=501;now=android.os.SystemClock.now;Control.rocketAutoSwipe(command(),now+201);android.os.Handler.drain();require(Pad.rocketSwipes==1,"unbounded future expiry refused");
  Control.rocketAutoSwipe(command(),now+200);android.os.SystemClock.now+=201;android.os.Handler.drain();require(Pad.rocketSwipes==1,"expired request refused");
  now=android.os.SystemClock.now;Control.rocketAutoSwipe(command(),now+200);Control.setRocketAutoPanelOpen(true);android.os.Handler.drain();require(Pad.rocketSwipes==1&&!Control.rocketAutoAllowed(),"settings revoke pending request and native flow");
  android.os.SystemClock.now+=5000;require(!Control.rocketAutoAllowed(),"panel blocks beyond timeout");Control.setRocketAutoPanelOpen(false);require(!Control.rocketAutoAllowed(),"closing panel has stabilization delay");android.os.SystemClock.now+=1001;Control.renewLease();require(Control.rocketAutoAllowed(),"resumes after stabilization with a live host lease");
  now=android.os.SystemClock.now;Control.rocketAutoSwipe(command(),now+200);set("foreground",false);android.os.Handler.drain();require(Pad.rocketSwipes==1,"background refuses queued gesture");set("foreground",true);
  now=android.os.SystemClock.now;Control.renewLease();long token=command();require(Control.rocketAutoGestureValid(token,now),"started gesture authorized");
  android.os.SystemClock.now=now+201;require(!Control.rocketAutoTouchValid(token,now+200)&&Control.rocketAutoGestureValid(token,now),"request expiry does not cancel an already started bounded gesture");
  android.os.SystemClock.now=now+601;require(!Control.rocketAutoGestureValid(token,now),"gesture has hard 600 ms limit");
  Control.renewLease();require(!Control.rocketAutoGestureValid(token+1,now+601),"wrong command rejected");
  Control.setRocketAutoPanelOpen(true);require(!Control.rocketAutoGestureValid(command(),now+601),"panel revokes running gesture");Control.setRocketAutoPanelOpen(false);
  Control.setRocketOptions(63);Control.flushSettings(5000);require(Control.exportSettings().getProperty("rocketOptions").equals("31"),"legacy skip rescue stripped before persistence/export");
  System.out.println("PASS Auto Rocket Control: default off, raw/effective masks, persistence, foreground, panel/Off revocation and bounded request expiry");
 }
}
