package dev.pgrazor;

import java.util.Properties;

/** Pure Java account configuration and native wire serialization for the Buddy Emotion Engine. */
public final class BuddyEmotionPreferences {
    private boolean enabled, gifts;
    private boolean overlay=true;
    private int reserve=50;
    private String berries="703,701,705";

    public static boolean validBerries(String value){
        if(value==null||value.length()>19)return false;
        if(value.isEmpty())return true;
        java.util.HashSet<String> seen=new java.util.HashSet<>();
        for(String id:value.split(",",-1)){
            if(!id.equals("701")&&!id.equals("703")&&!id.equals("705")&&!id.equals("706")&&!id.equals("708"))return false;
            if(!seen.add(id))return false;
        }
        return true;
    }

    private static int number(Properties p,String key,int fallback,int min,int max){
        try{int n=Integer.parseInt(p.getProperty("buddyEmotion"+key,""));return n>=min&&n<=max?n:fallback;}
        catch(NumberFormatException e){return fallback;}
    }
    private static boolean flag(Properties p,String key,boolean fallback){
        String s=p.getProperty("buddyEmotion"+key);return "true".equals(s)?true:"false".equals(s)?false:fallback;
    }
    public synchronized void read(Properties p){
        boolean legacy=!p.containsKey("buddyEmotionEnabled")&&p.containsKey("buddyBerry");
        String old=p.getProperty(legacy?"buddyBerry":"buddyEmotionBerry", "703");
        if(legacy&&old.equals("707"))old="708";
        enabled=flag(p,"Enabled",legacy&&!old.isEmpty()&&!old.equals("0")&&validBerries(old));gifts=flag(p,"Gifts",false);
        overlay=flag(p,"Overlay",true);
        String fallback=(legacy||p.containsKey("buddyEmotionBerry"))?(old.equals("0")?"":validBerries(old)?old:"703"):"703,701,705";
        String saved=p.getProperty("buddyEmotionBerries",fallback);
        berries=validBerries(saved)?saved:fallback;reserve=number(p,"Reserve",legacy?0:50,0,1000);
    }
    public synchronized void write(Properties p){
        p.setProperty("buddyEmotionEnabled",String.valueOf(enabled));p.setProperty("buddyEmotionGifts",String.valueOf(gifts));
        p.setProperty("buddyEmotionOverlay",String.valueOf(overlay));
        p.setProperty("buddyEmotionBerries",berries);p.setProperty("buddyEmotionReserve",String.valueOf(reserve));
        p.remove("buddyEmotionBerry");
    }
    public synchronized String config(){
        String[] ids=berries.isEmpty()?new String[0]:berries.split(",");
        StringBuilder wire=new StringBuilder("2;").append(enabled?1:0);
        for(int i=0;i<5;i++)wire.append(';').append(i<ids.length?ids[i]:"0");
        return wire.append(';').append(reserve).append(';').append(gifts?1:0).toString();
    }
    public synchronized boolean enabled(){return enabled;}
    public synchronized boolean overlay(){return overlay;}
    public synchronized int berry(){return berries.isEmpty()?0:Integer.parseInt(berries.split(",")[0]);}
    public synchronized String berries(){return berries;}
    public synchronized int reserve(){return reserve;}
    public synchronized boolean gifts(){return gifts;}
    /** Validates a complete prospective snapshot before replacing any state. */
    public synchronized void set(String suffix,String value){
        if(suffix==null||value==null)throw new IllegalArgumentException("Setting key and value are required.");
        Properties p=new Properties();write(p);p.setProperty("buddyEmotion"+suffix,value);
        BuddyEmotionPreferences candidate=new BuddyEmotionPreferences();candidate.read(p);
        Properties normalized=new Properties();candidate.write(normalized);
        if(!value.equals(normalized.getProperty("buddyEmotion"+suffix)))
            throw new IllegalArgumentException("Invalid buddy emotion "+suffix);
        read(p);
    }
}
