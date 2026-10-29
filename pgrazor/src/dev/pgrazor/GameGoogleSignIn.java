package dev.pgrazor;

import android.app.Activity;
import android.os.Handler;
import android.os.Looper;
import java.lang.reflect.Constructor;
import java.lang.reflect.Method;
import org.json.JSONException;
import org.json.JSONObject;

/**
 * Compiled Java port of the runtime browser sign-in adapter (embedded-agent.js
 * {@code installGoogleBrowserLogin}/{@code finishGoogleSignIn}).
 *
 * <p>This class owns the whole pending-consumer / poll-timer / result-parsing state machine that the
 * Frida script used to carry in JavaScript. It never installs any hook: the Frida layer keeps the one
 * {@code SignInManager$Companion.signIn(Activity, GoogleSignInClient, Function1)} hook and drives this
 * object through {@link #signIn(Activity, Object)} and {@link #shutdown()}. The existing
 * {@link GoogleBrowserAuth} (PKCE, networking, in-memory result storage) is used unchanged through its
 * public {@code begin}/{@code takeResult}/{@code cancel} contract; none of that moves here.
 *
 * <p>The game {@code SignInResponse(String,String,String)} constructor and Kotlin
 * {@code Function1.invoke(Object)} are reached only by reflection resolved in {@link #create()}, so the
 * module carries no compile dependency on the game. The pending-result JSON produced by
 * {@link GoogleBrowserAuth#takeResult(String)} is parsed and consumed entirely in Java and never
 * bridged back to JavaScript.
 *
 * <p>Threading and safety properties:
 * <ul>
 *   <li>{@link #create()} and {@link #signIn(Activity, Object)} may be called from the host thread;
 *       every actual {@link GoogleBrowserAuth} call, the poll, and the game-consumer invocation run on
 *       the main-looper {@link Handler}. All lifecycle mutation of the one pending request therefore
 *       happens on that single thread.</li>
 *   <li>{@code signIn} is always queued onto the handler, even when already on the main thread, so a
 *       reentrant consumer callback can never recursively corrupt the pending request.</li>
 *   <li>At most one request is pending and at most one poll is outstanding (the first poll and every
 *       reposted poll run 250&nbsp;ms apart). Browser authentication deliberately survives the game's
 *       background pause: there is no {@code Control.active}/foreground gate here.</li>
 *   <li>{@link #shutdown()} sets a volatile closed flag immediately and cancels the pending request on
 *       the handler; any {@code signIn} already queued completes Cancelled exactly once without ever
 *       launching a browser.</li>
 *   <li>No lock is ever held across a {@link GoogleBrowserAuth} call or a consumer invocation, and the
 *       pending request is always detached before the game consumer is invoked.</li>
 *   <li>{@link Control#event} records phase/status only, never a raw exception, the result JSON, an
 *       email, a token, the activity or an account id. Logging failures are swallowed.</li>
 * </ul>
 */
public final class GameGoogleSignIn {
    private static final String PHASE = "google-browser";
    // The exact and only accepted result statuses; matched by value, never coerced via toString.
    private static final String SUCCESS = "Success";
    private static final String CANCELLED = "Cancelled";
    private static final String FAIL = "Fail";
    private static final long POLL_INTERVAL_MS = 250L;

    // Resolved game metadata (set once by create(); treated as final afterwards).
    private Constructor<?> responseConstructor; // SignInResponse(String status, String email, String idToken)
    private Method invokeMethod;                // kotlin.jvm.functions.Function1.invoke(Object)
    private Handler handler;                    // main-looper handler: all Browser/poll/consumer work
    private final Runnable pollRunnable = new Runnable() { public void run() { pollOnce(); } };

    // Set immediately on shutdown from any thread; read on the handler to refuse further browser work.
    private volatile boolean closed;
    // Touched only on the handler thread: the single in-flight request, or null when idle.
    private Pending pending;

    /** Inert constructor: nothing is resolved or captured until {@link #create()} runs. */
    private GameGoogleSignIn() {}

    /** One in-flight sign-in: the opaque browser request id and the strongly held game consumer. */
    private static final class Pending {
        final String id;
        final Object consumer;
        boolean done; // delivered exactly once; guards against a replaced/stale poll resolving twice
        Pending(String id, Object consumer) {
            this.id = id;
            this.consumer = consumer;
        }
    }

