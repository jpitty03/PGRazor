package dev.pgrazor;

import android.app.Activity;
import android.os.Handler;
import com.nianticlabs.platform.authgooglesignin.SignInResponse;
import kotlin.jvm.functions.Function1;

/**
 * Drives the real {@link GameGoogleSignIn} against fake Android/game classes whose names and reflection
 * signatures match the runtime browser sign-in adapter (embedded-agent.js). No Android device, game
 * binary or network is involved: a controllable {@link Handler} queue (ordered by delay) lets the
 * driver step the poll deterministically, a fake {@link GoogleBrowserAuth} scripts begin/takeResult/
 * cancel, and the scenarios assert the observable behaviour (which status/fields reach the game
 * consumer, when a browser is begun or cancelled, and how the poll is timed).
 *
 * <p>The fake {@code Control} has no {@code active()}/foreground accessor, so the production class
 * compiling at all proves it applies no foreground gate to browser sign-in.
 */
public final class GameGoogleSignInTest {
    static final String SUCCESS="Success", CANCELLED="Cancelled", FAIL="Fail";
    static int checks;
    static final String EMAIL = "trainer@example.com";
    static final String TOKEN = "header.payload.signature";

    public static void main(String[] args) {
        successDeliversFields();
        pollTimingSingleTimer();
        cancelStatusDelivered();
        failStatusDelivered();
        malformedStatusFails();
        malformedJsonFails();
        emptySuccessFieldsFail();
        nullSuccessFieldFails();
        beginFailureFails();
        replacementCancelsOld();
        reentrantConsumerDoesNotCorrupt();
        throwingConsumerContained();
        shutdownCancelsPending();
        shutdownQueuedSignInCancelled();
        shutdownDuringBeginCancels();
        nullConsumerIgnored();
        nullActivityFails();
        noForegroundGate();
        noSensitiveEvents();
        System.out.println("PASS: GameGoogleSignIn (" + checks + " checks)");
    }

    // --- Scenarios --------------------------------------------------------------------------------

    static void successDeliversFields() {
        GameGoogleSignIn adapter = begin();
        Consumer consumer = start(adapter);
        check(GoogleBrowserAuth.beginCount == 1, "browser begun once");
        // First poll finds nothing yet, then a Success result arrives.
        Handler.runNext();
        check(consumer.calls == 0, "no callback while result pending");
        GoogleBrowserAuth.results.add(json(SUCCESS, EMAIL, TOKEN));
        Handler.runNext();
        check(consumer.calls == 1, "consumer invoked once on success");
        SignInResponse response = (SignInResponse) consumer.received;
        check(response.status.equals(SUCCESS), "success status delivered");
        check(response.email.equals(EMAIL), "success carries the email string");
        check(response.idToken.equals(TOKEN), "success carries the id token string");
        check(Handler.size() == 0, "no timer remains after completion");
    }

    static void pollTimingSingleTimer() {
        GameGoogleSignIn adapter = begin();
        start(adapter);
        check(Handler.size() == 1 && Handler.delayAt(0) == 250, "first poll scheduled 250ms out");
        Handler.runNext(); // empty result -> reschedule
        check(Handler.size() == 1 && Handler.delayAt(0) == 250, "poll reposts 250ms out as one timer");
        Handler.runNext(); // still empty -> reschedule, never more than one timer
        check(Handler.size() == 1, "only ever one poll timer outstanding");
    }

    static void cancelStatusDelivered() {
        GameGoogleSignIn adapter = begin();
        Consumer consumer = start(adapter);
        GoogleBrowserAuth.results.add(json(CANCELLED, null, null));
        Handler.runNext();
        check(consumer.calls == 1, "cancel result delivered once");
        SignInResponse response = (SignInResponse) consumer.received;
        check(response.status.equals(CANCELLED), "cancelled status delivered");
        check(response.email == null && response.idToken == null, "cancelled carries no email or token");
    }

    static void failStatusDelivered() {
        GameGoogleSignIn adapter = begin();
        Consumer consumer = start(adapter);
        GoogleBrowserAuth.results.add(json(FAIL, null, null));
        Handler.runNext();
        SignInResponse response = (SignInResponse) consumer.received;
        check(response.status.equals(FAIL), "fail status delivered");
        check(response.email == null && response.idToken == null, "fail carries no email or token");
    }

    static void malformedStatusFails() {
        GameGoogleSignIn adapter = begin();
        Consumer consumer = start(adapter);
        GoogleBrowserAuth.results.add("{\"status\":\"Weird\"}");
        Handler.runNext();
        SignInResponse response = (SignInResponse) consumer.received;
        check(response.status.equals(FAIL), "non-whitelisted status reported as Fail");
        check(GoogleBrowserAuth.cancelled.contains(GoogleBrowserAuth.lastId), "malformed status cancels and drains");
    }

    static void malformedJsonFails() {
        GameGoogleSignIn adapter = begin();
        Consumer consumer = start(adapter);
        GoogleBrowserAuth.results.add("this is not json");
        Handler.runNext();
        SignInResponse response = (SignInResponse) consumer.received;
        check(response.status.equals(FAIL), "unparseable result reported as Fail");
        check(GoogleBrowserAuth.cancelled.contains(GoogleBrowserAuth.lastId), "malformed JSON cancels and drains");
    }

