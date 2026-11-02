package dev.pgrazor;

import android.app.Application;
import android.os.SystemClock;
import java.util.function.BiConsumer;
import java.util.function.BooleanSupplier;
import java.util.function.Consumer;
import java.util.function.IntSupplier;
import java.util.function.LongSupplier;

/**
 * Owns the VGP runtime-only state that must never persist: the guard reason, status text and the
 * on-screen target marker, plus the enable/cooldown-guard decision and the optional result
 * notification. Extracted from Control so this state has a single owner with explicit injected
 * dependencies.
 *
 * <p>The persistent VGP preferences (overlay, stop-on-cooldown, option mask) and the enabled/ready
 * flags stay as Control fields and are read/written through the accessors passed to the constructor,
 * so the runtime state lives in exactly one place and is never duplicated. The enabled/ready flags
 * remain at the Control bridge boundary, alongside its other feature readiness flags.
 *
 * <p>Locking: {@link #lock} serializes the compound enabled/target/guard decisions. The cooldown
 * clock is always read before the lock is taken, so the controller lock is never nested with
 * Cooldown's. The notification path and {@code saveSettings} run with no lock held. Turning VGP on
 * is refused while a live cooldown is guarded; a live cooldown trips it off through the heartbeat and
 * it stays off (no auto-resume); PGRazorX: losing the foreground keeps it on (account switches still turn it off).
 */
final class VgpController {
    /** Serializes the compound enabled/target/guard decisions; never held across Cooldown. */
    private final Object lock = new Object();
    /** Sticky reason for the current VGP pause/refusal; takes priority over native status when set. */
    private volatile String guardReason = "";
    private volatile String status = "Waiting for VGP support";
    private volatile float targetX, targetY;
    private volatile long targetAt;
    private volatile boolean targetSpin;
    private volatile Application application;

    private final BooleanSupplier enabled;
    private final Consumer<Boolean> enabledWriter;
    private final BooleanSupplier overlay;
    private final Consumer<Boolean> overlayWriter;
    private final BooleanSupplier stopOnCooldown;
    private final Consumer<Boolean> stopWriter;
    private final BooleanSupplier foreground;
    private final IntSupplier options;
    private final LongSupplier cooldownRemaining;
    private final Consumer<String> cooldownAction;
    private final BiConsumer<String,String> event;
    private final Runnable save;

    VgpController(BooleanSupplier enabled, Consumer<Boolean> enabledWriter,
                  BooleanSupplier overlay, Consumer<Boolean> overlayWriter,
                  BooleanSupplier stopOnCooldown, Consumer<Boolean> stopWriter,
                  BooleanSupplier foreground, IntSupplier options,
                  LongSupplier cooldownRemaining, Consumer<String> cooldownAction,
                  BiConsumer<String,String> event, Runnable save) {
        this.enabled = enabled; this.enabledWriter = enabledWriter;
        this.overlay = overlay; this.overlayWriter = overlayWriter;
        this.stopOnCooldown = stopOnCooldown; this.stopWriter = stopWriter;
        this.foreground = foreground; this.options = options;
        this.cooldownRemaining = cooldownRemaining; this.cooldownAction = cooldownAction;
        this.event = event; this.save = save;
    }

    void setApplication(Application app) { application = app; }

