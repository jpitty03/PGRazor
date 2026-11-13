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
            check(manual.length==6&&manual[0].equals("C2")&&manual[2].equals("0"));
            check(manual[5].equals((mask&2)!=0?"1":"0"));
        }
        System.out.println("PASS Go Wild independent wire flags; base features remain off");
    }
}
