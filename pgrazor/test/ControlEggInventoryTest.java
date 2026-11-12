package dev.pgrazor;

/** Auto incubation must reach the screen policy even when the current screen is not the map. */
public final class ControlEggInventoryTest {
    private static void set(String name,boolean value)throws Exception{
        java.lang.reflect.Field f=Control.class.getDeclaredField(name);f.setAccessible(true);f.set(null,value);
    }
    public static void main(String[] args)throws Exception{
        for(boolean skip:new boolean[]{false,true})for(boolean incubate:new boolean[]{false,true}){
            set("skipCutscenes",skip);set("autoIncubate",incubate);
            String expected="K2|"+(skip?1:0)+"|"+(incubate?1:0);
            if(!Control.cutsceneSkipConfig().equals(expected))throw new AssertionError(expected);
            if(Control.incubatorAllowed())throw new AssertionError("Fixture must not be map-ready");
        }
        System.out.println("PASS egg inventory policy: independent Auto incubate and Skip cutscenes, no map-ready dependency");
    }
}
