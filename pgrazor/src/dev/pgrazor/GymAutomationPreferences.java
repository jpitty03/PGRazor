package dev.pgrazor;

import java.util.Properties;

/** Pure Java account configuration and native wire serialization for the Gym Slotter & Berry Feeder. */
public final class GymAutomationPreferences {
    private boolean deploy, feed, fallback;
    private int nanabReserve=50, razzReserve=50;

    private static int number(Properties p,String key,int fallback,int min,int max){
        try{int n=Integer.parseInt(p.getProperty("gymAutomation"+key,""));return n>=min&&n<=max?n:fallback;}
        catch(NumberFormatException e){return fallback;}
    }
    private static boolean flag(Properties p,String key,boolean fallback){
        String s=p.getProperty("gymAutomation"+key);return "true".equals(s)?true:"false".equals(s)?false:fallback;
    }
    public synchronized void read(Properties p){
        deploy=flag(p,"Deploy",false);feed=flag(p,"Feed",false);fallback=flag(p,"Fallback",false);
        nanabReserve=number(p,"NanabReserve",50,0,1000);razzReserve=number(p,"RazzReserve",50,0,1000);
    }
    public synchronized void write(Properties p){
        p.setProperty("gymAutomationDeploy",String.valueOf(deploy));p.setProperty("gymAutomationFeed",String.valueOf(feed));
        p.setProperty("gymAutomationFallback",String.valueOf(fallback));
        p.setProperty("gymAutomationNanabReserve",String.valueOf(nanabReserve));
        p.setProperty("gymAutomationRazzReserve",String.valueOf(razzReserve));
    }
    public synchronized String config(){
        return "1;"+(deploy?1:0)+";"+(feed?1:0)+";"+(fallback?1:0)+";"+nanabReserve+";"+razzReserve;
    }
    public synchronized boolean deploy(){return deploy;}
    public synchronized boolean feed(){return feed;}
    public synchronized boolean fallback(){return fallback;}
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
        read(p);
    }
}
