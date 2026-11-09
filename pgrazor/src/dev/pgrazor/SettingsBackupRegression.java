package dev.pgrazor;

import android.app.Application;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.AtomicFile;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.nio.charset.StandardCharsets;
import java.util.Iterator;
import java.io.File;
import java.io.FileOutputStream;

/**
 * In-process settings-backup roundtrip check. It drives the exact {@link SettingsBackup#snapshot} and
 * {@link SettingsBackup#restore} entry points the Settings-backup screen uses, so it exercises the real
 * JSON serializer and the real import path inside the running app. It never opens the Android document
 * picker and never reads or writes an external file, so it does not and must not claim the file
 * export/import UI itself was tested: only the serializer and import logic are validated here.
 *
 * <p>The check: capture the original backup <em>in full</em> before any mutation; confirm the recently
 * whitelisted {@code throwAlwaysHit} is present in the export; flip {@code joystickVisible} and
 * {@code throwAlwaysHit} through their real {@link Control} setters on the main thread; re-export and
 * confirm both changes; then <em>always</em> restore the captured original through the real import and
 * structurally verify that every original setting, every preference group and the speed come back,
 * independent of JSON key order. The response carries only booleans — never a secret, identity,
 * location, favorite, or any other backup value, and never any exception text.
 *
 * <p>It refuses to run (throws {@link IllegalStateException} with the fixed code {@value #ERR_PREFLIGHT})
 * when VGP is enabled or the avatar is moving, so it cannot disturb live user automation. The real
 * backup operations run on the calling (regression worker) thread; only the two setter mutations are
 * dispatched to the main thread under a bounded, cancellable wait that holds no lock. Any exception
 * aborts before a success can be reported, so {@code ok} can never be true on a failed run.
 */
final class SettingsBackupRegression {
    private static final String ERR_PREFLIGHT = "preflight-blocked";
    private static final long MAIN_TIMEOUT_MS = 2000L, MAIN_GRACE_MS = 250L;
    private static volatile MainCall activeCall;
    private static boolean settled(){MainCall call=activeCall;return call==null||call.settled();}

    private SettingsBackupRegression() {}

