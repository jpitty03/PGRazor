package dev.pgrazor;

/** Runs real Control code; Android lifecycle/clock shells are supplied by test_control_java.py. */
public final class ControlQuickSummaryTest {
    private static void check(boolean value,String message) {
        if(!value)throw new AssertionError(message);
    }
    private static void foreground(boolean value) throws Exception {
        java.lang.reflect.Field field=Control.class.getDeclaredField("foreground");
        field.setAccessible(true); field.setBoolean(null,value);
    }
    public static void main(String[] args) throws Exception {
        check(!Control.quickSummaryConfigured() && !Control.quickSummaryEnabled() && !Control.quickSummaryAllowed(),"OFF in fresh process");
        Control.setQuickSummaryReady(true,"Premature ready"); Control.setQuickSummaryEnabled(true);
        check(!Control.quickSummaryReady() && Control.quickSummaryEnabled(),"Unconfigured preserves choice without readiness");
        Control.configureQuickSummary(true);
        check(!Control.quickSummaryReady() && Control.quickSummaryEnabled(),"Configuration preserves choice");
        Control.setQuickSummaryEnabled(true);
        check(Control.quickSummaryEnabled(),"Choice can be set before readiness");

        Control.engine.position(12.5,45.25); Control.engine.speed(7);
        Control.engine.walkRoute(new double[][]{{12.6,45.3},{12.7,45.4}});
        MotionEngine.RouteProgress original=Control.engine.routeProgress();
        boolean desired=Control.desiredEnabled();
        Control.setQuickSummaryReady(true,"Ready"); Control.setQuickSummaryEnabled(true);
        check(Control.quickSummaryEnabled() && !Control.quickSummaryAllowed(),"Feature intent is gated by runtime");
        foreground(true);
        check(!Control.quickSummaryAllowed(),"Foreground alone does not bypass backend readiness");
        Control.connect(); // Handler stub intentionally does not execute UI restoration.
        check(Control.quickSummaryAllowed(),"Ready connected foreground allows feature");
        Control.setAutoTransferReady(true,"Ready");Control.setAutoTransferEnabled(true);
        Control.setTransferKeepAll(true);
        check(Control.autoTransferAllowed()&&Control.transferKeepAll(),"Manual Keep All preserves background permission and its own flag");
        Control.setAutoTransferEnabled(false);
        check(!Control.autoTransferAllowed(),"Main master off blocks background transfer even with Keep All");
        Control.setTransferKeepAll(false);
        check(!Control.autoTransferAllowed(),"Overlay cannot enable the main transfer master");
        foreground(false);
        check(!Control.quickSummaryAllowed() && Control.quickSummaryEnabled(),"Background suspends permission without changing intent");
        foreground(true);
        android.os.SystemClock.now+=9000;
        check(!Control.quickSummaryAllowed(),"Expired connection lease blocks shortcut");
        Control.renewLease();
        check(Control.quickSummaryAllowed(),"Renewed lease restores runtime gate");

        // Reestablish movement after connect(), whose intentional pause is unrelated to toggling.
        Control.engine.position(12.5,45.25); Control.engine.speed(7);
        Control.engine.walkRoute(new double[][]{{12.6,45.3},{12.7,45.4}});
        Control.setQuickSummaryEnabled(false); Control.setQuickSummaryEnabled(true);
        Control.setQuickSummaryReady(false,"Hook unavailable");
        check(!Control.quickSummaryReady() && Control.quickSummaryEnabled() && !Control.quickSummaryAllowed(),"Readiness loss gates execution without clearing choice");
        Control.setQuickSummaryReady(true,"Ready again");
        check(Control.quickSummaryEnabled() && Control.quickSummaryAllowed(),"Recovery restores saved choice");
        check(Control.engine.latitude==12.5 && Control.engine.longitude==45.25 && Control.engine.speedKmh==7,"Feature calls preserve coordinates/speed");
        MotionEngine.RouteProgress after=Control.engine.routeProgress();
        check(Control.engine.enabled && Control.engine.walking && after.total==original.total && after.visited==original.visited && after.remainingMeters==original.remainingMeters,"Feature calls preserve route and enabled state");
        check(Control.desiredEnabled()==desired,"Feature calls preserve saved location intent");
        check(android.util.Log.lines.stream().anyMatch(line->line.contains("[quick-summary-setting] ON")),"Enable log emitted");
        check(android.util.Log.lines.stream().anyMatch(line->line.contains("Suspended; saved choice retained")),"Readiness-loss log emitted");

        Control.setQuickSummaryEnabled(true); Control.disconnect();
        check(!Control.quickSummaryReady() && Control.quickSummaryEnabled(),"Disconnect resets readiness and retains choice");
        Control.setQuickSummaryReady(true,"Ready"); Control.setQuickSummaryEnabled(true);
        Control.configureQuickSummary(false);
        check(!Control.quickSummaryConfigured() && !Control.quickSummaryReady() && Control.quickSummaryEnabled() && Control.quickSummaryStatus().isEmpty(),"Removing configuration clears readiness and retains choice");
        Control.configureQuickSummary(true); Control.setQuickSummaryReady(true,"Ready"); Control.setQuickSummaryEnabled(true);
        Control.embedded("Restarting adapter");
        check(!Control.quickSummaryReady() && Control.quickSummaryEnabled(),"Adapter initialization retains choice with readiness off");
        Control.configureQuickSummaryAutoDismiss(true);
        Control.configureQuickSummary(true);
        check(Control.quickSummaryAutoDismiss(),"Packaging mode survives summary configuration");
        check(Control.quickSummaryEnabled(),"Auto-dismiss configuration retains choice");
        Control.setQuickSummaryReady(true,"Ready"); Control.setQuickSummaryEnabled(false); Control.setQuickSummaryEnabled(true);
        check(android.util.Log.lines.stream().anyMatch(line->line.contains("ON; summary and details auto-dismiss; new-species screens remain")),"Auto-dismiss enable log is accurate");
        Control.embedded("Restarting auto-dismiss adapter");
        check(Control.quickSummaryAutoDismiss() && Control.quickSummaryEnabled(),"Readiness resets preserve mode and choice");
        Control.configureQuickSummaryAutoDismiss(false);
        check(!Control.quickSummaryAutoDismiss(),"Wait-only mode remains distinct");
        System.out.println("PASS: real Control summary default/readiness/lifecycle/lease gates, movement preservation, and logs");
    }
}