    /** The overlay toggle persists; turning it off also clears the runtime enable, target and guard. */
    void setOverlay(boolean value) {
        synchronized(lock) { overlayWriter.accept(value); if(!value)enabledWriter.accept(false); targetAt=0; guardReason=""; }
        save.run();
    }
    /**
     * VGP active state is runtime only (never persisted). Turning it on while the cooldown guard is
     * armed and a cooldown is live is refused with a visible reason; turning it off clears the guard.
     */
    void setEnabled(boolean value) {
        if(!value){synchronized(lock){enabledWriter.accept(false);targetAt=0;guardReason="";status="Off";}return;}
        // Read Cooldown before taking the lock so the two locks are never nested.
        boolean cooldownActive=stopOnCooldown.getAsBoolean()&&cooldownRemaining.getAsLong()>0;
        boolean refused=false,on=false;
        synchronized(lock){
            if(!overlay.getAsBoolean()||!foreground.getAsBoolean()){enabledWriter.accept(false);targetAt=0;}
            else if(cooldownActive){enabledWriter.accept(false);targetAt=0;guardReason="Cooldown active; tap VGP after it clears";refused=true;}
            else{enabledWriter.accept(true);guardReason="";on=true;}
        }
        if(refused)event.accept("vgp-cooldown-guard","Enable refused; cooldown active");
        else if(on)event.accept("vgp-enabled","ON; runtime only");
    }
    /**
     * Arms or disarms the optional cooldown safety. Arming it while VGP is already active and a
     * cooldown is live switches VGP off immediately; disarming clears any lingering guard reason.
     */
    void setStopOnCooldown(boolean value) {
        boolean cooldownActive=value&&cooldownRemaining.getAsLong()>0;
        boolean tripped=false;
        synchronized(lock){
            stopWriter.accept(value);
            if(!value)guardReason="";
            else if(enabled.getAsBoolean()&&cooldownActive){enabledWriter.accept(false);targetAt=0;guardReason="Paused for active cooldown; tap VGP to resume once it clears";tripped=true;}
        }
        save.run();
        event.accept("vgp-stop-on-cooldown",value?(tripped?"ON; switched VGP off for active cooldown":"ON"):"OFF");
    }
    /**
     * When the guard is armed, a live cooldown trips VGP off before the next request and it stays off
     * (no auto-resume). Driven by the enabled/allowed heartbeat; the Cooldown read happens with no
     * lock held and logs only on the single off transition.
     */
    void guardCooldown() {
        if(!(enabled.getAsBoolean()&&stopOnCooldown.getAsBoolean()))return;
        if(cooldownRemaining.getAsLong()<=0)return;
        boolean tripped=false;
        synchronized(lock){
            if(enabled.getAsBoolean()&&stopOnCooldown.getAsBoolean()){enabledWriter.accept(false);targetAt=0;guardReason="Paused for active cooldown; tap VGP to resume once it clears";tripped=true;}
        }
        if(tripped)event.accept("vgp-cooldown-guard","VGP switched off; cooldown active");
    }
    void setStatus(String value) { status = value; }
    /** A guard reason (cooldown pause/refusal) is shown in place of a generic native status. */
    String status() { String reason=guardReason; return reason.length()>0?reason:status; }

    void setTarget(boolean visible,double x,double y,boolean spin) {
        if(!visible){if(targetAt>0)targetAt=Math.min(targetAt,SystemClock.elapsedRealtime()-600);return;}
        if(!Double.isFinite(x)||!Double.isFinite(y)||x<0||x>1||y<0||y>1){targetAt=0;return;}
        targetX=(float)x;targetY=(float)y;targetSpin=spin;targetAt=SystemClock.elapsedRealtime();
    }
    /** Drops any pending target without touching the enable/guard state (used on settings reload). */
    void clearTarget() { targetAt=0; }
    boolean targetVisible(boolean allowed,boolean mapVisible) {
        long age=SystemClock.elapsedRealtime()-targetAt;
        return allowed&&mapVisible&&targetAt>0&&age>=0&&age<1200;
    }
    float targetX() { return targetX; }
    float targetY() { return targetY; }
    boolean targetSpin() { return targetSpin; }

    void result(String action,boolean success) {
        if(success&&action.equals("spin"))cooldownAction.accept("Pokestop spin");
        Application app=application;if(!enabled.getAsBoolean()||(options.getAsInt()&4)==0||app==null)return;
        if(android.os.Build.VERSION.SDK_INT>=33&&app.checkSelfPermission("android.permission.POST_NOTIFICATIONS")!=android.content.pm.PackageManager.PERMISSION_GRANTED)return;
        try{android.app.NotificationManager manager=(android.app.NotificationManager)app.getSystemService(android.content.Context.NOTIFICATION_SERVICE);
            manager.createNotificationChannel(new android.app.NotificationChannel("pgrazor-vgp","PGRazor VGP",android.app.NotificationManager.IMPORTANCE_LOW));
            String message=action.equals("catch")?(success?"Pokémon caught":"Pokémon not caught"):(success?"PokéStop spun":"Spin unsuccessful");
            manager.notify(7301,new android.app.Notification.Builder(app,"pgrazor-vgp").setSmallIcon(android.R.drawable.stat_notify_sync).setContentTitle("PGRazor VGP").setContentText(message).setAutoCancel(true).build());
        }catch(RuntimeException error){event.accept("vgp-notification-error",error.toString());}
    }
    /** Losing the foreground switches VGP off, clears any target, and never auto-resumes. */
    void suspend() {
        synchronized(lock){enabledWriter.accept(false);targetAt=0;guardReason="App left foreground; tap VGP to start";}
    }
}
