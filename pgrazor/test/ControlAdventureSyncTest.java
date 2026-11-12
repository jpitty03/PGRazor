package dev.pgrazor;

/** Runs the real native-transport permission gate, independently of Android grants. */
public final class ControlAdventureSyncTest {
    private static void set(String name,Object value) throws Exception {
        java.lang.reflect.Field f=Control.class.getDeclaredField(name);
        f.setAccessible(true);f.set(null,value);
    }
    private static void check(boolean expected,String reason) {
        if(Control.adventureSyncDismissAllowed()!=expected)throw new AssertionError(reason);
    }
    public static void main(String[] args) throws Exception {
        check(false,"no attached app");
        set("foreground",true);Control.connect();
        android.app.Application app=new android.app.Application(){
            public int checkSelfPermission(String name){throw new AssertionError("Dismissal must not query OS grants");}
        };
        set("hostApplication",app);set("desiredEnabled",true);set("skipCutscenes",false);
        check(true,"spoofing alone enables dismissal");
        set("skipCutscenes",true);check(true,"skip preference irrelevant");
        set("desiredEnabled",false);check(false,"spoofing off even with Skip cutscenes on");
        set("desiredEnabled",true);set("foreground",false);check(false,"background suspended");
        set("foreground",true);android.os.SystemClock.now+=9000;check(false,"stale host suspended");
        Control.renewLease();check(true,"live host restored");
        set("hostApplication",null);check(false,"detached app suspended");
        System.out.println("PASS Adventure Sync gate: spoofing on/off, independent of grants and cutscenes, host lifecycle");
    }
}
