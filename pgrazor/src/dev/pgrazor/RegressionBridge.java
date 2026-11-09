package dev.pgrazor;

import android.app.Application;
import android.content.res.AssetManager;
import android.os.Handler;
import android.os.Looper;
import android.os.Process;
import android.os.SystemClock;
import android.util.AtomicFile;
import android.util.Log;

import org.json.JSONException;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.regex.Pattern;

/**
 * Opt-in, app-private regression bridge for meaningful settings-persistence end-to-end checks.
 *
 * <p>It is strictly inert unless the APK ships the exact asset {@code pgrazor/regression-v1.enabled}
 * whose trimmed contents equal {@code v1}. When enabled it watches a single app-private directory
 * ({@code getFilesDir()/pgrazor-regression}) for a {@code request.json} file, performs a supported
 * settings, inventory, or backup check against the live app state, and publishes
 * {@code response.json}. There is no exported component, no receiver, no network, and no shell or
 * reflection use: the only transport is the app's own private files directory.
 *
 * <h2>Protocol</h2>
 * <p>Request (max {@value #MAX_REQUEST_BYTES} bytes, valid JSON object):
 * <pre>{@code {"id":"<[a-z0-9-]{1,64}>","op":"snapshot"}
 * {"id":"<id>","op":"set-joystick","value":true}
 * {"id":"<id>","op":"inventory"}
 * {"id":"<id>","op":"backup-roundtrip"}
 * {"id":"<id>","op":"backup-recover"}}</pre>
 * {@code value} must be a strict JSON Boolean ({@code true}/{@code false}); strings and numbers are
 * rejected. Unknown operations are rejected. A request is atomically consumed (renamed to
 * {@code request.processing} then removed) <em>before</em> any action runs, so a process restart can
 * never replay it: each request executes at most once.
 *
 * <p>Response (written atomically via {@link AtomicFile}):
 * <pre>{@code {"id":"<id>","pid":<int>,"ok":true,
 *  "settings":{"joystickVisible":<bool>,"quickSpin":<bool>,"autoTransfer":<bool>}}
 * {"id":"<id>","pid":<int>,"ok":false,"error":"<fixed-code>"}}</pre>
 * {@code pid} is {@link Process#myPid()}. On success {@code settings} is present and {@code error} is
 * absent; on failure {@code error} carries one of a fixed set of codes and {@code settings} is absent.
 * The response always carries the request's {@code id}; the harness must match by {@code id}.
 *
 * <h2>Semantics and safety</h2>
 * <ul>
 *   <li>Settings are read from the actual {@link Control} getters on the main thread (not from disk),
 *       so a snapshot reflects live in-memory state.</li>
 *   <li>{@code set-joystick} applies {@link Control#setJoystickVisible(boolean)} on the main thread and
 *       then blocks (off the main thread, holding no lock) on {@link Control#flushSettings(long)} so the
 *       change is durable on disk before the success response is written.</li>
 *   <li>Main-thread work is bounded. If the posted task has not started when the deadline passes it is
 *       cancelled and its mutation never runs; a {@code main-timeout} error is returned instead. The
 *       wait releases its monitor, so the main thread is never blocked on this bridge.</li>
 *   <li>Responses contain fixed error codes, validated request/process identifiers, boolean settings,
 *       and, when requested, numeric item counts/revision or boolean backup assertions. They contain
 *       no credentials, identities, locations, request bodies, or exception text. Backup checks retain
 *       a private recovery snapshot until restoration is verified.</li>
 * </ul>
 *
 * <h2>Known limitations</h2>
 * <ul>
 *   <li>The bridge assumes {@link Control#begin(Application)} already ran; call {@link #start} after it.
 *       Before settings are initialised, {@code set-joystick} still applies the in-memory value but the
 *       durability flush is a no-op (nothing queued), so run the harness only once the app is up.</li>
 *   <li>Requests must be published atomically by the harness (write a temp file, then rename to
 *       {@code request.json}). A torn or malformed request yields no response; the harness should retry.</li>
 *   <li>A request with no parseable {@code id} produces no response by design (nothing to address it to).</li>
 *   <li>A request lost to a crash between consume and response is never retried or replayed; the harness
 *       observes a timeout and may re-issue a fresh {@code id}.</li>
 *   <li>Timeout cancellation guarantees a not-yet-started mutation never runs after the deadline. A
 *       mutation that has already begun is a single non-blocking field assignment that completes in
 *       microseconds; the implementation does not, and cannot, un-run an in-flight assignment.</li>
 *   <li>The bridge runs for the life of the app process. There is no shutdown entry point. Inventory
 *       scans force recycling off without changing disposal preferences. Backup checks require VGP
 *       and movement to be inactive and restore the original settings, preferences, and speed.</li>
 * </ul>
 */
