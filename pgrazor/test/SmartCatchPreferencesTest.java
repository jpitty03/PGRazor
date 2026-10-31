package dev.pgrazor;
import java.util.Properties;
public final class SmartCatchPreferencesTest {
    static int checks;
    static void eq(Object expected,Object actual){checks++;if(!expected.equals(actual))throw new AssertionError(expected+" != "+actual);}
    static void reject(Runnable action){checks++;try{action.run();}catch(IllegalArgumentException expected){return;}throw new AssertionError("Accepted invalid input");}
    public static void main(String[] args){
        SmartCatchPreferences p=new SmartCatchPreferences();
        String defaults="1;0;1;0;1;1500;30;3;1;";
        eq(defaults,p.config());
        eq(false,p.spinGyms());
        eq(true,SmartCatchPreferences.matchesSpecies(25,"Pikachu","PIKA"));
        eq(true,SmartCatchPreferences.matchesSpecies(25,"Pikachu"," #25 "));
        eq(true,SmartCatchPreferences.matchesSpecies(25,"",""));
        eq(false,SmartCatchPreferences.matchesSpecies(25,"Pikachu","Eevee"));
        String[][] values={{"Enabled","true"},{"Ball","3"},{"Berry","708"},{"Premium","false"},{"HighCp","10000"},{"HighLevel","50"},{"Grade","0"},{"Curve","false"},{"CandySpecies"," 25,1,25,2000 "}};
        for(String[] v:values)p.set(v[0],v[1]);
        eq("1;1;3;708;0;10000;50;0;0;1,25,2000",p.config());
        p.set("SpinGyms","true");eq(true,p.spinGyms());eq("1;1;3;708;0;10000;50;0;0;1,25,2000",p.config());
        Properties saved=new Properties();p.write(saved);eq(10,saved.size());
        SmartCatchPreferences restored=new SmartCatchPreferences();restored.read(saved);eq(p.config(),restored.config());eq(true,restored.spinGyms());
        Properties legacy=new Properties();legacy.setProperty("gymAutomationSpin","true");
        restored.read(legacy);eq(true,restored.spinGyms());
        restored.write(legacy);eq(false,legacy.containsKey("gymAutomationSpin"));eq("true",legacy.getProperty("smartCatchSpinGyms"));
        legacy.setProperty("smartCatchSpinGyms","false");legacy.setProperty("gymAutomationSpin","true");
        restored.read(legacy);eq(false,restored.spinGyms()); // New key wins over legacy.
        restored.read(new Properties());eq(defaults,restored.config());eq(false,restored.spinGyms());
        String[][] invalid={{"Enabled","yes"},{"Ball","4"},{"Berry","707"},{"Premium","TRUE"},{"HighCp","-1"},{"HighLevel","51"},{"Grade","4"},{"Curve","1"},{"SpinGyms","1"},{"CandySpecies","0"}};
        Properties bad=new Properties();for(String[] v:invalid)bad.setProperty("smartCatch"+v[0],v[1]);
        p.read(bad);eq(defaults,p.config());
        for(String[] v:invalid){String before=p.config();reject(()->p.set(v[0],v[1]));eq(before,p.config());}
        reject(()->p.set("Unknown","1"));
        reject(()->p.set(null,"1"));reject(()->p.set("Ball",null));
        eq(defaults,p.config());
        for(String value:new String[]{"1,,2","1,","x","0","2001","-1","1;2"})reject(()->SmartCatchPreferences.normalizeSpecies(value));
        eq("",SmartCatchPreferences.normalizeSpecies("  "));
        eq("1,25,2000",SmartCatchPreferences.normalizeSpecies("2000,25,1,25"));
        StringBuilder ids=new StringBuilder();for(int n=1;n<=200;n++){if(n>1)ids.append(',');ids.append(n);}
        eq(ids.toString(),SmartCatchPreferences.normalizeSpecies(ids.toString()));
        reject(()->SmartCatchPreferences.normalizeSpecies(ids+",201"));
        for(int berry:new int[]{0,701,703,705,706,708}){p.set("Berry",String.valueOf(berry));eq(berry,p.berry());}
        p.set("HighCp","0");p.set("HighLevel","0");p.set("Grade","3");p.set("Ball","1");
        eq(0,p.highCp());eq(0,p.highLevel());eq(3,p.grade());eq(1,p.ball());
        System.out.println("SmartCatchPreferences: "+checks+" checks passed");
    }
}
