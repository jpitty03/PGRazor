package dev.pgrazor;

import android.os.Handler;
import android.os.SystemClock;

/**
 * Exercises the real {@link AutoRaid} controller against the narrow boundary
 * shells under {@code auto_raid_stubs/} (Android handler/clock and a flat JSON
 * parser). {@link AutoRaidPreferences} and {@link MotionEngine} are the real
 * classes, compiled unmodified.
 *
 * <p>The controller keeps its lifecycle state private, so tests observe it
 * through {@link AutoRaid#config()} (which carries the session token and the
 * frozen settings), {@link AutoRaid#status()}, {@link AutoRaid#running()}, the
 * deferred main-thread runnables queued on the {@code Handler} shell, and the
 * side effects recorded on the {@code Control} shell (walks, teleports, saves,
 * events, VGP state, and the engine itself).
 */
public final class AutoRaidControllerTest {
    private static int checks;

    /** A deferred assertion body that may throw the controller's checked exceptions. */
    private interface Action { void run() throws Exception; }

    private static void require(boolean ok, String what) {
        checks++;
        if (!ok) throw new AssertionError(what);
    }

    private static void requireThrows(Action action, String needle, String what) {
        checks++;
        try {
            action.run();
        } catch (Throwable thrown) {
            String message = thrown.getMessage() == null ? "" : thrown.getMessage();
            if (!message.contains(needle))
                throw new AssertionError(what + ": wrong message \"" + message + "\" (wanted \"" + needle + "\")");
            return;
        }
        throw new AssertionError(what + ": expected an exception containing \"" + needle + "\"");
    }

    /** Default, valid, launchable preferences (free pass on, teleport mode). */
    private static AutoRaidPreferences prefs() {
        return new AutoRaidPreferences();
    }

    /** Clean controller + shell state for an independent scenario. */
    private static void setup() {
        AutoRaid.reset();
        Control.engine = new MotionEngine();
        Control.ready = true;
        Control.battle = 1;
        Control.gameRoute = false;
        Control.planning = false;
        Control.walkUnavailable = false;
        Control.vgp = true;
        Control.saves = Control.walks = Control.teleports = Control.events = 0;
        Control.lastEvent = "";
        OneShotAutomation.active = false;
        GiftBatchSettings.active = false;
        BatchEvolutionSettings.active = false;
        ShundoSniper.active = false;
        GymAutomationSettings.active = false;
        SystemClock.now = 100000;
        Cooldown.remaining = 0;
        Handler.queue.clear();
    }

    /** The private session token, read back from {@link AutoRaid#config()}. */
    private static long token() {
        String config = AutoRaid.config();
        if (config == null) throw new AssertionError("expected an active session");
        return Long.parseLong(config.split("\\|")[1]);
    }

    private static String[] config() {
        String config = AutoRaid.config();
        if (config == null) throw new AssertionError("expected an active session");
        return config.split("\\|");
    }

    // Numbers are emitted as bare tokens and coordinates via Double.toString so the
    // boundary JSON shell parses them and no locale turns a decimal point into a comma.
    private static String travelEvent(long token, long serial, double lat, double lon) {
        return "{\"token\":" + token + ",\"phase\":\"travel\",\"travel\":" + serial
            + ",\"latitude\":" + Double.toString(lat) + ",\"longitude\":" + Double.toString(lon) + "}";
    }

    private static String phaseEvent(long token, String phase) {
        return "{\"token\":" + token + ",\"phase\":\"" + phase + "\"}";
    }

    private static String phaseEvent(long token, String phase, int completed, String message) {
        return "{\"token\":" + token + ",\"phase\":\"" + phase + "\",\"completed\":" + completed
            + ",\"message\":\"" + message + "\"}";
    }

