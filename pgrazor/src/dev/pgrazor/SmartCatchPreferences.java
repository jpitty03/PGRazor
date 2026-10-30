package dev.pgrazor;

import java.util.Properties;
import java.util.TreeSet;

/** Pure Java account configuration and native wire serialization. */
public final class SmartCatchPreferences {
    private boolean enabled, premium=true, curve=true;
    private int ball=1, berry, highCp=1500, highLevel=30, grade=3;
    private String candySpecies="";
    private static final int[] BERRIES={0,701,703,705,706,708};

    private static int number(Properties p,String key,int fallback,int min,int max){
        try{int n=Integer.parseInt(p.getProperty("smartCatch"+key,""));return n>=min&&n<=max?n:fallback;}
        catch(NumberFormatException e){return fallback;}
    }
    private static boolean flag(Properties p,String key,boolean fallback){
        String s=p.getProperty("smartCatch"+key);return "true".equals(s)?true:"false".equals(s)?false:fallback;
    }
    private static boolean validBerry(int id){for(int n:BERRIES)if(n==id)return true;return false;}
    public static String normalizeSpecies(String value){
        if(value==null||value.length()>10000)throw new IllegalArgumentException("Enter at most 200 species IDs.");
        if(value.trim().isEmpty())return "";
        String[] parts=value.split(",",-1);
        if(parts.length>200)throw new IllegalArgumentException("Enter at most 200 species IDs.");
        TreeSet<Integer> ids=new TreeSet<>();
        for(String part:parts){
            int id;
            try{id=Integer.parseInt(part.trim());}catch(NumberFormatException e){throw new IllegalArgumentException("Use comma-separated National Dex IDs (1–2000).");}
            if(id<1||id>2000)throw new IllegalArgumentException("Species IDs must be 1–2000.");
            ids.add(id);
        }
        StringBuilder out=new StringBuilder();
        for(int id:ids){if(out.length()>0)out.append(',');out.append(id);}
        return out.toString();
    }
    public synchronized void read(Properties p){
        enabled=flag(p,"Enabled",false);ball=number(p,"Ball",1,1,3);
        berry=number(p,"Berry",0,0,708);if(!validBerry(berry))berry=0;
        premium=flag(p,"Premium",true);highCp=number(p,"HighCp",1500,0,10000);
        highLevel=number(p,"HighLevel",30,0,50);grade=number(p,"Grade",3,0,3);curve=flag(p,"Curve",true);
        try{candySpecies=normalizeSpecies(p.getProperty("smartCatchCandySpecies",""));}catch(IllegalArgumentException e){candySpecies="";}
    }
    public synchronized void write(Properties p){
        p.setProperty("smartCatchEnabled",String.valueOf(enabled));p.setProperty("smartCatchBall",String.valueOf(ball));
        p.setProperty("smartCatchBerry",String.valueOf(berry));p.setProperty("smartCatchPremium",String.valueOf(premium));
        p.setProperty("smartCatchHighCp",String.valueOf(highCp));p.setProperty("smartCatchHighLevel",String.valueOf(highLevel));
        p.setProperty("smartCatchGrade",String.valueOf(grade));p.setProperty("smartCatchCurve",String.valueOf(curve));
        p.setProperty("smartCatchCandySpecies",candySpecies);
    }
    public synchronized String config(){
        return "1;"+(enabled?1:0)+";"+ball+";"+berry+";"+(premium?1:0)+";"+highCp+";"+highLevel+";"+grade+";"+(curve?1:0)+";"+candySpecies;
    }
    public static boolean matchesSpecies(int id,String name,String query){
        String q=query==null?"":query.trim().toLowerCase(java.util.Locale.US);
        if(q.startsWith("#"))q=q.substring(1).trim();
        return q.isEmpty()||String.valueOf(id).contains(q)||(name!=null&&name.toLowerCase(java.util.Locale.US).contains(q));
    }
    public synchronized boolean enabled(){return enabled;}
    public synchronized boolean premium(){return premium;}
    public synchronized boolean curve(){return curve;}
    public synchronized int ball(){return ball;}
    public synchronized int berry(){return berry;}
    public synchronized int highCp(){return highCp;}
    public synchronized int highLevel(){return highLevel;}
    public synchronized int grade(){return grade;}
    public synchronized String candySpecies(){return candySpecies;}
    /** Validates a complete prospective snapshot before replacing any state. */
    public synchronized void set(String suffix,String value){
        if(suffix==null||value==null)throw new IllegalArgumentException("Setting key and value are required.");
        Properties p=new Properties();write(p);p.setProperty("smartCatch"+suffix,value);
        SmartCatchPreferences candidate=new SmartCatchPreferences();candidate.read(p);
        Properties normalized=new Properties();candidate.write(normalized);
        if(!value.equals(normalized.getProperty("smartCatch"+suffix)) && !"CandySpecies".equals(suffix))
            throw new IllegalArgumentException("Invalid smart catch "+suffix);
        if("CandySpecies".equals(suffix))p.setProperty("smartCatchCandySpecies",normalizeSpecies(value));
        read(p);
    }
}