    /**
     * Resolves the game sign-in metadata and the main-looper handler. May be called from the host
     * thread. The instance is ready to receive {@link #signIn(Activity, Object)} once this returns.
     *
     * @throws IllegalStateException if the {@code SignInResponse} constructor or Kotlin
     *     {@code Function1.invoke} is absent; the message is a constant and never carries the cause.
     */
    public static GameGoogleSignIn create() {
        GameGoogleSignIn adapter = new GameGoogleSignIn();
        adapter.initialize();
        return adapter;
    }

    private void initialize() {
        try {
            Class<?> responseClass = gameClass("com.nianticlabs.platform.authgooglesignin.SignInResponse");
            responseConstructor = responseClass.getConstructor(String.class, String.class, String.class);
            responseConstructor.setAccessible(true);
            Class<?> functionClass = gameClass("kotlin.jvm.functions.Function1");
            invokeMethod = functionClass.getMethod("invoke", Object.class);
            invokeMethod.setAccessible(true);
        } catch (Throwable missing) {
            // Never log the reflection failure detail; required metadata is simply absent.
            throw new IllegalStateException("Game sign-in metadata unavailable");
        }
        handler = new Handler(Looper.getMainLooper());
        log("Browser sign-in adapter installed");
    }

    // --- Public lifecycle (driven by the single Frida sign-in hook) -------------------------------

    /**
     * Begins a browser sign-in for {@code consumer}, replacing any request already pending. Safe to
     * call from any thread; the work is always queued onto the main handler so it serializes behind any
     * in-flight completion and a reentrant consumer cannot corrupt the pending request. A null consumer
     * is ignored (there is nothing to call back); a null activity produces a Fail callback.
     */
    public void signIn(final Activity activity, final Object consumer) {
        if (consumer == null) {
            log("Ignored sign-in; missing consumer");
            return;
        }
        if (!post(new Runnable() { public void run() { signInOnMain(activity, consumer); } }, 0)) {
            // Never invoke a game consumer on an arbitrary host thread. No queued closure survives
            // a rejected post; surface an explicit, credential-free dispatch failure to that caller.
            if (Looper.myLooper() == Looper.getMainLooper())
                invokeConsumer(consumer, closed ? CANCELLED : FAIL, null, null);
            else throw new IllegalStateException("Game sign-in main-thread queue unavailable");
        }
    }

    /** Marks the adapter closed immediately and cancels any pending request on the handler thread. */
    public void shutdown() {
        if (closed) return;
        closed = true;
        if (!post(new Runnable() { public void run() { shutdownOnMain(); } }, 0)
                && Looper.myLooper() == Looper.getMainLooper()) shutdownOnMain();
        // If a foreign-thread shutdown post is rejected, the existing poll or queued start
        // drains the pending request on main when it next executes.

    }

    // --- Handler-thread work ----------------------------------------------------------------------

    private void signInOnMain(Activity activity, Object consumer) {
        // A request queued before shutdown completed must cancel without ever launching a browser.
        if (closed) {
            shutdownOnMain();
            invokeConsumer(consumer, CANCELLED, null, null);
            return;
        }
        // Replacing a pending request: cancel+drain it and call its consumer Cancelled exactly once
        // before beginning the new one. invokeConsumer isolates a throwing old consumer.
        if (pending != null) replacePending(pending);
        if (closed) { invokeConsumer(consumer, CANCELLED, null, null); return; }
        if (activity == null) {
            invokeConsumer(consumer, FAIL, null, null);
            return;
        }
        String id;
        try {
            id = GoogleBrowserAuth.begin(activity);
            if (id == null || id.isEmpty()) throw new IllegalStateException("Browser request unavailable");
        } catch (Throwable beginFailed) {
            invokeConsumer(consumer, closed ? CANCELLED : FAIL, null, null);
            return;
        }
        // No activity reference is retained past begin; only the id and consumer are held.
        Pending started = new Pending(id, consumer);
        pending = started;
        // Shutdown may have raced in while begin ran: honour it rather than start a doomed poll.
        if (closed) {
            pending = null;
            cancelAndDrain(id);
            invokeConsumer(consumer, CANCELLED, null, null);
            return;
        }
        handler.removeCallbacks(pollRunnable);
        if (!post(pollRunnable, POLL_INTERVAL_MS)) { failAndDrain(started); return; }
        log("Browser sign-in started");
    }

