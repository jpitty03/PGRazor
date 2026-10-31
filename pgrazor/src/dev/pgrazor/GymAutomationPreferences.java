package dev.pgrazor;

import java.util.Properties;

/** Pure Java account configuration and native wire serialization for the Gym Slotter & Berry Feeder. */
public final class GymAutomationPreferences {
    private boolean deploy, feed, fallback, overlay=true, running;
    private int nanabReserve=50, razzReserve=50;

    private static int number(Properties p,String key,int fallback,int min,int max){
        try{int n=Integer.parseInt(p.getProperty("gymAutomation"+key,""));return n>=min&&n<=max?n:fallback;}
        catch(NumberFormatException e){return fallback;}
    }
    private static boolean flag(Properties p,String key,boolean fallback){
        String s=p.getProperty("gymAutomation"+key);return "true".equals(s)?true:"false".equals(s)?false:fallback;
    }
    public synchronized void read(Properties p){
        running=false;
        deploy=flag(p,"Deploy",false);feed=flag(p,"Feed",false);fallback=flag(p,"Fallback",false);
        overlay=flag(p,"Overlay",true);
        nanabReserve=number(p,"NanabReserve",50,0,1000);razzReserve=number(p,"RazzReserve",50,0,1000);
    }
    public synchronized void write(Properties p){
        p.setProperty("gymAutomationDeploy",String.valueOf(deploy));p.setProperty("gymAutomationFeed",String.valueOf(feed));
        p.setProperty("gymAutomationFallback",String.valueOf(fallback));
        p.setProperty("gymAutomationOverlay",String.valueOf(overlay));
        p.setProperty("gymAutomationNanabReserve",String.valueOf(nanabReserve));
        p.setProperty("gymAutomationRazzReserve",String.valueOf(razzReserve));
    }
    public synchronized String config(){return config(false);}
    public synchronized String config(boolean gymSpinActive){
        return "2;"+(running&&deploy?1:0)+";"+(running&&feed?1:0)+";"+(fallback?1:0)+";"+nanabReserve+";"+razzReserve+";"+(gymSpinActive?1:0);
    }
    public synchronized boolean deploy(){return deploy;}
    public synchronized boolean feed(){return feed;}
    public synchronized boolean fallback(){return fallback;}
    public synchronized boolean overlay(){return overlay;}
    public synchronized boolean running(){return running;}
    public synchronized boolean effectiveDeploy(){return running&&deploy;}
    public synchronized boolean effectiveFeed(){return running&&feed;}
    /** Starting requires at least one selected action; no selection is changed. */
    public synchronized boolean start(){
        if(!deploy&&!feed)return false;
        running=true;return true;
    }
    public synchronized void stop(){running=false;}
    /** Cooldown trips the whole run once and keeps its selected actions. */
    public synchronized boolean stopForCooldown(boolean active){
        if(!active||!running)return false;
        running=false;return true;
    }
    public synchronized int nanabReserve(){return nanabReserve;}
    public synchronized int razzReserve(){return razzReserve;}
    /** Validates a complete prospective snapshot before replacing any state. */
    public synchronized void set(String suffix,String value){
        if(suffix==null||value==null)throw new IllegalArgumentException("Setting key and value are required.");
        Properties p=new Properties();write(p);p.setProperty("gymAutomation"+suffix,value);
        GymAutomationPreferences candidate=new GymAutomationPreferences();candidate.read(p);
        Properties normalized=new Properties();candidate.write(normalized);
        if(!value.equals(normalized.getProperty("gymAutomation"+suffix)))
            throw new IllegalArgumentException("Invalid gym automation "+suffix);
        boolean wasRunning=running;
        read(p);
        running=wasRunning&&(deploy||feed);
    }
}
