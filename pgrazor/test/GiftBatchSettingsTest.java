package dev.pgrazor;

public final class GiftBatchSettingsTest {
    private static void check(boolean ok) {if(!ok)throw new AssertionError();}
    private static void event(String token,String phase) {
        GiftBatchSettings.publish("{\"token\":\""+token+"\",\"phase\":\""+phase+"\",\"sent\":\"2\",\"opened\":\"3\",\"skipped\":\"1\"}");
    }
    public static void main(String[] args) {
        GiftBatchSettings.reset(); Control.ready=true;
        check(GiftBatchSettings.config()==null);
        GiftBatchSettings.start(true);
        String token=GiftBatchSettings.config().split("\\|")[1];
        check(GiftBatchSettings.config().endsWith("|1|1"));
        event("stale","done"); check(GiftBatchSettings.running());
        GiftBatchSettings.stop(); check(GiftBatchSettings.config().endsWith("|0|1"));
        event(token,"running"); check(GiftBatchSettings.stopPending());
        event(token,"stopped"); check(!GiftBatchSettings.running());
        GiftBatchSettings.start(false); check(GiftBatchSettings.config().endsWith("|1|0"));
        event(token,"done"); check(GiftBatchSettings.running());
        token=GiftBatchSettings.config().split("\\|")[1];
        Control.ready=false; check(GiftBatchSettings.stopPending());
        event(token,"stopped"); check(GiftBatchSettings.config()==null);
        Control.ready=true; GiftBatchSettings.start(true);
        android.os.SystemClock.now+=1800001; check(GiftBatchSettings.stopPending());
        GiftBatchSettings.reset(); check(!GiftBatchSettings.running());
        BatchEvolutionSettings.active=true;
        try {GiftBatchSettings.start(true); throw new AssertionError();} catch(IllegalStateException expected) {}
        BatchEvolutionSettings.active=false;
        System.out.println("PASS GiftBatchSettings: stale events, cancel drain, readiness loss, timeout, task exclusion");
    }
}
