package dev.pgrazor;

/**
 * Runs real Control VGP policy to exercise VgpController state transitions; the Android shells and
 * the controllable Cooldown clock are supplied by test_control_java.py. These transitions (cooldown
 * refusal, guard trip, no auto-resume, background off, overlay off) were previously only covered by
 * device testing.
 */
public final class VgpControllerTest {
    private static void check(boolean value,String message) { if(!value)throw new AssertionError(message); }
    private static void foreground(boolean value) throws Exception {
        java.lang.reflect.Field field=Control.class.getDeclaredField("foreground");
        field.setAccessible(true); field.setBoolean(null,value);
    }
    private static boolean logged(String kind,String detail) {
        return android.util.Log.lines.stream().anyMatch(line->line.contains("["+kind+"] "+detail));
    }
    public static void main(String[] args) throws Exception {
        // Overlay off: the feature cannot be active regardless of an enable request.
        check(!Control.vgpOverlay() && !Control.vgpEnabled(),"Default overlay/active off");
        foreground(true);
        Control.setVgpEnabled(true);
        check(!Control.vgpEnabled(),"Enable refused while overlay off");

        // Overlay on, no cooldown armed: an enable request turns the runtime flag on.
        Control.setVgpOverlay(true);
        Control.setVgpEnabled(true);
        check(Control.vgpEnabled() && Control.vgpOverlay(),"Enabled with overlay on in foreground");
        check(logged("vgp-enabled","ON; runtime only"),"Enable logged");
        java.util.Properties spin=new java.util.Properties();
        spin.setProperty("smartCatchEnabled","true");spin.setProperty("smartCatchSpinGyms","true");
        SmartCatchSettings.read(spin);
        Control.event("gym-spin","{\"message\":\"Gym spun\"}");
        check(Control.vgpStatus().equals("Gym spun"),"Gym spin reports through VGP status");
        SmartCatchSettings.read(new java.util.Properties());Control.setVgpStatus("Ready");
        Control.event("gym-spin","{\"message\":\"Gym spun\"}");
        check(Control.vgpStatus().equals("Ready"),"Late gym spin status ignored when VGP+ gym spin is disabled");

        // Arming the guard with no live cooldown keeps VGP on.
        Control.setVgpStopOnCooldown(true);
        check(Control.vgpStopOnCooldown() && Control.vgpEnabled(),"Arming guard without cooldown keeps VGP on");

        // A live cooldown trips VGP off through the heartbeat and shows the sticky pause reason.
        Cooldown.remaining=5000;
        check(!Control.vgpEnabled(),"Live cooldown trips VGP off");
        check(Control.vgpStatus().equals("Paused for active cooldown; tap VGP to resume once it clears"),"Pause reason shown");
        check(logged("vgp-cooldown-guard","VGP switched off; cooldown active"),"Guard trip logged");

        // No auto-resume: clearing the cooldown does not turn VGP back on.
        Cooldown.remaining=0;
        check(!Control.vgpEnabled(),"No auto-resume after cooldown clears");

        // Only an explicit tap re-enables, once the cooldown is clear.
        Control.setVgpEnabled(true);
        check(Control.vgpEnabled(),"Manual re-enable after cooldown clears");

        // Enable is refused while a cooldown is live and the guard is armed.
        Control.setVgpEnabled(false);
        check(!Control.vgpEnabled(),"Explicit off");
        Cooldown.remaining=5000;
        Control.setVgpEnabled(true);
        check(!Control.vgpEnabled(),"Enable refused during live cooldown");
        check(Control.vgpStatus().equals("Cooldown active; tap VGP after it clears"),"Refusal reason shown");
        check(logged("vgp-cooldown-guard","Enable refused; cooldown active"),"Refusal logged");

        // Disarming the guard clears the lingering reason; status falls back to the native text.
        Control.setVgpStatus("Ready");
        Control.setVgpStopOnCooldown(false);
        check(Control.vgpStatus().equals("Ready"),"Disarming clears guard reason");

        // Background switches VGP off with its own reason and never auto-resumes.
        Cooldown.remaining=0;
        Control.setVgpEnabled(true);
        check(Control.vgpEnabled(),"On before backgrounding");
        Control.suspendForBackground();
        android.os.Handler.drain();
        check(!Control.vgpEnabled(),"Background switches VGP off");
        check(Control.vgpStatus().equals("App left foreground; tap VGP to start"),"Background reason shown");

        // Returning to the foreground and turning the overlay off clears the runtime enable.
        foreground(true);
        Control.setVgpOverlay(true);
        Control.setVgpEnabled(true);
        check(Control.vgpEnabled(),"Re-enabled after returning to foreground");
        Control.setVgpOverlay(false);
        check(!Control.vgpOverlay() && !Control.vgpEnabled(),"Overlay off clears active VGP");

        // Target marker state round-trips and rejects out-of-range coordinates without clobbering.
        Control.setVgpTarget(true,0.25,0.75,true);
        check(Control.vgpTargetX()==0.25f && Control.vgpTargetY()==0.75f && Control.vgpTargetSpin(),"Marker stored");
        Control.setVgpTarget(true,2.0,0.5,false);
        check(Control.vgpTargetX()==0.25f,"Out-of-range marker leaves prior coordinates intact");

        // The option mask is a saved preference, independent of the runtime flag.
        Control.setVgpOptions(5);
        check(Control.vgpOptions()==5,"Option mask stored");

        // Exercise the actual Java permission boundary, not a source slice evaluated as JS.
        Control.configureQuickSummary(true); Control.setQuickSummaryReady(true,"Ready");
        Control.setItemReady(true,"Ready"); Control.setVgpReady(true,"Ready");
        Control.connect(); foreground(true); Control.setVgpOverlay(true); Control.setVgpEnabled(true);
        check(Control.vgpAllowed(),"Off-map foreground may run VGP");
        foreground(false); check(!Control.vgpAllowed(),"Background blocks permission"); foreground(true);
        Control.setVgpReady(false,"Unavailable"); check(!Control.vgpAllowed(),"Native readiness blocks permission");
        Control.setVgpReady(true,"Ready");
        java.lang.reflect.Field desired=Control.class.getDeclaredField("desiredEnabled"); desired.setAccessible(true);
        desired.setBoolean(null,false); check(!Control.vgpAllowed(),"Unknown location blocks permission"); desired.setBoolean(null,true);
        Control.setVgpStopOnCooldown(true); Cooldown.remaining=5000;
        check(!Control.vgpAllowed() && !Control.vgpEnabled(),"Cooldown revokes permission and active state");

        System.out.println("PASS: real Control VGP enable, cooldown refusal/trip, no auto-resume, background off, overlay off, marker bounds");
    }
}
