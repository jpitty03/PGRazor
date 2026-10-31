package dev.pgrazor;
import java.util.Properties;
public final class BuddyEmotionPreferencesTest {
 static void eq(Object a,Object b){if(!a.equals(b))throw new AssertionError(a+" != "+b);}
 static void reject(Runnable r){try{r.run();}catch(IllegalArgumentException ok){return;}throw new AssertionError("accepted invalid setting");}
 public static void main(String[] args){
  BuddyEmotionPreferences p=new BuddyEmotionPreferences();eq("2;0;703;701;705;0;0;50;0",p.config());
  eq(true,p.overlay());
  p.set("Enabled","true");p.set("Berries","701,708,706");p.set("Reserve","0");p.set("Gifts","true");eq("2;1;701;708;706;0;0;0;1",p.config());
  String nativeConfig=p.config();p.set("Overlay","false");eq(false,p.overlay());eq(nativeConfig,p.config());
  Properties saved=new Properties();p.write(saved);BuddyEmotionPreferences restored=new BuddyEmotionPreferences();restored.read(saved);eq(p.config(),restored.config());restored.read(new Properties());eq("2;0;703;701;705;0;0;50;0",restored.config());
  eq("false",saved.getProperty("buddyEmotionOverlay"));restored.read(saved);eq(false,restored.overlay());
  restored.set("Enabled","false");String disabledConfig=restored.config();restored.set("Overlay","true");eq(disabledConfig,restored.config());eq(true,restored.overlay());
  restored.set("Overlay","false");restored.read(new Properties());eq(true,restored.overlay());
  reject(()->p.set("Overlay","yes"));eq(false,p.overlay());
  String[][] bad={{"Enabled","yes"},{"Gifts","1"},{"Berries","701,701"},{"Berries","707"},{"Berries","701,"},{"Berries","0701"},{"Berries","701, 703"},{"Berries","1201"},{"Reserve","-1"},{"Reserve","1001"},{"Reserve"," 50"},{"Unknown","true"}};
  for(String[] v:bad){String before=p.config();reject(()->p.set(v[0],v[1]));eq(before,p.config());}
  p.set("Berries","");p.set("Reserve","1000");p.set("Enabled","false");eq("2;0;0;0;0;0;0;1000;1",p.config());
  Properties legacy=new Properties();legacy.setProperty("buddyBerry","703");p.read(legacy);eq("2;1;703;0;0;0;0;0;0",p.config());
  legacy.setProperty("buddyBerry","0");p.read(legacy);eq(false,p.enabled());eq("",p.berries());
  legacy.setProperty("buddyEmotionEnabled","false");legacy.setProperty("buddyEmotionBerry","705");legacy.setProperty("buddyEmotionReserve","37");p.read(legacy);eq("2;0;705;0;0;0;0;37;0",p.config());
  legacy.setProperty("buddyEmotionBerries","708,706,703,701,705");p.read(legacy);eq("708,706,703,701,705",p.berries());
  System.out.println("BuddyEmotionPreferences: ordered berries, migration, account reset, reserves and invalid settings passed");
 }
}