    /** Readiness, battle-option, competition, and busy-movement refusals at start(). */
    private static void testStartPreflight() {
        setup();
        Control.ready = false;
        requireThrows(() -> AutoRaid.start(prefs()), "ready", "start refused when game/location not ready");
        require(!AutoRaid.running(), "no session after not-ready refusal");
        Control.ready = true;

        Control.battle = 0;
        requireThrows(() -> AutoRaid.start(prefs()), "quick attack", "start refused without battle bit 0");
        Control.battle = 1;

        OneShotAutomation.active = true;
        requireThrows(() -> AutoRaid.start(prefs()), "current automation", "start refused with one-shot running");
        OneShotAutomation.active = false;

        GymAutomationSettings.active = true;
        requireThrows(() -> AutoRaid.start(prefs()), "current automation", "start refused with gym automation running");
        GymAutomationSettings.active = false;

        Control.gameRoute = true;
        requireThrows(() -> AutoRaid.start(prefs()), "walk or Route", "start refused during a game Route");
        Control.gameRoute = false;

        Control.engine.walkRoute(new double[][]{{10.0, 10.0}});
        require(Control.engine.walking, "engine walking precondition");
        requireThrows(() -> AutoRaid.start(prefs()), "walk or Route", "start refused while the engine walks");
        Control.engine = new MotionEngine();

        Control.planning = true;
        requireThrows(() -> AutoRaid.start(prefs()), "walk or Route", "start refused while planning");
        Control.planning = false;

        require(AutoRaid.config() == null, "no session survived the refusals");
        require(Control.saves == 0, "a refused start never persists settings");
        require(Control.vgp, "a refused start never disables VGP");
    }

    /** A clean start freezes a session copy; the caller's object and later edits can't reach it. */
    private static void testStartAndImmutableSession() {
        setup();
        AutoRaidPreferences caller = prefs();
        caller.tierMask = AutoRaidPreferences.TIER_FIVE_STAR; // 4
        caller.maxRaids = 7;
        caller.minPlayers = 2;
        caller.waitSeconds = 30;
        caller.walk = false;
        caller.premiumPass = true;
        caller.stopOnShiny = true;
        caller.bosses = "150,151";
        AutoRaid.start(caller);

        require(AutoRaid.running(), "running after a clean start");
        require(Control.vgp, "start preserves the saved VGP preference; readiness gates pause it");
        require(Control.saves == 1, "start persists the chosen settings once");
        require(Control.events == 1, "start emits one session event");
        require(Control.lastEvent.equals("auto-raid-session"), "start event tag");

        String[] f = config();
        require(f[0].equals("R1"), "config header");
        require(f[2].equals("1"), "running flag in config");
        require(f[3].equals("4"), "tier mask frozen into session");
        require(f[4].equals("7"), "max raids frozen into session");
        require(f[5].equals("2"), "min players frozen into session");
        require(f[6].equals("30"), "wait seconds frozen into session");
        require(f[9].equals("1"), "premium pass frozen into session");
        require(f[10].equals("1"), "stop-on-shiny frozen into session");
        require(f[11].equals("150,151"), "boss list frozen into session");

        // Mutating the caller's object must not reach the frozen session.
        caller.maxRaids = 99;
        caller.bosses = "1";
        require(config()[4].equals("7"), "caller mutation cannot change a running session");

        // Reconfiguring mid-session is rejected outright.
        requireThrows(() -> AutoRaid.configure(prefs()), "Stop Auto-Raid", "configure blocked while running");

        require(AutoRaid.status().contains("0/7 raids"), "status reports completion over the frozen max");
    }

    /** An event carrying a stale token is ignored silently, with no diagnostic and no state change. */
    private static void testStaleToken() {
        setup();
        AutoRaid.start(prefs());
        long tok = token();
        int eventsBefore = Control.events;

        AutoRaid.publish(travelEvent(tok + 999, 1, 10.0, 20.0));

        require(Control.events == eventsBefore, "a stale-token event produces no diagnostic");
        require(Handler.queue.isEmpty(), "a stale-token travel posts nothing");
        require(AutoRaid.running(), "a stale-token event leaves the session running");
        require(AutoRaid.status().startsWith("Checking raid support"), "a stale-token event leaves the status untouched");
    }

