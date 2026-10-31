package dev.pgrazor;
import java.util.Properties;
public final class GymAutomationPreferencesTest {
    static int checks;
    static void eq(Object expected,Object actual){checks++;if(!expected.equals(actual))throw new AssertionError(expected+" != "+actual);}
    static void reject(Runnable action){checks++;try{action.run();}catch(IllegalArgumentException expected){return;}throw new AssertionError("Accepted invalid input");}
    public static void main(String[] args){
        GymAutomationPreferences p=new GymAutomationPreferences();
        String defaults="1;0;0;0;50;50";
        eq(defaults,p.config());
        String[][] values={{"Deploy","true"},{"Feed","true"},{"Fallback","true"},{"NanabReserve","1000"},{"RazzReserve","0"}};
        for(String[] v:values)p.set(v[0],v[1]);
        eq("1;1;1;1;1000;0",p.config());
        Properties saved=new Properties();p.write(saved);eq(5,saved.size());
        GymAutomationPreferences restored=new GymAutomationPreferences();restored.read(saved);eq(p.config(),restored.config());
        restored.read(new Properties());eq(defaults,restored.config()); // account with no saved keys must reset everything
        String[][] invalid={{"Deploy","yes"},{"Feed","1"},{"Fallback","TRUE"},{"NanabReserve","-1"},{"RazzReserve","1001"}};
        Properties bad=new Properties();for(String[] v:invalid)bad.setProperty("gymAutomation"+v[0],v[1]);
        p.read(bad);eq(defaults,p.config()); // corrupt stored values fall back to defaults
        for(String[] v:invalid){String before=p.config();reject(()->p.set(v[0],v[1]));eq(before,p.config());}
        reject(()->p.set("Unknown","1"));
        reject(()->p.set(null,"1"));reject(()->p.set("Deploy",null));
        eq(defaults,p.config());
        // Reserve bounds: accept endpoints, reject out of range and non-canonical input.
        p.set("NanabReserve","0");eq(0,p.nanabReserve());
        p.set("NanabReserve","1000");eq(1000,p.nanabReserve());
        reject(()->p.set("NanabReserve","1001"));
        reject(()->p.set("NanabReserve"," 50 "));
        reject(()->p.set("RazzReserve","x"));
        p.set("RazzReserve","750");eq(750,p.razzReserve());
        // Flags accept canonical booleans only.
        p.set("Feed","false");eq(false,p.feed());
        p.set("Feed","true");eq(true,p.feed());
        p.set("Deploy","false");p.set("Fallback","false");
        eq("1;0;1;0;1000;750",p.config());
        System.out.println("GymAutomationPreferences: "+checks+" checks passed");
    }
}
