package dev.pgrazor;
public final class ControlAutoHealTest {
 static void require(boolean b,String why){if(!b)throw new AssertionError(why);}
 static void set(String n,Object value)throws Exception{java.lang.reflect.Field f=Control.class.getDeclaredField(n);f.setAccessible(true);f.set(null,value);}
 public static void main(String[]args)throws Exception{
  android.app.Application app=new android.app.Application(){public java.io.File getFilesDir(){return new java.io.File(args[0]);}};
  java.lang.reflect.Method load=Control.class.getDeclaredMethod("loadSettings",android.app.Application.class);load.setAccessible(true);load.invoke(null,app);
  if(args[1].equals("load")){require(Control.autoHeal()&&Control.autoHealBelowHalf(),"both healing options restored");require(Control.autoHealConfig().endsWith(":3:0"),"readiness not restored");return;}
  require(!Control.autoHeal()&&!Control.autoHealBelowHalf(),"both default off");
  Control.setAutoHealBelowHalf(true);require(Control.autoHealConfig().endsWith(":2:0"),"threshold independently enabled, readiness gated");
  Control.configureQuickSummary(true);Control.setQuickSummaryReady(true,"Ready");set("foreground",true);Control.connect();
  require(Control.autoHealConfig().endsWith(":2:1"),"live foreground authorizes injured healing");
  String old=Control.autoHealConfig().split(":")[1];Control.setAutoHealPanelOpen(true);require(Control.autoHealConfig().endsWith(":2:0"),"panel disarms");require(!old.equals(Control.autoHealConfig().split(":")[1]),"panel changes command");
  Control.setAutoHealPanelOpen(false);require(Control.autoHealConfig().endsWith(":2:1"),"closed panel can resume");set("foreground",false);require(Control.autoHealConfig().endsWith(":2:0"),"background disarms");set("foreground",true);
  Control.setAutoHeal(true);Control.flushSettings(5000);require(Control.exportSettings().getProperty("autoHeal").equals("true")&&Control.exportSettings().getProperty("autoHealBelowHalf").equals("true"),"both options in backup");
  System.out.println("PASS auto heal Control: defaults, independent threshold, readiness, foreground, panel command revocation and persistence");
 }
}
