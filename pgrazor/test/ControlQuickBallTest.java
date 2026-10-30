package dev.pgrazor;

/** Runs real Control code; Android lifecycle/clock shells are supplied by test_control_java.py. */
public final class ControlQuickBallTest {
    private static void check(boolean value,String message) {
        if(!value)throw new AssertionError(message);
    }
    private static void foreground(boolean value) throws Exception {
        java.lang.reflect.Field field=Control.class.getDeclaredField("foreground");
        field.setAccessible(true); field.setBoolean(null,value);
    }
    public static void main(String[] args) throws Exception {
        check(!Control.quickBallConfigured() && !Control.quickBallEnabled() && !Control.quickBallAllowed(),"OFF in fresh process");
        Control.setQuickBallReady(true,"Premature ready"); Control.setQuickBallEnabled(true);
        check(!Control.quickBallReady() && Control.quickBallEnabled(),"Unconfigured preserves choice without readiness");
        Control.configureQuickBall(true);
        check(!Control.quickBallReady() && Control.quickBallEnabled(),"Configuration preserves choice");
        Control.setQuickBallEnabled(true);
        check(Control.quickBallEnabled(),"Choice can be set before readiness");

        Control.engine.position(12.5,45.25); Control.engine.speed(7);
        Control.engine.walkRoute(new double[][]{{12.6,45.3},{12.7,45.4}});
        MotionEngine.RouteProgress original=Control.engine.routeProgress();
        boolean desired=Control.desiredEnabled();
        Control.setQuickBallReady(true,"Ready"); Control.setQuickBallEnabled(true);
        check(Control.quickBallEnabled() && !Control.quickBallAllowed(),"Feature intent is gated by runtime");
        foreground(true);
        check(!Control.quickBallAllowed(),"Foreground alone does not bypass backend readiness");
        Control.connect(); // Handler stub intentionally does not execute UI restoration.
        check(Control.quickBallAllowed(),"Ready connected foreground allows feature");
        foreground(false);
        check(!Control.quickBallAllowed() && Control.quickBallEnabled(),"Background suspends permission without changing intent");
        foreground(true);
        android.os.SystemClock.now+=9000;
        check(!Control.quickBallAllowed(),"Expired connection lease blocks shortcut");
        Control.renewLease();
        check(Control.quickBallAllowed(),"Renewed lease restores runtime gate");

        // Reestablish movement after connect(), whose intentional pause is unrelated to toggling.
        Control.engine.position(12.5,45.25); Control.engine.speed(7);
        Control.engine.walkRoute(new double[][]{{12.6,45.3},{12.7,45.4}});
        Control.setQuickBallEnabled(false); Control.setQuickBallEnabled(true);
        Control.setQuickBallReady(false,"Hook unavailable");
        check(!Control.quickBallReady() && Control.quickBallEnabled() && !Control.quickBallAllowed(),"Readiness loss gates execution without clearing choice");
        Control.setQuickBallReady(true,"Ready again");
        check(Control.quickBallEnabled() && Control.quickBallAllowed(),"Recovery restores saved choice");
        check(Control.engine.latitude==12.5 && Control.engine.longitude==45.25 && Control.engine.speedKmh==7,"Feature calls preserve coordinates/speed");
        MotionEngine.RouteProgress after=Control.engine.routeProgress();
        check(Control.engine.enabled && Control.engine.walking && after.total==original.total && after.visited==original.visited && after.remainingMeters==original.remainingMeters,"Feature calls preserve route and enabled state");
        check(Control.desiredEnabled()==desired,"Feature calls preserve saved location intent");
        check(android.util.Log.lines.stream().anyMatch(line->line.contains("[quick-ball-setting] ON")),"Enable log emitted");
        check(android.util.Log.lines.stream().anyMatch(line->line.contains("Suspended; saved choice retained")),"Readiness-loss log emitted");

        Control.setQuickBallEnabled(true); Control.disconnect();
        check(!Control.quickBallReady() && Control.quickBallEnabled(),"Disconnect resets readiness and retains choice");
        Control.setQuickBallReady(true,"Ready"); Control.setQuickBallEnabled(true);
        Control.configureQuickBall(false);
        check(!Control.quickBallConfigured() && !Control.quickBallReady() && Control.quickBallEnabled() && Control.quickBallStatus().isEmpty(),"Removing configuration clears readiness and retains choice");
        Control.configureQuickBall(true); Control.setQuickBallReady(true,"Ready"); Control.setQuickBallEnabled(true);
        Control.embedded("Restarting adapter");
        check(!Control.quickBallReady() && Control.quickBallEnabled(),"Adapter initialization retains choice with readiness off");
        Control.engine.position(0,0);
        Control.engine.walkRoute(new double[][]{{0,0}});
        long arrivedToken=Control.engine.routeGeneration();
        Control.invalidateCameraHeading("Camera not visible");
        Control.cameraSteeringStatus();
        check(Control.engine.routeGeneration()==arrivedToken && Control.engine.routeProgress().visited==1,
            "Real Control camera invalidation preserves completed route ownership");
        Control.engine.input(1,0);Control.engine.enabled=true;
        Control.invalidateCameraHeading("Camera not visible");
        check(Control.engine.metersPerSecond()==0,"Real Control camera invalidation stops manual velocity");
        System.out.println("PASS: real Control ball default/readiness/lifecycle/lease gates, movement preservation, and logs");
    }
}
