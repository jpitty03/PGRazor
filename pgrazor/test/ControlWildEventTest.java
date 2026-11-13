package dev.pgrazor;

/** Actual Control wire construction; EventSettings persistence has its own fixture. */
public final class ControlWildEventTest {
    private static void check(boolean value) { if (!value) throw new AssertionError("Go Wild wire mismatch"); }
    public static void main(String[] args) throws Exception {
        java.lang.reflect.Field host=Control.class.getDeclaredField("hostApplication");host.setAccessible(true);
        dev.pgrazor.events.EventSettings.vgpSafari=true;
        dev.pgrazor.events.EventSettings.autoSafari=true;
        check(Control.smartCatchConfig().endsWith(";0"));
        check(Control.catchChanceConfig().endsWith("|0"));
        host.set(null,new android.app.Application());
        for(int mask=0;mask<4;mask++) {
            dev.pgrazor.events.EventSettings.vgpSafari=(mask&1)!=0;
            dev.pgrazor.events.EventSettings.autoSafari=(mask&2)!=0;
            String[] smart=Control.smartCatchConfig().split(";",-1);
            String[] manual=Control.catchChanceConfig().split("\\|",-1);
            check(smart.length==15&&smart[0].equals("5")&&smart[1].equals("0"));
            check(smart[14].equals((mask&1)!=0?"1":"0"));
            check(manual.length==9&&manual[0].equals("C4")&&manual[2].equals("0"));
            check(manual[5].equals((mask&2)!=0?"1":"0"));
        }
        check(!Control.autoThrow()&&!Control.autoThrowOnMap());
        Control.setAutoThrowOnMap(true);
        String[] wire=Control.catchChanceConfig().split("\\|",-1);
        check(wire[6].equals("0")&&wire[8].equals("1")); // The child never enables its parent.
        Control.setAutoThrow(true);
        wire=Control.catchChanceConfig().split("\\|",-1);
        check(wire[6].equals("1")&&wire[8].equals("1"));
        Control.setAutoThrow(false);
        check(Control.autoThrowOnMap()); // Preserve preference while parent is off.
        MapCatchStatus badge=new MapCatchStatus();
        badge.event("ready",0,100);check(badge.label(100).isEmpty());
        badge.event("tap",0,100);check(badge.label(100).equals("CATCH"));
        badge.event("catch-result",2,200);check(badge.label(200).equals("RETRY"));
        badge.event("throw",1,1400);check(badge.label(1400).equals("CATCH"));
        badge.event("caught",0,1600);check(badge.label(1700).equals("CAUGHT"));
        check(badge.label(3100).isEmpty());
        badge.event("tap",0,4000);badge.event("busy",0,23000);check(badge.label(24000).isEmpty());
        badge.event("throw",1,25000);badge.clear();check(badge.label(25000).isEmpty());
        System.out.println("PASS Go Wild independent wire flags; base features remain off");
    }
}
