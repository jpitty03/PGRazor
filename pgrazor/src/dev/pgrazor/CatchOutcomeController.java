package dev.pgrazor;

import android.app.Activity;
import android.os.Handler;
import android.widget.Toast;
import java.util.function.BooleanSupplier;
import java.util.function.Consumer;
import java.util.function.Supplier;

/**
 * Owns the post-catch outcome toast: the single queued {@link Toast}, its cancellation token and
 * the main-thread display gate. Extracted from Control so the toast lifecycle has one owner with
 * explicit injected dependencies rather than sharing Control's quick-ball lock and static fields.
 *
 * <p>Queues one short toast while the feature is active in the foreground. The gate is re-checked on
 * the main thread; a newer outcome or any OFF, readiness, background or connection change discards
 * queued work through {@link #invalidate()}. Logs record request/suppression only, never that the
 * user actually saw the toast. The token is guarded by {@link #lock}; the toast reference, display and
 * cancellation run on the injected main-thread {@link Handler}.
 */
final class CatchOutcomeController {
    private final Handler main;
    private final Supplier<Activity> activity;
    private final BooleanSupplier ballActive;
    private final BooleanSupplier transferActive;
    private final Consumer<String> log;
    /** Guards the token and the current toast reference; any token change invalidates queued work. */
    private final Object lock = new Object();
    private long token;
    private Toast toast;

    CatchOutcomeController(Handler main, Supplier<Activity> activity,
                           BooleanSupplier ballActive, BooleanSupplier transferActive, Consumer<String> log) {
        this.main = main; this.activity = activity;
        this.ballActive = ballActive; this.transferActive = transferActive; this.log = log;
    }

    /** Visible text for a natively confirmed outcome; anything else is rejected. */
    static String text(String outcome) {
        if("transferred".equals(outcome))return "Transferred";
        if("transfer-error".equals(outcome))return "Transfer Error";
        if("caught".equals(outcome))return "Caught";
        if("broke-free".equals(outcome))return "Not caught - broke free";
        if("fled".equals(outcome))return "Not caught - fled";
        return null;
    }
    private boolean outcomeAllowed(String outcome) {
        return ("transferred".equals(outcome)||"transfer-error".equals(outcome)) ? transferActive.getAsBoolean() : ballActive.getAsBoolean();
    }
    boolean request(String outcome) {
        final String message=text(outcome);
        if(message==null) { log.accept("suppressed; unsupported outcome"); return false; }
        final long queued;
        synchronized(lock) { queued=outcomeAllowed(outcome) ? ++token : -1; }
        if(queued<0) { log.accept("suppressed outcome="+outcome+"; feature not active in foreground"); return false; }
        log.accept("received outcome="+outcome+"; queued for display");
        if(main.post(() -> show(queued,outcome,message))) return true;
        log.accept("suppressed outcome="+outcome+"; main thread unavailable");
        return false;
    }
    private void show(long queued,String outcome,String message) {
        String reason=null;
        synchronized(lock) {
            if(queued!=token) reason="superseded or turned off";
            else if(!outcomeAllowed(outcome)) reason="feature not active in foreground";
        }
        Activity screen=activity.get();
        if(reason==null && (screen==null || screen.isFinishing() || screen.isDestroyed())) reason="no resumed game screen";
        if(reason==null && Control.overlaysHidden()) reason="overlay hidden";
        if(reason!=null) { log.accept("suppressed outcome="+outcome+"; "+reason); return; }
        try {
            cancel();
            toast=Toast.makeText(screen,message,Toast.LENGTH_SHORT);
            toast.show();
            log.accept("displayed outcome="+outcome+"; Toast.show requested, user visibility unverified");
        } catch(RuntimeException error) {
            toast=null;
            log.accept("suppressed outcome="+outcome+"; toast error "+error.getClass().getSimpleName());
        }
    }
    void invalidate() {
        synchronized(lock) { token++; }
        main.post(this::cancel);
    }
    private void cancel() {
        Toast current=toast; toast=null;
        if(current!=null) try { current.cancel(); } catch(RuntimeException ignored) {}
    }
}