    /**
     * Runs the roundtrip and returns only booleans
     * ({@code alwaysHitIncluded}, {@code mutated}, {@code settingsMatch}, {@code preferencesMatch},
     * {@code speedMatch}, {@code restored}, {@code ok}). Must be called off the main thread.
     */
    static JSONObject run(Application app) throws Exception {
        if (app == null) throw new IllegalStateException(ERR_PREFLIGHT);
        if (Control.regressionAutomationActive() || !settled()) {
            throw new IllegalStateException(ERR_PREFLIGHT);
        }
        AtomicFile recovery=new AtomicFile(new File(app.getFilesDir(),"pgrazor-regression/backup-recovery.json"));
        if(recovery.getBaseFile().exists())throw new IllegalStateException("backup-recovery-pending");

        // Capture the original backup in full, through the same serializer the export uses, BEFORE any
        // mutation. A partially captured snapshot is never used as a restore source: if this throws,
        // nothing was changed and there is nothing to restore.
        JSONObject original = SettingsBackup.snapshot(app);
        JSONObject originalDoc = new JSONObject(
            new String(original.toString(2).getBytes(StandardCharsets.UTF_8), StandardCharsets.UTF_8));
        boolean alwaysHitIncluded = originalDoc.getJSONObject("settings").has("throwAlwaysHit");
        boolean fromJoystick = Control.joystickVisible();
        boolean fromAlwaysHit = Control.throwAlwaysHit();
        FileOutputStream saved=null;
        try{saved=recovery.startWrite();saved.write(originalDoc.toString().getBytes(StandardCharsets.UTF_8));
            recovery.finishWrite(saved);saved=null;
        }finally{if(saved!=null)recovery.failWrite(saved);}

        boolean mutated = false, restored = false,
                settingsMatch = false, preferencesMatch = false, speedMatch = false;
        try {
            // Flip both settings through the real setters, on the main thread, bounded and cancellable.
            boolean applied = onMain(() -> {
                if(Control.regressionAutomationActive())throw new IllegalStateException(ERR_PREFLIGHT);
                Control.setJoystickVisible(!fromJoystick);
                Control.setThrowAlwaysHit(!fromAlwaysHit);
            }, () -> {
                Control.setJoystickVisible(fromJoystick);
                Control.setThrowAlwaysHit(fromAlwaysHit);
            });
            if (applied) {
                JSONObject changed = SettingsBackup.snapshot(app).getJSONObject("settings");
                mutated = bool(changed, "joystickVisible") == !fromJoystick
                       && bool(changed, "throwAlwaysHit") == !fromAlwaysHit;
            }
        } finally {
            // Always restore the fully captured original through the real import, whatever happened above
            // (including a partial or abandoned mutation). Restoration being unverified fails the result.
            try {
                SettingsBackup.restore(app, originalDoc);
                JSONObject back = SettingsBackup.snapshot(app);
                settingsMatch = deepEquals(original.getJSONObject("settings"), back.getJSONObject("settings"));
                preferencesMatch = deepEquals(original.getJSONObject("preferences"), back.getJSONObject("preferences"));
                speedMatch = Double.compare(original.getDouble("speedKmh"), back.getDouble("speedKmh")) == 0;
                restored = settingsMatch && preferencesMatch && speedMatch && settled();
            } catch (Exception restoreFailed) {
                restored = settingsMatch = preferencesMatch = speedMatch = false;
            }
        }

        JSONObject result = new JSONObject();
        result.put("alwaysHitIncluded", alwaysHitIncluded);
        result.put("mutated", mutated);
        result.put("settingsMatch", settingsMatch);
        result.put("preferencesMatch", preferencesMatch);
        result.put("speedMatch", speedMatch);
        result.put("restored", restored);
        result.put("ok", alwaysHitIncluded && mutated && restored);
        if(restored)recovery.delete();
        return result;
    }

    /** Explicit recovery after an interrupted test; retained data never leaves app-private storage. */
    static JSONObject recover(Application app)throws Exception{
        if(app==null||Control.regressionAutomationActive()||!settled())throw new IllegalStateException(ERR_PREFLIGHT);
        AtomicFile recovery=new AtomicFile(new File(app.getFilesDir(),"pgrazor-regression/backup-recovery.json"));
        byte[] bytes=recovery.readFully();
        if(bytes.length>512*1024)throw new IllegalStateException("backup-recovery-invalid");
        JSONObject original=new JSONObject(new String(bytes,StandardCharsets.UTF_8));
        SettingsBackup.restore(app,original);
        JSONObject back=SettingsBackup.snapshot(app);
        boolean restored=deepEquals(original.getJSONObject("settings"),back.getJSONObject("settings"))
            &&deepEquals(original.getJSONObject("preferences"),back.getJSONObject("preferences"))
            &&Double.compare(original.getDouble("speedKmh"),back.getDouble("speedKmh"))==0;
        if(restored)recovery.delete();
        return new JSONObject().put("restored",restored);
    }

    /** A persisted boolean setting is exported as the string {@code "true"}/{@code "false"}. */
    private static boolean bool(JSONObject settings, String key) {
        return "true".equals(settings.optString(key));
    }