    static void emptySuccessFieldsFail() {
        GameGoogleSignIn adapter = begin();
        Consumer consumer = start(adapter);
        GoogleBrowserAuth.results.add(json(SUCCESS, "", TOKEN));
        Handler.runNext();
        check(((SignInResponse) consumer.received).status.equals(FAIL), "empty email fails the success");
    }

    static void nullSuccessFieldFails() {
        GameGoogleSignIn adapter = begin();
        Consumer consumer = start(adapter);
        // A Success whose idToken is JSON null is not a String and must fail without logging contents.
        GoogleBrowserAuth.results.add("{\"status\":\"Success\",\"email\":\"" + EMAIL + "\",\"idToken\":null}");
        Handler.runNext();
        check(((SignInResponse) consumer.received).status.equals(FAIL), "null token material fails the success");
    }

    static void beginFailureFails() {
        GameGoogleSignIn adapter = begin();
        GoogleBrowserAuth.beginThrows = true;
        Consumer consumer = new Consumer();
        adapter.signIn(new Activity(), consumer);
        Handler.runNext();
        check(consumer.calls == 1, "begin failure delivers once");
        check(((SignInResponse) consumer.received).status.equals(FAIL), "begin failure reported as Fail");
        check(Handler.size() == 0, "begin failure schedules no poll");
    }

    static void replacementCancelsOld() {
        GameGoogleSignIn adapter = begin();
        Consumer first = start(adapter);
        String firstId = GoogleBrowserAuth.lastId;
        GoogleBrowserAuth.nextId = "id-second";
        Consumer second = new Consumer();
        adapter.signIn(new Activity(), second);
        Handler.runNext(); // replacement runnable runs ahead of the old poll (delay 0 < 250)
        check(first.calls == 1, "replaced consumer invoked exactly once");
        check(((SignInResponse) first.received).status.equals(CANCELLED), "replaced consumer gets Cancelled");
        check(GoogleBrowserAuth.cancelled.contains(firstId), "old request cancelled and drained");
        check(second.calls == 0, "new consumer still pending");
        check(Handler.size() == 1 && Handler.delayAt(0) == 250, "replacement leaves a single new poll timer");
        GoogleBrowserAuth.results.add(json(SUCCESS, EMAIL, TOKEN));
        Handler.runNext();
        check(((SignInResponse) second.received).status.equals(SUCCESS), "new request completes normally");
    }

    static void reentrantConsumerDoesNotCorrupt() {
        GameGoogleSignIn adapter = begin();
        Consumer reentrant = new Consumer();
        reentrant.reentrant = adapter; // on invoke, start another sign-in
        adapter.signIn(new Activity(), reentrant);
        Handler.runNext(); // begin
        GoogleBrowserAuth.results.add(json(SUCCESS, EMAIL, TOKEN));
        int beginsBefore = GoogleBrowserAuth.beginCount;
        Handler.runNext(); // poll -> complete -> consumer reentrantly queues a new signIn
        check(reentrant.calls == 1, "reentrant consumer invoked once");
        check(GoogleBrowserAuth.beginCount == beginsBefore, "reentrant signIn is queued, not run inline");
        check(Handler.size() == 1, "reentrant signIn left exactly one queued task");
        Handler.runNext(); // the queued reentrant signIn begins a fresh request
        check(GoogleBrowserAuth.beginCount == beginsBefore + 1, "queued reentrant signIn begins a clean request");
    }

    static void throwingConsumerContained() {
        GameGoogleSignIn adapter = begin();
        Consumer consumer = new Consumer();
        consumer.throwOnInvoke = true;
        adapter.signIn(new Activity(), consumer);
        Handler.runNext();
        GoogleBrowserAuth.results.add(json(SUCCESS, EMAIL, TOKEN));
        Handler.runNext(); // consumer throws; must be contained
        check(consumer.calls == 1, "throwing consumer was invoked");
        check(Handler.size() == 0, "pending detached before a throwing consumer ran");
        check(Control.events.contains("google-browser|Game sign-in callback failed"), "throw logged as a constant");
    }

    static void shutdownCancelsPending() {
        GameGoogleSignIn adapter = begin();
        Consumer consumer = start(adapter);
        String id = GoogleBrowserAuth.lastId;
        adapter.shutdown();
        Handler.runNext(); // shutdownOnMain ahead of the old poll
        check(consumer.calls == 1, "pending consumer cancelled once on shutdown");
        check(((SignInResponse) consumer.received).status.equals(CANCELLED), "shutdown delivers Cancelled");
        check(GoogleBrowserAuth.cancelled.contains(id), "shutdown cancels and drains the pending request");
        check(Handler.size() == 0, "shutdown removes the poll timer");
    }

