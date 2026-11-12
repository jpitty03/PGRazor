package dev.pgrazor;
import java.io.*;import java.nio.file.*;import java.lang.reflect.*;
public class ControlLogRetentionTest {
 static void set(String n,Object v)throws Exception{Field f=Control.class.getDeclaredField(n);f.setAccessible(true);f.set(null,v);}
 public static void main(String[] args)throws Exception{
  File dir=Files.createTempDirectory("pgr-log-test").toFile();
  try{
   File unrelated=new File(dir,"evidence.txt");Files.write(unrelated.toPath(),new byte[]{1});
   for(int i=0;i<9;i++)Files.write(new File(dir,"20261030-120000-00"+i+".txt").toPath(),new byte[]{1});
   Control.event("native-scheduler-unavailable","status=-3 retained startup reason");
   Control.event("session-root","rootStatus=not-detected retained root evidence");
   File current=new File(dir,"20261031-120000-000.txt");set("logFile",current);set("log",new PrintWriter(new FileOutputStream(current),true));set("logBytes",1048576L);
   Control.event("test","latest marker");
   File active=Control.currentLog();if(active.equals(current)||active.length()>1048576||!unrelated.exists())throw new AssertionError("rotation");
   File[] logs=dir.listFiles((d,n)->!n.equals("evidence.txt"));if(logs.length!=6)throw new AssertionError("retention "+logs.length);
   String big=new String(new char[100000]).replace('\0','x');Control.event("test",big);
   if(active.length()>65536+1000)throw new AssertionError("event cap");
   String tail=new String(Files.readAllBytes(active.toPath()),"UTF-8");if(!tail.contains("[retained-context]")||!tail.contains("retained startup reason")||!tail.contains("retained root evidence")||!tail.contains("latest marker")||!tail.contains("[truncated]"))throw new AssertionError("tail content");
   Field writer=Control.class.getDeclaredField("log");writer.setAccessible(true);((PrintWriter)writer.get(null)).close();
   set("log",new PrintWriter(new OutputStream(){public void write(int b)throws IOException{throw new IOException("fixture disk full");}},true));
   // The JVM Log.e fixture throws; require the expected disk-write error, not silent PrintWriter failure.
   try{Control.event("test","write failure must surface");throw new AssertionError("missing write error");}
   catch(AssertionError expected){if(!"File logging unavailable".equals(expected.getMessage())||!(expected.getCause() instanceof IOException))throw expected;}
   if(writer.get(null)!=null)throw new AssertionError("failed writer retained");
   System.out.println("PASS bounded rotating logs, retained newest segments, unrelated evidence preserved and oversized event cap");
  }finally{Field f=Control.class.getDeclaredField("log");f.setAccessible(true);PrintWriter w=(PrintWriter)f.get(null);if(w!=null)w.close();set("log",null);for(File p:dir.listFiles())p.delete();dir.delete();}
 }
}
