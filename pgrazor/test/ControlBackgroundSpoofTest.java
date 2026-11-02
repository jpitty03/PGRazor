package dev.pgrazor;

import android.app.Activity;
import android.os.Handler;

/** PGRazorX: the spoofed location, a route (still moving) and VGP survive leaving the game; a lapsed lease is restored. */
public final class ControlBackgroundSpoofTest {
    private static void check(boolean value,String message) { if(!value)throw new AssertionError(message); }
    private static void set(String name,Object value) throws Exception {
        java.lang.reflect.Field field=Control.class.getDeclaredField(name); field.setAccessible(true); field.set(null,value);
    }
    private static boolean get(String name) throws Exception {
        java.lang.reflect.Field field=Control.class.getDeclaredField(name); field.setAccessible(true); return field.getBoolean(null);
    }
    /** The away tick reposts itself, so it is run one round at a time instead of drained. */
    private static void runQueuedOnce() { java.util.List<Runnable> now=new java.util.ArrayList<>(Handler.queue); Handler.queue.clear(); for(Runnable r:now)r.run(); }
    public static void main(String[] args) throws Exception {
        set("registered",true); // Control.begin's lifecycle registration without the Android application
        Control.connect(); Handler.drain();
        check(Control.engine.enabled && Control.active(),"Ready adapter enables the saved spoof, even before the game is in front");
        Control.attach(new Activity()); Handler.drain();
        check(Control.active(),"Active in the foreground");
        Control.engine.position(10,20);
        Control.walkRoute(new double[][]{{10.001,20},{10.002,20}},true);
        check(Control.engine.walking,"Route running");

        Control.setVgpOverlay(true); Control.setVgpEnabled(true);
        check(get("vgpEnabled"),"VGP on in the foreground");

        // Leaving the game (backup menu, file picker, another app): location, route and VGP stay.
        Control.suspendForBackground(); set("pad",null);
        check(Control.active(),"Spoof kept while away");
        check(Control.engine.walking,"Route kept while away");
        check(get("vgpEnabled"),"VGP stays on while away");
        double lat=Control.engine.latitude;
        android.os.SystemClock.now+=1000; runQueuedOnce();
        check(Control.engine.latitude!=lat,"Route keeps moving while away");
        Control.attach(new Activity()); Handler.drain();
        check(Control.active() && Control.engine.walking,"Back in the game: spoof and route continue");
        check(get("vgpEnabled"),"VGP still on back in the game");

        // A heartbeat that lapsed while away: the real location passes meanwhile; on return the spoof is restored
        // by the next heartbeat (previously it could stay off).
        Control.suspendForBackground(); set("pad",null);
        android.os.SystemClock.now+=9000;
        check(!Control.active(),"Late heartbeat hands the game its real location");
        Control.attach(new Activity()); Handler.drain();
        check(!Control.engine.enabled,"Expired lease suspends on return");
        Control.renewLease(); Handler.drain();
        check(Control.engine.enabled && Control.active(),"Next heartbeat restores the spoof");
        System.out.println("PASS: background spoof kept with route (still moving) and VGP, lapsed lease restored on return");
    }
}