    /** Equal or older travel serials never re-issue a move; they still feed the watchdog. */
    private static void testDuplicateTravel() {
        setup();
        AutoRaidPreferences p = prefs();
        p.walk = false; // teleport mode
        AutoRaid.start(p);
        long tok = token();

        AutoRaid.publish(travelEvent(tok, 3, 10.0, 20.0));
        require(Handler.queue.size() == 1, "fresh travel serial posts a move");
        Handler.drain();
        require(Control.teleports == 1, "fresh travel serial teleports once");

        AutoRaid.publish(travelEvent(tok, 3, 30.0, 40.0));
        require(Handler.queue.isEmpty(), "an equal travel serial posts nothing");
        AutoRaid.publish(travelEvent(tok, 2, 50.0, 60.0));
        require(Handler.queue.isEmpty(), "an older travel serial posts nothing");
        require(Control.teleports == 1, "duplicate travel serials never teleport again");
        require(AutoRaid.running(), "duplicate travel keeps the session alive");
    }

    /** A move posted before stop() is a no-op once it runs: the session is no longer running. */
    private static void testQueuedTravelAfterStop() {
        setup();
        AutoRaidPreferences p = prefs();
        p.walk = false;
        AutoRaid.start(p);
        long tok = token();

        AutoRaid.publish(travelEvent(tok, 5, 11.0, 22.0));
        require(Handler.queue.size() == 1, "travel queued before stop");
        AutoRaid.stop();
        Handler.drain();

        require(Control.teleports == 0, "a travel queued before stop never moves");
        require(AutoRaid.running(), "stop() leaves the session in a stopping state until acknowledged");
        require(AutoRaid.status().startsWith("Stopping"), "stop() status");
    }

    /** A move posted before reset() is a no-op: reset() rotates the token the runnable checks. */
    private static void testQueuedTravelAfterReset() {
        setup();
        AutoRaidPreferences p = prefs();
        p.walk = false;
        AutoRaid.start(p);
        long tok = token();

        AutoRaid.publish(travelEvent(tok, 5, 11.0, 22.0));
        require(Handler.queue.size() == 1, "travel queued before reset");
        AutoRaid.reset();
        Handler.drain();

        require(Control.teleports == 0, "a travel queued before reset never moves");
        require(!AutoRaid.running(), "reset() ends the session");
        require(AutoRaid.config() == null, "reset() clears the session so config() is null");
    }

    /** A done/stopped/error phase while stopping acknowledges the stop and clears the session. */
    private static void testStopAck() {
        setup();
        AutoRaidPreferences p = prefs();
        p.walk = false;
        AutoRaid.start(p);
        long tok = token();

        AutoRaid.stop();
        require(AutoRaid.running(), "running() stays true across the stopping window");

        AutoRaid.publish(phaseEvent(tok, "stopped", 2, "Stopped by controller"));

        require(!AutoRaid.running(), "a stop acknowledgement ends the session");
        require(AutoRaid.config() == null, "config() is null once stopping is acknowledged");
        require(AutoRaid.status().contains("2/5 raids"), "a stop acknowledgement records completed raids");
        require(AutoRaid.status().startsWith("Stopped by controller"), "a stop acknowledgement adopts its message");
    }

    /** No events for too long trips the watchdog on the next tick; the boundary is strict. */
    private static void testWatchdog() {
        setup();
        AutoRaid.start(prefs());

        SystemClock.now = 100000 + 45000; // exactly the limit, not past it
        AutoRaid.tick(false);
        require(AutoRaid.running(), "the watchdog tolerates a silence of exactly its limit");

        SystemClock.now = 100000 + 45001;
        AutoRaid.tick(false);
        require(!AutoRaid.running(), "the watchdog stops a session gone silent past its limit");
        require(AutoRaid.status().contains("stopped responding"), "the watchdog explains why it stopped");
    }

