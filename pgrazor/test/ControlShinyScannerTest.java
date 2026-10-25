package dev.pgrazor;

/** Real bridge policy with the same clock/lifecycle shells as other Control tests. */
public final class ControlShinyScannerTest {
    private static void check(boolean condition,String message){if(!condition)throw new AssertionError(message);}
    private static void field(String name,Object value)throws Exception{
        java.lang.reflect.Field f=Control.class.getDeclaredField(name);f.setAccessible(true);f.set(null,value);
    }
    public static void main(String[] args)throws Exception{
        check(!Control.shinyScannerEnabled()&&!Control.shinyScannerAllowed(),"Scanner defaults off");
        Control.setShinyScannerEnabled(true);
        check(!Control.shinyScannerAllowed(),"Preference cannot bypass readiness");
        Control.engine.position(42.65,23.34);
        field("foreground",true);Control.connect();Control.renewLease();
        Control.setShinyScannerReady(true,"Ready");
        field("buddyMapSample",android.os.SystemClock.now);
        check(Control.shinyScannerAllowed(),"Fresh foreground map permits scanner");
        field("vgpOverlay",true);field("vgpEnabled",true);
        check(!Control.shinyScannerAllowed(),"VGP blocks scanner requests");
        field("vgpEnabled",false);
        check(Control.shinyScannerAllowed(),"VGP off resumes eligibility");
        android.os.SystemClock.now+=1600;
        check(!Control.shinyScannerAllowed(),"Stale map sample blocks scanner");
        check(!Control.shinyScannerOverlayVisible(),"Overlay hidden outside map");
        field("buddyMapSample",android.os.SystemClock.now);
        field("foreground",false);
        check(!Control.shinyScannerAllowed(),"Background blocks scanner");
        field("foreground",true);
        Control.setShinyScannerReady(false,"Unsupported");
        check(!Control.shinyScannerAllowed()&&Control.shinyScannerEnabled(),"Unavailable preserves preference but blocks requests");
        Control.setShinyScannerReady(true,"Ready");Control.setShinyScannerEnabled(false);
        check(!Control.shinyScannerAllowed(),"User disable blocks scanner");
        System.out.println("PASS: scanner readiness, foreground, map freshness, VGP and preference gates");
    }
}