public final class RegressionBridge {
    private static final String TAG = "PGRazorRegression";
    private static final String ENABLE_ASSET = "pgrazor/regression-v1.enabled";
    private static final String ENABLE_TOKEN = "v1";
    private static final int MAX_ENABLE_BYTES = 256;
    private static final String DIR_NAME = "pgrazor-regression";
    private static final String REQUEST_NAME = "request.json";
    private static final String PROCESSING_NAME = "request.processing";
    private static final String RESPONSE_NAME = "response.json";

    static final int MAX_REQUEST_BYTES = 4096;
    private static final long POLL_MS = 250L;
    private static final long MAIN_TIMEOUT_MS = 2000L;
    private static final long MAIN_GRACE_MS = 250L;
    private static final long FLUSH_TIMEOUT_MS = 5000L;
    private static final Pattern ID_PATTERN = Pattern.compile("[a-z0-9-]{1,64}");

    // Fixed response error codes; no arbitrary text ever leaves the process.
    private static final String ERR_UNSUPPORTED_OP = "unsupported-op";
    private static final String ERR_BAD_VALUE = "bad-value";
    private static final String ERR_MAIN_TIMEOUT = "main-timeout";
    private static final String ERR_DURABILITY = "durability-failed";
    private static final String ERR_INTERNAL = "internal-error";

    private static final AtomicBoolean STARTED = new AtomicBoolean(false);

    private RegressionBridge() {}

    /**
     * Starts the bridge at most once. Does nothing unless the opt-in asset is present and equals the
     * expected token. Safe to call unconditionally at app startup after {@link Control#begin}.
     */
    public static void start(Application app) {
        if (app == null) return;
        if (!STARTED.compareAndSet(false, true)) return; // start once
        if (!enabled(app)) return;                        // inert unless the opt-in asset is present
        File dir = new File(app.getFilesDir(), DIR_NAME);
        if (!dir.isDirectory() && !dir.mkdirs()) {
            Log.i(TAG, "regression bridge disabled: workspace unavailable");
            return;
        }
        Handler main = new Handler(Looper.getMainLooper());
        Thread worker = new Thread(() -> loop(app, dir, main), "pgrazor-regression");
        worker.setDaemon(true);
        worker.start();
        Log.i(TAG, "regression bridge active");
    }

    /** True only when the exact opt-in asset exists and its trimmed contents equal {@value #ENABLE_TOKEN}. */
    private static boolean enabled(Application app) {
        AssetManager assets = app.getAssets();
        if (assets == null) return false;
        try (InputStream in = assets.open(ENABLE_ASSET)) {
            ByteArrayOutputStream buffer = new ByteArrayOutputStream();
            byte[] chunk = new byte[64];
            int read, total = 0;
            while ((read = in.read(chunk)) != -1) {
                total += read;
                if (total > MAX_ENABLE_BYTES) return false; // unexpected large asset: stay inert
                buffer.write(chunk, 0, read);
            }
            String token = new String(buffer.toByteArray(), StandardCharsets.UTF_8).trim();
            return ENABLE_TOKEN.equals(token);
        } catch (IOException missing) {
            return false; // absent asset => inert
        }
    }

    private static void loop(Application app, File dir, Handler main) {
        File request = new File(dir, REQUEST_NAME);
        File processing = new File(dir, PROCESSING_NAME);
        File response = new File(dir, RESPONSE_NAME);
        sweep(processing); // discard any crash orphan from a prior process; never replay it
        while (true) {
            try {
                sweep(processing); // an orphan can only come from a mid-flight crash; drop it unprocessed
                if (request.exists()) handle(app, request, processing, response, main);
            } catch (Throwable loopError) {
                // One bad request must never stop the daemon, and no arbitrary text leaves the process.
                Log.w(TAG, "regression bridge iteration error");
            }
            try {
                Thread.sleep(POLL_MS);
            } catch (InterruptedException interrupted) {
                Thread.currentThread().interrupt();
                return;
            }
        }
    }

