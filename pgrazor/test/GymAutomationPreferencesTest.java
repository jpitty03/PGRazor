package dev.pgrazor;

import java.util.Properties;

public final class GymAutomationPreferencesTest {
    static int checks;
    static void eq(Object expected,Object actual){checks++;if(!expected.equals(actual))throw new AssertionError(expected+" != "+actual);}
    static void reject(Runnable action){checks++;try{action.run();}catch(IllegalArgumentException expected){return;}throw new AssertionError("Accepted invalid input");}

    public static void main(String[] args){
        GymAutomationPreferences p=new GymAutomationPreferences();
        String defaults="1;0;0;0;50;50";
        eq(defaults,p.config());eq(false,p.running());eq(true,p.overlay());
        eq(false,p.start());

        p.set("Deploy","true");p.set("Feed","true");p.set("Fallback","true");
        p.set("NanabReserve","1000");p.set("RazzReserve","0");
        eq(false,p.running());eq("1;0;0;1;1000;0",p.config());
        eq(true,p.start());eq("1;1;1;1;1000;0",p.config());
        Properties saved=new Properties();p.write(saved);eq(6,saved.size());
        GymAutomationPreferences restored=new GymAutomationPreferences();restored.read(saved);
        eq(false,restored.running());eq(true,restored.deploy());eq(true,restored.feed());
        eq("1;0;0;1;1000;0",restored.config());
        restored.read(new Properties());eq(defaults,restored.config());

        eq(false,p.stopForCooldown(false));eq(true,p.running());
        eq(true,p.stopForCooldown(true));eq(false,p.running());
        eq(true,p.deploy());eq(true,p.feed());eq("1;0;0;1;1000;0",p.config());
        eq(false,p.stopForCooldown(true));eq(false,p.running());
        eq(true,p.start());
        p.set("NanabReserve","123");eq(true,p.running());
        p.set("Fallback","false");eq(true,p.running());
        p.set("Overlay","false");eq(true,p.running());eq(false,p.overlay());
        p.set("Deploy","false");eq(true,p.running());eq("1;0;1;0;123;0",p.config());
        p.set("Feed","false");eq(false,p.running());eq("1;0;0;0;123;0",p.config());
        eq(false,p.start());
        p.set("Feed","true");eq(false,p.running());eq(true,p.start());
        p.stop();eq(false,p.running());eq(true,p.feed());

        Properties overlaySaved=new Properties();p.write(overlaySaved);
        GymAutomationPreferences overlayRestored=new GymAutomationPreferences();overlayRestored.read(overlaySaved);
        eq(false,overlayRestored.overlay());eq(false,overlayRestored.running());
        String[][] invalid={{"Deploy","yes"},{"Feed","1"},{"Fallback","TRUE"},
            {"Overlay","yes"},{"NanabReserve","-1"},{"RazzReserve","1001"}};
        for(String[] value:invalid){String before=p.config();reject(()->p.set(value[0],value[1]));eq(before,p.config());}
        reject(()->p.set("Unknown","1"));reject(()->p.set(null,"1"));reject(()->p.set("Deploy",null));
        reject(()->p.set("NanabReserve"," 50 "));
        p.read(new Properties());eq(defaults,p.config());eq(false,p.running());
        System.out.println("GymAutomationPreferences: "+checks+" checks passed");
    }
}
