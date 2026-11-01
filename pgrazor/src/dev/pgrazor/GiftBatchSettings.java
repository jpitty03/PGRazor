package dev.pgrazor;

import android.os.SystemClock;
import java.util.Locale;
import org.json.JSONObject;

/**
 * Session-only state for the one-shot "Send &amp; Open Gifts" automation. The native coordinator
 * processes each eligible friend once — sending an available gift before opening a received one —
 * and reports running counts back through {@link #publish(String)}. There is no recurring schedule
 * and no persisted enabled flag: a pass runs only while the user keeps it started.
 *
 * <p>The coordinator polls {@link #config()} on its transport loop and routes native events to
 * {@link #publish(String)}; both the dialog ({@link GiftBatchDialog}) and the coordinator read state
 * through the query methods here.
 */
public final class GiftBatchSettings {
    private GiftBatchSettings() {}

    private static final Object LOCK = new Object();
    private static final long RUN_LIMIT_MS = 30L * 60L * 1000L;
    private static final String DEFAULT_MESSAGE = "Sends your available gifts, then opens received gifts.";

    private static long nextToken = System.currentTimeMillis() > 0 ? System.currentTimeMillis() : 1;
    private static long token;            // 0 when no command has ever run; identifies the current pass
    private static boolean running;       // an enabled=1 command is live
    private static boolean stopping;      // stop requested; an enabled=0 command stays live until native acks
    private static boolean fullBag = true;
    private static long deadline;         // SystemClock.elapsedRealtime() cut-off for the running pass
    private static String phase = "idle";
    private static String message = DEFAULT_MESSAGE;
    private static long sent, opened, skipped;

    /** True while a command is live — either running (enabled=1) or awaiting a stop acknowledgement. */
    public static boolean running() {
        synchronized (LOCK) { tick(); return running || stopping; }
    }

    /** True once Stop has been requested but native has not yet acknowledged stopped/done/error. */
    public static boolean stopPending() {
        synchronized (LOCK) { tick(); return stopping; }
    }

    /** The current full-bag choice; used as the dialog checkbox's initial state (default on). */
    public static boolean fullBagDefault() {
        synchronized (LOCK) { return fullBag; }
    }

    /** Compact, multi-line status for the dialog. */
    public static String status() {
        synchronized (LOCK) {
            tick();
            String label = phase.isEmpty() ? "" : phase.substring(0, 1).toUpperCase(Locale.US) + phase.substring(1);
            return label + ": " + message
                + "\nSent " + sent + "   Opened " + opened + "   Skipped " + skipped;
        }
    }

    /**
     * Polled by the transport daemon. Returns {@code null} when there is no pending or active command,
     * otherwise {@code G1|token|enabled(1/0)|fullBag(1/0)}. The enabled flag is 1 while running and 0
     * once Stop has been requested; the command keeps being emitted at enabled=0 until native
     * acknowledges a terminal phase for the matching token.
     */
    public static String config() {
        synchronized (LOCK) {
            tick();
            if (!running && !stopping) return null;
            return "G1|" + token + "|" + (running ? 1 : 0) + "|" + (fullBag ? 1 : 0);
        }
    }

    /**
     * Starts a single send-and-open pass. Refuses — with a user-facing message — when gifts are not
     * available, when another Automation task or batch evolution is active, and is a no-op when a
     * gift pass is already live.
     */
    public static void start(boolean fullBagValue) {
        // Do not hold our lock while consulting another automation's synchronized state.
        if (!Control.giftsAllowed())
            throw new IllegalStateException("Wait until the game is ready and gifts are available.");
        if (OneShotAutomation.enabled() || BatchEvolutionSettings.enabled())
            throw new IllegalStateException("Finish or stop the current Automation task first.");
        synchronized (LOCK) {
            if (running || stopping) return;
            token = freshToken();
            running = true; stopping = false; fullBag = fullBagValue;
            phase = "running"; message = "Starting; sending and opening gifts.";
            sent = opened = skipped = 0;
            deadline = SystemClock.elapsedRealtime() + RUN_LIMIT_MS;
        }
    }

    /**
     * Requests a stop. The enabled=0 command stays live (so {@link #config()} keeps returning it)
     * until native acknowledges stopped/done/error for the matching token.
     */
    public static void stop() {
        synchronized (LOCK) {
            if (!running && !stopping) return;
            if (token == 0) return;
            running = false; stopping = true; phase = "stopping";
            message = "Stopping. A send or open already in progress may finish.";
        }
    }

    /** Called on logout/account reset. Invalidates the token so late native events are ignored. */
    public static void reset() {
        synchronized (LOCK) {
            token = 0; running = false; stopping = false; fullBag = true;
            phase = "idle"; message = DEFAULT_MESSAGE; sent = opened = skipped = 0; deadline = 0;
        }
    }

    /**
     * Applies a native event. A stale token (not the current pass) is ignored, as is any event that
     * arrives after the pass has already concluded locally. Recognised terminal phases are
     * {@code done}, {@code stopped} and {@code error}.
     */
    public static void publish(String json) {
        if (json == null) return;
        try {
            JSONObject event = new JSONObject(json);
            String eventToken = event.optString("token", "");
            String eventPhase = event.optString("phase", "");
            if (!"running".equals(eventPhase) && !"done".equals(eventPhase)
                && !"stopped".equals(eventPhase) && !"error".equals(eventPhase)) return;
            synchronized (LOCK) {
                if (token == 0) return;
                if (!Long.toString(token).equals(eventToken)) return; // ignore stale token
                if (!running && !stopping) return;                    // pass already concluded; ignore late echo
                sent = nonNegative(event, "sent", sent);
                opened = nonNegative(event, "opened", opened);
                skipped = nonNegative(event, "skipped", skipped);
                boolean terminal = "done".equals(eventPhase) || "stopped".equals(eventPhase)
                    || "error".equals(eventPhase);
                if (terminal) {
                    running = false; stopping = false; phase = eventPhase; message = text(event, message);
                } else if (running) {
                    if (!eventPhase.isEmpty()) phase = eventPhase;
                    message = text(event, message);
                }
                // While stopping and not yet terminal we keep the stopping phase/message; counts updated above.
            }
        } catch (Exception ignored) {}
    }

    /** Ends a running pass when gifts become unavailable or the 30-minute limit is reached. */
    private static void tick() {
        if (!running) return;
        if (!Control.giftsAllowed()) requestStop("Stopping: gifts are no longer available.");
        else if (SystemClock.elapsedRealtime() >= deadline) requestStop("Stopping: time limit reached.");
    }

    private static void requestStop(String why) {
        running = false; stopping = true; phase = "stopping"; message = why;
    }

    private static long freshToken() {
        if (nextToken == Long.MAX_VALUE) nextToken = 1; else nextToken++;
        return nextToken;
    }

    private static long nonNegative(JSONObject event, String key, long fallback) {
        long value = event.optLong(key, fallback);
        return value < fallback || value > 10000 ? fallback : value;
    }

    private static String text(JSONObject event, String fallback) {
        String value = event.optString("message", fallback);
        return value == null || value.isEmpty() ? fallback : value;
    }
}