    private static void sweep(File processing) {
        if (processing.exists() && !processing.delete()) Log.w(TAG, "could not clear orphan request");
    }

    private static void handle(Application app, File request, File processing, File response, Handler main) {
        // Atomically consume the request BEFORE any action: rename then remove, so a restart cannot
        // replay it. Same-directory rename is atomic; the removed file leaves nothing to re-run.
        sweep(processing);
        if (!request.renameTo(processing)) {
            sweep(request); // cannot stage it; drop it rather than spin on a stuck file
            return;
        }
        byte[] raw = readBounded(processing);
        sweep(processing); // consumed: nothing remains on disk to replay
        if (raw == null) return; // oversized/unreadable: no trustworthy id, so no response

        JSONObject req;
        try {
            req = new JSONObject(new String(raw, StandardCharsets.UTF_8));
        } catch (JSONException invalid) {
            return; // not a JSON object: no id, no response
        }

        Object idValue = req.opt("id");
        if (!(idValue instanceof String) || !ID_PATTERN.matcher((String) idValue).matches()) {
            return; // no valid id: cannot guarantee the response matches, so emit nothing
        }
        String id = (String) idValue;

        Object opValue = req.opt("op");
        String op = opValue instanceof String ? (String) opValue : "";
        if ("snapshot".equals(op)) {
            doSnapshot(id, response, main);
        } else if ("backup-roundtrip".equals(op)) {
            doBackup(app,id,response,main,false);
        } else if ("backup-recover".equals(op)) {
            doBackup(app,id,response,main,true);
        } else if ("inventory".equals(op)) {
            doInventory(id, response, main);
        } else if ("set-joystick".equals(op)) {
            Object value = req.opt("value");
            if (!(value instanceof Boolean)) {
                writeError(response, id, ERR_BAD_VALUE);
                return;
            }
            doSetJoystick(id, (Boolean) value, response, main);
        } else {
            writeError(response, id, ERR_UNSUPPORTED_OP);
        }
    }

    /** Reads up to {@value #MAX_REQUEST_BYTES} bytes; returns null if larger or unreadable. */
    private static byte[] readBounded(File file) {
        try (FileInputStream in = new FileInputStream(file)) {
            ByteArrayOutputStream buffer = new ByteArrayOutputStream();
            byte[] chunk = new byte[1024];
            int read, total = 0;
            while ((read = in.read(chunk)) != -1) {
                total += read;
                if (total > MAX_REQUEST_BYTES) return null;
                buffer.write(chunk, 0, read);
            }
            return buffer.toByteArray();
        } catch (IOException error) {
            return null;
        }
    }

    private static void doSnapshot(String id, File response, Handler main) {
        SettingsSnapshot snapshot = new SettingsSnapshot();
        MainTask task = new MainTask(snapshot::capture);
        if (!runOnMain(main, task)) {
            writeError(response, id, ERR_MAIN_TIMEOUT);
            return;
        }
        writeOk(response, id, snapshot);
    }

    private static void doBackup(Application app,String id,File response,Handler main,boolean recover) {
        try {
            JSONObject checks=recover?SettingsBackupRegression.recover(app):SettingsBackupRegression.run(app);
            SettingsSnapshot snapshot=new SettingsSnapshot();
            if(!runOnMain(main,new MainTask(snapshot::capture))){writeError(response,id,ERR_MAIN_TIMEOUT);return;}
            JSONObject result=base(id,true);
            result.put("settings",settingsJson(snapshot));result.put("backup",checks);
            writeRaw(response,result.toString());
        }catch(Exception error){
            String code=error instanceof IllegalStateException?error.getMessage():"backup-error";
            if(!"preflight-blocked".equals(code)&&!"backup-recovery-pending".equals(code))code="backup-error";
            writeError(response,id,code);
        }
    }