    /** Manual movement during a session hands control back to the user and begins stopping. */
    private static void testManualMovement() {
        setup();
        AutoRaid.start(prefs());

        AutoRaid.tick(true);

        require(AutoRaid.running(), "manual movement enters the stopping state");
        require(AutoRaid.status().startsWith("Stopping: movement changed"), "manual movement explains the stop");
    }

    /** Walk-mode travel starts a real planned route and captures its ownership token. */
    private static void testNormalRouteArrival() {
        setup();
        AutoRaidPreferences p = prefs();
        p.walk = true;
        AutoRaid.start(p);
        long tok = token();

        AutoRaid.publish(travelEvent(tok, 1, 40.0, -70.0));
        require(Handler.queue.size() == 1, "walk-mode travel posts a move");
        Handler.drain();

        require(Control.walks == 1, "walk-mode travel starts a walk route");
        require(Control.teleports == 0, "walk-mode travel never teleports");
        require(Control.engine.walking, "the engine is walking after a walk-mode travel");
        require(Control.engine.routeGeneration() != 0, "the engine owns a live route generation");

        AutoRaid.tick(false);
        require("1".equals(config()[2]), "an unchanged route keeps the session running");
        Control.engine.enabled = true;
        Control.engine.position(40.0, -70.0);
        Control.engine.step(.25);
        require(!Control.engine.walking, "the real engine completed the route");
        AutoRaid.tick(false);
        require("1".equals(config()[2]), "arriving at the raid does not cancel the session");
    }

    /** If something replaces our route, the next tick sees the changed generation and stops. */
    private static void testReplacedRoute() {
        setup();
        AutoRaidPreferences p = prefs();
        p.walk = true;
        AutoRaid.start(p);
        long tok = token();

        AutoRaid.publish(travelEvent(tok, 1, 40.0, -70.0));
        Handler.drain();
        require(Control.engine.walking, "walking before the route is replaced");

        // An external walk replaces ours and bumps the engine's route generation.
        Control.engine.walkRoute(new double[][]{{10.0, 10.0}});

        AutoRaid.tick(false);
        require(AutoRaid.running(), "a replaced route enters the stopping state");
        require(AutoRaid.status().startsWith("Stopping: movement changed"), "a replaced route explains the stop");
    }

    private static void testCooldownAndManualTeleport() {
        setup(); AutoRaid.start(prefs());
        Cooldown.remaining=12345;
        require("12345".equals(config()[12]),"current cooldown travels with command");
        Cooldown.remaining=Long.MAX_VALUE;
        require("7200000".equals(config()[12]),"cooldown bounded on wire");
        AutoRaid.publish(travelEvent(token(),1,10,20)); Handler.drain();
        AutoRaid.tick(false);
        require("1".equals(config()[2]),"owned teleport keeps session active");
        Control.teleport(30,40); AutoRaid.tick(false);
        require("0".equals(config()[2]),"manual teleport cancels even without an owned walk");
    }

    private static void testInvalidTravelDoesNotConsumeSerial() {
        setup(); AutoRaid.start(prefs()); long tok=token();
        AutoRaid.publish(travelEvent(tok,1,91,20));
        require(Handler.queue.isEmpty(),"invalid coordinates cannot move");
        AutoRaid.publish(travelEvent(tok,1,10,20)); Handler.drain();
        require(Control.teleports==1,"corrected travel can reuse rejected serial");
        SystemClock.now+=45001;
        AutoRaid.publish(travelEvent(tok,2,91,20)); AutoRaid.tick(false);
        require(!AutoRaid.running(),"invalid traffic cannot keep watchdog alive");
    }

    public static void main(String[] args) {
        testStartPreflight();
        testStartAndImmutableSession();
        testStaleToken();
        testDuplicateTravel();
        testQueuedTravelAfterStop();
        testQueuedTravelAfterReset();
        testStopAck();
        testWatchdog();
        testManualMovement();
        testNormalRouteArrival();
        testReplacedRoute();
        testCooldownAndManualTeleport();
        testInvalidTravelDoesNotConsumeSerial();
        System.out.println("PASS AutoRaid: " + checks + " checks");
    }
}