    private void pollOnce() {
        if (closed) { shutdownOnMain(); return; }
        Pending current = pending;
        if (current == null || current.done) return;
        String raw;
        try {
            raw = GoogleBrowserAuth.takeResult(current.id);
        } catch (Throwable pollFailed) {
            failAndDrain(current);
            return;
        }
        if (closed) { shutdownOnMain(); return; }
        if (raw == null) {
            if (!post(pollRunnable, POLL_INTERVAL_MS)) failAndDrain(current);
            return;
        }
        String status;
        String email = null;
        String token = null;
        try {
            JSONObject result = new JSONObject(raw);
            String resultStatus = requireString(result, "status");
            if (SUCCESS.equals(resultStatus)) {
                String resultEmail = requireString(result, "email");
                String resultToken = requireString(result, "idToken");
                if (resultEmail.isEmpty() || resultToken.isEmpty())
                    throw new JSONException("empty token material");
                status = SUCCESS;
                email = resultEmail;
                token = resultToken;
            } else if (CANCELLED.equals(resultStatus) || FAIL.equals(resultStatus)) {
                status = resultStatus; // Fail/Cancelled carry no email or token
            } else {
                throw new JSONException("status not whitelisted");
            }
        } catch (Throwable malformed) {
            // Malformed JSON, a non-whitelisted status, or missing/empty success material: never log
            // the contents, just fail once and drain any lingering cancellation result.
            failAndDrain(current);
            return;
        }
        complete(current, status, email, token);
    }

    private void shutdownOnMain() {
        Pending current = pending;
        handler.removeCallbacks(pollRunnable);
        if (current == null || current.done) return;
        current.done = true;
        pending = null;
        cancelAndDrain(current.id);
        invokeConsumer(current.consumer, CANCELLED, null, null);
    }

    private void replacePending(Pending old) {
        if (old.done) return;
        old.done = true;
        pending = null;
        handler.removeCallbacks(pollRunnable);
        cancelAndDrain(old.id);
        invokeConsumer(old.consumer, CANCELLED, null, null);
    }

    private void failAndDrain(Pending request) {
        cancelAndDrain(request.id);
        complete(request, FAIL, null, null);
    }

    // Cancels the browser session and drains the resulting Cancelled JSON so nothing is retained.
    private void cancelAndDrain(String id) {
        try { GoogleBrowserAuth.cancel(id); } catch (Throwable ignored) { /* bounded, never logged */ }
        try { GoogleBrowserAuth.takeResult(id); } catch (Throwable ignored) { /* drain only */ }
    }

    // Detaches the pending request and stops the poll BEFORE invoking the game consumer, so a reentrant
    // or throwing consumer can never see or corrupt a live pending request.
    private void complete(Pending request, String status, String email, String token) {
        if (pending != request || request.done) return;
        request.done = true;
        pending = null;
        handler.removeCallbacks(pollRunnable);
        if (closed) { cancelAndDrain(request.id); status = CANCELLED; email = null; token = null; }
        invokeConsumer(request.consumer, status, email, token);
    }

    private void invokeConsumer(Object consumer, String status, String email, String token) {
        if (consumer == null) return;
        try {
            Object response = responseConstructor.newInstance(status, email, token);
            invokeMethod.invoke(consumer, response);
            log("Completed: " + status);
        } catch (Throwable callbackFailed) {
            // The game consumer threw, or the response could not be built: contain it to a constant log.
            log("Game sign-in callback failed");
        }
    }

    private boolean post(Runnable action, long delay) {
        try {
            boolean accepted = delay == 0 ? handler.post(action) : handler.postDelayed(action, delay);
            if (accepted) return true;
        } catch (Throwable ignored) { }
        log("Main-thread queue unavailable");
        return false;
    }

    // --- Helpers ----------------------------------------------------------------------------------

    // Returns a JSON value only when it is genuinely a String; never coerces numbers/booleans/null.
    private static String requireString(JSONObject object, String key) throws JSONException {
        Object value = object.get(key); // throws when the key is absent
        if (!(value instanceof String)) throw new JSONException("not a string");
        return (String) value;
    }

    private static void log(String detail) {
        try {
            Control.event(PHASE, detail);
        } catch (Throwable ignored) {
            // A logging failure must never surface to the game or drop a callback.
        }
    }

    private static Class<?> gameClass(String name) throws ClassNotFoundException {
        ClassLoader own = GameGoogleSignIn.class.getClassLoader();
        try {
            return Class.forName(name, false, own);
        } catch (ClassNotFoundException missing) {
            ClassLoader context = Thread.currentThread().getContextClassLoader();
            if (context != null && context != own) return Class.forName(name, false, context);
            throw missing;
        }
    }
}