    /** Fresh game-owned recyclable-item counts; no item names or account data. */
    private static void doInventory(String id, File response, Handler main) {
        int[] token = new int[1];
        boolean[] allowed = new boolean[1];
        MainTask request = new MainTask(() -> {
            // The native regression-token path forces recycling off for this scan.
            // Do not toggle or require changes to the user's disposal preferences.
            allowed[0] = Control.itemAccessAllowed();
            if (allowed[0]) token[0] = Control.requestRegressionItemCatalog();
        });
        if (!runOnMain(main, request) || !allowed[0]) {
            writeError(response, id, "inventory-unavailable"); return;
        }
        long deadline = SystemClock.uptimeMillis() + 7000;
        while (Control.regressionCatalogCompleted() != token[0] && SystemClock.uptimeMillis() < deadline) {
            try { Thread.sleep(100); }
            catch (InterruptedException error) { Thread.currentThread().interrupt(); writeError(response,id,"interrupted"); return; }
        }
        if (Control.regressionCatalogCompleted() != token[0]) {
            writeError(response, id, "inventory-timeout"); return;
        }
        SettingsSnapshot snapshot = new SettingsSnapshot();
        String[] catalog = new String[1];
        long[] revision = new long[1];
        MainTask read = new MainTask(() -> { snapshot.capture(); catalog[0]=Control.regressionCatalog(); revision[0]=Control.regressionCatalogRevision(); });
        if (!runOnMain(main, read)) { writeError(response,id,ERR_MAIN_TIMEOUT); return; }
        try {
            JSONObject counts = new JSONObject();
            for (String row : catalog[0].split("\n")) {
                String[] fields = row.split("\\|", -1);
                if (fields.length != 3) continue;
                int item = Integer.parseInt(fields[0]), count = Integer.parseInt(fields[2]);
                if (item > 0 && count >= 0) counts.put(String.valueOf(item), count);
            }
            JSONObject result = base(id,true);
            result.put("settings",settingsJson(snapshot));
            result.put("inventory",counts);
            result.put("inventoryRevision",revision[0]);
            writeRaw(response,result.toString());
        } catch (RuntimeException | JSONException error) { writeError(response,id,"inventory-invalid"); }
    }

    private static void doSetJoystick(String id, boolean value, File response, Handler main) {
        SettingsSnapshot snapshot = new SettingsSnapshot();
        MainTask task = new MainTask(() -> {
            Control.setJoystickVisible(value);
            snapshot.capture();
        });
        if (!runOnMain(main, task)) {
            // The mutation never started (or is being abandoned); report a timeout, apply nothing durable.
            writeError(response, id, ERR_MAIN_TIMEOUT);
            return;
        }
        // Durability is the whole point of the test. Block here (off the main thread, holding no lock)
        // until the queued settings write has reached disk before reporting success.
        try {
            Control.flushSettings(FLUSH_TIMEOUT_MS);
        } catch (IOException notDurable) {
            writeError(response, id, ERR_DURABILITY);
            return;
        }
        writeOk(response, id, snapshot);
    }

    /**
     * Posts {@code task} to the main thread and waits, bounded. Returns true only if the task body ran
     * to completion. On timeout the task is cancelled (its not-yet-started body will never run) and a
     * best-effort dequeue is attempted. The monitor is released while waiting, so the main thread is
     * never blocked on this bridge, and no bridge lock is held across the flush that may follow.
     */
    private static boolean runOnMain(Handler main, MainTask task) {
        if (!main.post(task)) {
            task.markAbandoned();
            return false;
        }
        boolean completed = task.await(MAIN_TIMEOUT_MS, MAIN_GRACE_MS);
        if (!completed) main.removeCallbacks(task); // effective only while still queued (PENDING)
        return completed;
    }

    private static void writeOk(File response, String id, SettingsSnapshot snapshot) {
        try {
            JSONObject resp = base(id, true);
            resp.put("settings", settingsJson(snapshot));
            writeRaw(response, resp.toString());
        } catch (JSONException unexpected) {
            writeError(response, id, ERR_INTERNAL);
        }
    }