    /** Structural equality independent of JSON key order; numbers are compared by value. */
    private static boolean deepEquals(Object a, Object b) throws JSONException {
        if (a == null || a == JSONObject.NULL) return b == null || b == JSONObject.NULL;
        if (b == null || b == JSONObject.NULL) return false;
        if (a instanceof JSONObject && b instanceof JSONObject) {
            JSONObject oa = (JSONObject) a, ob = (JSONObject) b;
            if (oa.length() != ob.length()) return false;
            for (Iterator<String> it = oa.keys(); it.hasNext(); ) {
                String key = it.next();
                if (!ob.has(key) || !deepEquals(oa.get(key), ob.get(key))) return false;
            }
            return true;
        }
        if (a instanceof JSONArray && b instanceof JSONArray) {
            JSONArray aa = (JSONArray) a, ab = (JSONArray) b;
            if (aa.length() != ab.length()) return false;
            for (int i = 0; i < aa.length(); i++) if (!deepEquals(aa.get(i), ab.get(i))) return false;
            return true;
        }
        if (a instanceof Number && b instanceof Number)
            return Double.compare(((Number) a).doubleValue(), ((Number) b).doubleValue()) == 0;
        if (a instanceof Boolean || b instanceof Boolean) return a.equals(b);
        return a.toString().equals(b.toString());
    }

    /** Posts the mutation to the main thread and waits, bounded; holds no lock across the wait. */
    private static boolean onMain(Runnable body,Runnable lateRestore) {
        Handler main = new Handler(Looper.getMainLooper());
        MainCall call = new MainCall(body,lateRestore);
        activeCall=call;
        if (!main.post(call)) { call.abandon(); return false; }
        boolean done = call.await(MAIN_TIMEOUT_MS, MAIN_GRACE_MS);
        if (!done) main.removeCallbacks(call); // effective only while still queued (PENDING)
        return done;
    }

    /**
     * One-shot main-thread call with a bounded, cancellable wait. The body runs exactly once, and only
     * if it is claimed before the waiting worker abandons it, so a timed-out mutation never starts. A
     * body that throws is reported as failed, so it can never certify a mutation.
     */
    private static final class MainCall implements Runnable {
        private static final int PENDING = 0, RUNNING = 1, DONE = 2, ABANDONED = 3;
        private final Object lock = new Object();
        private final Runnable body;
        private final Runnable lateRestore;
        private int state = PENDING;
        private boolean failed;
        private boolean abandoned;

        MainCall(Runnable body,Runnable lateRestore) { this.body = body;this.lateRestore=lateRestore; }

        @Override public void run() {
            synchronized (lock) {
                if (state != PENDING) return; // the worker already abandoned this call; do not mutate
                state = RUNNING;
            }
            boolean ok = true;
            try {
                body.run();
            } catch (RuntimeException error) {
                ok = false; // do not crash the game's main thread or certify a partial mutation
            } finally {
                boolean restore;
                synchronized(lock){restore=abandoned;if(!restore){failed=!ok;state=DONE;lock.notifyAll();}}
                if(restore){try{lateRestore.run();}finally{synchronized(lock){failed=true;state=DONE;lock.notifyAll();}}}
            }
        }

        void abandon() { synchronized (lock) { if (state == PENDING) state = ABANDONED; } }
        boolean settled(){synchronized(lock){return state==DONE||state==ABANDONED;}}

        boolean await(long timeoutMillis, long graceMillis) {
            synchronized (lock) {
                long deadline = SystemClock.uptimeMillis() + timeoutMillis;
                boolean graced = false;
                while (state != DONE) {
                    long remaining = deadline - SystemClock.uptimeMillis();
                    if (remaining <= 0) {
                        if (state == PENDING) { state = ABANDONED; return false; } // never started: cancel it
                        if (state == ABANDONED) return false;
                        if (graced || graceMillis <= 0) {abandoned=true;return false;}
                        graced = true;
                        deadline = SystemClock.uptimeMillis() + graceMillis;
                        continue;
                    }
                    try {
                        lock.wait(remaining);
                    } catch (InterruptedException interrupted) {
                        Thread.currentThread().interrupt();
                        if (state == PENDING) state = ABANDONED;
                        else if(state==RUNNING)abandoned=true;
                        return false;
                    }
                }
                return !failed;
            }
        }
    }
}