    static void shutdownQueuedSignInCancelled() {
        GameGoogleSignIn adapter = begin();
        adapter.shutdown();
        Handler.runNext(); // drain the shutdown runnable
        int beginsBefore = GoogleBrowserAuth.beginCount;
        Consumer consumer = new Consumer();
        adapter.signIn(new Activity(), consumer);
        Handler.runNext();
        check(consumer.calls == 1, "signIn after shutdown completes once");
        check(((SignInResponse) consumer.received).status.equals(CANCELLED), "post-shutdown signIn is Cancelled");
        check(GoogleBrowserAuth.beginCount == beginsBefore, "post-shutdown signIn never begins a browser");
    }

    static void shutdownDuringBeginCancels() {
        GameGoogleSignIn adapter = begin();
        Consumer consumer = new Consumer();
        GoogleBrowserAuth.onBegin = () -> adapter.shutdown(); // closed flips while begin runs
        adapter.signIn(new Activity(), consumer);
        Handler.runNext(); // signInOnMain begins, then rechecks closed
        check(consumer.calls == 1, "request cancelled once when shutdown races begin");
        check(((SignInResponse) consumer.received).status.equals(CANCELLED), "begin/shutdown race yields Cancelled");
        check(GoogleBrowserAuth.cancelled.contains(GoogleBrowserAuth.lastId), "raced request cancelled and drained");
    }

    static void nullConsumerIgnored() {
        GameGoogleSignIn adapter = begin();
        adapter.signIn(new Activity(), null);
        check(Handler.size() == 0, "null consumer queues no work");
        check(GoogleBrowserAuth.beginCount == 0, "null consumer begins no browser");
        check(Control.events.contains("google-browser|Ignored sign-in; missing consumer"), "null consumer logged as a constant");
    }

    static void nullActivityFails() {
        GameGoogleSignIn adapter = begin();
        Consumer consumer = new Consumer();
        adapter.signIn(null, consumer);
        Handler.runNext();
        check(consumer.calls == 1, "null activity delivers once");
        check(((SignInResponse) consumer.received).status.equals(FAIL), "null activity reported as Fail");
        check(GoogleBrowserAuth.beginCount == 0, "null activity begins no browser");
    }

    static void noForegroundGate() {
        // Browser sign-in must proceed regardless of any game foreground state. The fake Control has no
        // foreground accessor at all, so production referencing one would fail to compile; this scenario
        // additionally shows a full success round-trip with nothing resembling an activity gate set.
        GameGoogleSignIn adapter = begin();
        Consumer consumer = start(adapter);
        GoogleBrowserAuth.results.add(json(SUCCESS, EMAIL, TOKEN));
        Handler.runNext();
        check(((SignInResponse) consumer.received).status.equals(SUCCESS), "sign-in completes with no foreground gate");
    }

    static void noSensitiveEvents() {
        // Replay success and every failure path, then prove no event line ever carried the email,
        // token, raw JSON or a raw exception.
        GameGoogleSignIn adapter = begin();
        Consumer consumer = start(adapter);
        GoogleBrowserAuth.results.add(json(SUCCESS, EMAIL, TOKEN));
        Handler.runNext();
        Consumer bad = start(adapter);
        GoogleBrowserAuth.results.add("{\"status\":\"Success\",\"email\":\"" + EMAIL + "\",\"idToken\":\"" + TOKEN + "\",oops");
        Handler.runNext();
        for (String event : Control.events) {
            check(!event.contains(EMAIL), "no logged event contains the email");
            check(!event.contains(TOKEN), "no logged event contains the token");
            check(!event.contains("{") && !event.contains("}"), "no logged event contains raw JSON");
        }
        check(consumer.received != null && bad.received != null, "both consumers were driven");
    }

    // --- Helpers ----------------------------------------------------------------------------------

    static GameGoogleSignIn begin() {
        Handler.reset();
        GoogleBrowserAuth.reset();
        Control.events.clear();
        GameGoogleSignIn adapter = GameGoogleSignIn.create();
        Control.events.clear();
        return adapter;
    }

    // Starts a sign-in and runs the queued begin, leaving the request pending with its first poll armed.
    static Consumer start(GameGoogleSignIn adapter) {
        Consumer consumer = new Consumer();
        adapter.signIn(new Activity(), consumer);
        Handler.runNext();
        return consumer;
    }

    static String json(String status, String email, String token) {
        StringBuilder out = new StringBuilder("{\"status\":\"").append(status).append('"');
        if (email != null) out.append(",\"email\":\"").append(email).append('"');
        if (token != null) out.append(",\"idToken\":\"").append(token).append('"');
        return out.append('}').toString();
    }

    static void check(boolean condition, String what) {
        checks++;
        if (!condition) throw new AssertionError("FAIL: " + what);
    }

    /** Fake game consumer implementing the Kotlin Function1 interface. */
    static final class Consumer implements Function1 {
        Object received;
        int calls;
        boolean throwOnInvoke;
        GameGoogleSignIn reentrant;

        public Object invoke(Object argument) {
            calls++;
            received = argument;
            if (reentrant != null) {
                GameGoogleSignIn target = reentrant;
                reentrant = null;
                target.signIn(new Activity(), new Consumer());
            }
            if (throwOnInvoke) throw new RuntimeException("consumer blew up");
            return null;
        }
    }
}