    private static JSONObject settingsJson(SettingsSnapshot snapshot) throws JSONException {
        return new JSONObject().put("joystickVisible",snapshot.joystickVisible)
            .put("quickSpin",snapshot.quickSpin).put("autoTransfer",snapshot.autoTransfer);
    }

    private static void writeError(File response, String id, String code) {
        try {
            JSONObject resp = base(id, false);
            resp.put("error", code);
            writeRaw(response, resp.toString());
        } catch (JSONException unexpected) {
            // id is already validated against [a-z0-9-]{1,64} and the code is a fixed constant, so this
            // minimal hand-built fallback carries no quotes, backslashes, or arbitrary text.
            writeRaw(response, "{\"id\":\"" + id + "\",\"pid\":" + Process.myPid()
                + ",\"ok\":false,\"error\":\"" + ERR_INTERNAL + "\"}");
        }
    }

    private static JSONObject base(String id, boolean ok) throws JSONException {
        JSONObject resp = new JSONObject();
        resp.put("id", id);
        resp.put("pid", Process.myPid());
        resp.put("ok", ok);
        return resp;
    }

    private static void writeRaw(File response, String json) {
        AtomicFile file = new AtomicFile(response);
        FileOutputStream out = null;
        try {
            out = file.startWrite();
            out.write(json.getBytes(StandardCharsets.UTF_8));
            file.finishWrite(out);
            out = null;
        } catch (IOException error) {
            if (out != null) file.failWrite(out);
            Log.w(TAG, "could not write regression response");
        }
    }

    /** The three persisted settings the harness verifies, captured from live {@link Control} getters. */
    private static final class SettingsSnapshot {
        volatile boolean joystickVisible, quickSpin, autoTransfer;

        /** Must run on the main thread. */
        void capture() {
            joystickVisible = Control.joystickVisible();
            quickSpin = Control.quickSpinEnabled();
            autoTransfer = Control.autoTransferEnabled();
        }
    }

    /**
     * A one-shot main-thread task with a bounded, cancellable wait. The body runs exactly once, and only
     * if it is claimed before the waiting worker abandons it, so a timed-out mutation never starts.
     */
    private static final class MainTask implements Runnable {
        private static final int PENDING = 0, RUNNING = 1, DONE = 2, ABANDONED = 3;
        private final Object lock = new Object();
        private final Runnable body;
        private int state = PENDING;
        private boolean failed;

        MainTask(Runnable body) {
            this.body = body;
        }

        @Override
        public void run() {
            synchronized (lock) {
                if (state != PENDING) return; // the worker already abandoned this task; do not mutate
                state = RUNNING;
            }
            try {
                body.run();
            } catch (RuntimeException error) {
                // Do not crash the game's main thread or certify a partial capture.
                synchronized (lock) { failed = true; }
            } finally {
                synchronized (lock) {
                    state = DONE;
                    lock.notifyAll();
                }
            }
        }

        void markAbandoned() {
            synchronized (lock) {
                if (state == PENDING) state = ABANDONED;
            }
        }

        /**
         * Waits up to {@code timeoutMillis} for the body to complete. If the body has not yet started at
         * the deadline it is abandoned and false is returned, guaranteeing the mutation never runs. If the
         * body is already running (a non-blocking Control call), a single bounded grace is allowed for it
         * to finish so an accurate result can be reported; otherwise false is returned.
         */
        boolean await(long timeoutMillis, long graceMillis) {
            synchronized (lock) {
                long deadline = SystemClock.uptimeMillis() + timeoutMillis;
                boolean graced = false;
                while (state != DONE) {
                    long remaining = deadline - SystemClock.uptimeMillis();
                    if (remaining <= 0) {
                        if (state == PENDING) {
                            state = ABANDONED; // never started: cancel it so the mutation cannot run later
                            return false;
                        }
                        if (state == ABANDONED) return false;
                        if (graced || graceMillis <= 0) return false; // already running but overran the grace
                        graced = true;
                        deadline = SystemClock.uptimeMillis() + graceMillis;
                        continue;
                    }
                    try {
                        lock.wait(remaining);
                    } catch (InterruptedException interrupted) {
                        Thread.currentThread().interrupt();
                        if (state == PENDING) state = ABANDONED;
                        return false;
                    }
                }
                return !failed;
            }
        }
    }
}
