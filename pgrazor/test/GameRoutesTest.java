package dev.pgrazor;

import org.json.JSONObject;

/**
 * Source-grounded tests for {@link GameRoutesController}: the full select→walk→start→traverse→
 * complete lifecycle, reversed-route selection, cooldown filtering, malformed-snapshot resilience,
 * no duplicate completion, manual-takeover cancellation, stale/timed-out async requests, lost host
 * lease and refusal to start over an existing active game Route. Runs as pure Java (injected clocks
 * and MotionEngine) with Android's real org.json parser; no device, network or Android runtime.
 */
public final class GameRoutesTest {
    private static void check(boolean condition, String message) { if (!condition) throw new AssertionError(message); }

    // Alpha is reversible with its reverse start at the engine origin (0,0); Bravo sits far away.
    private static String snap(String current) {
        String routes = "\"routes\":[" +
            "{\"id\":\"A\",\"name\":\"Alpha\",\"reversible\":true,\"cooldownFinishMs\":0,\"points\":[[0.01,0.0],[0.0,0.0]]}," +
            "{\"id\":\"B\",\"reversible\":false,\"cooldownFinishMs\":0,\"points\":[[0.02,0.0],[0.03,0.0]]}]";
        return "{" + routes + (current == null ? ",\"current\":null" : "," + current) + "}";
    }
    private static final String STARTED = "\"current\":{\"id\":\"A\",\"started\":true}";
    private static final String ELIGIBLE = "\"current\":{\"id\":\"A\",\"started\":true,\"completedWalk\":true,\"inRangeOfEnd\":true}";
    private static final String REWARD = "\"current\":{\"id\":\"A\",\"acquiredReward\":true}";

    private static void stepUntilIdle(MotionEngine e) {
        for (int i = 0; i < 500_000 && e.routeProgress().active; i++) e.step(0.25);
    }
    private static JSONObject lastCommand(GameRoutesController c) throws Exception {
        String last = "", next;
        while (!(next = c.command()).isEmpty()) { last = next; c.acceptAction("{\"seq\":"+new JSONObject(next).getInt("seq")+",\"status\":\"submitted\"}"); }
        return last.isEmpty() ? null : new JSONObject(last);
    }

    public static void main(String[] args) throws Exception {
        happyPathReversedAndNoDuplicate();
        malformedSnapshotKept();
        cooldownBlocksSelection();
        manualTakeoverCancels();
        cameraInvalidationPreservesArrival();
        lostHostLeaseStops();
        refusesOverActiveRoute();
        asyncTimeoutStops();
        System.out.println("PASS: game-routes lifecycle, reversal, cooldown, malformed, takeover, host, active, timeout");
    }

    /** Full lifecycle with a reversed selection, one bounded completion and no double counting. */
    private static void cameraInvalidationPreservesArrival() throws Exception {
        MotionEngine engine = new MotionEngine();
        engine.position(0, 0);
        GameRoutesController c = new GameRoutesController(engine, () -> 0L, () -> 0L);
        c.setReady(true); c.acceptSnapshot(snap(null)); c.start(1);
        long arrivedToken = engine.routeGeneration();
        check(!engine.walking && engine.routeProgress().visited == 1, "Start waypoint already reached");
        engine.stopManualMotion();
        c.tick(true);
        JSONObject command = lastCommand(c);
        check(c.running() && command != null && "start".equals(command.optString("action")), "Camera invalidation at arrival permits start");
        for (int i = 0; i < 3; i++) { engine.stopManualMotion(); c.tick(true); }
        check(c.running() && engine.routeGeneration() == arrivedToken, "Camera invalidation while waiting preserves route ownership");
        c.acceptSnapshot(snap(STARTED)); c.tick(true);
        check(c.running() && engine.walking, "Server confirmation begins traversal");
        long walkingToken = engine.routeGeneration(); engine.stopManualMotion(); c.tick(true);
        check(c.running() && engine.walking && engine.routeGeneration() == walkingToken, "Camera invalidation preserves active traversal");
        engine.stop(); c.tick(true);
        check(!c.running(), "Explicit stop still revokes route ownership");
    }

    /** Full lifecycle with a reversed selection, one bounded completion and no double counting. */
    private static void happyPathReversedAndNoDuplicate() throws Exception {
        long[] clock = {0, 0};
        MotionEngine engine = new MotionEngine();
        engine.position(0, 0); engine.speed(30);
        GameRoutesController c = new GameRoutesController(engine, () -> clock[0], () -> clock[1]);
        c.setReady(true);
        check(c.acceptSnapshot(snap(null)), "Snapshot accepted");

        c.start(1);
        check(c.running(), "Automation running after start");

        // WALK_TO_START: Alpha's reverse start (0,0) is coincident, so arrival is immediate.
        c.tick(true);
        JSONObject startCmd = lastCommand(c);
        check(startCmd != null && "start".equals(startCmd.optString("action")), "Start requested");
        check("A".equals(startCmd.optString("id")) && startCmd.optBoolean("reversed"), "Reversed Alpha selected");
        check(startCmd.optInt("seq") == 1, "First command seq is 1");

        // Game confirms the session; controller begins walking the ordered geometry once.
        check(c.acceptSnapshot(snap(STARTED)), "Started snapshot accepted");
        c.tick(true);
        check(engine.routeProgress().active, "Walking the route after start confirmation");
        stepUntilIdle(engine);

        // Endpoint arrival alone is not success; completion is requested only once eligible.
        c.tick(true);
        check(lastCommand(c) == null, "No completion before eligibility");
        check(c.acceptSnapshot(snap(ELIGIBLE)), "Eligible snapshot accepted");
        c.tick(true);
        JSONObject doneCmd = lastCommand(c);
        check(doneCmd != null && "complete".equals(doneCmd.optString("action")), "Completion requested when eligible");

        check(c.takeRewardNotice().isEmpty(), "No reward notice before server confirmation");
        // Reward confirmation counts the completion; max=1 then finishes.
        check(c.acceptSnapshot(snap(REWARD)), "Reward snapshot accepted");
        c.tick(true);
        check(!c.running(), "Stopped after the bounded completion");
        check(c.status().contains("Done"), "Done status after completion");

        check(c.takeRewardNotice().startsWith("Route rewards received:"), "Confirmed reward has a notice");
        check(c.takeRewardNotice().isEmpty(), "Reward notice consumed once");
        check(!c.lastReward().isEmpty(), "Last reward retained for details");
        // A repeated reward snapshot must not re-trigger or double count.
        String finished = c.status();
        c.acceptSnapshot(snap(REWARD)); c.tick(true);
        check(!c.running() && c.status().equals(finished), "No duplicate completion after finish");
        check(c.takeRewardNotice().isEmpty(), "Repeated snapshot does not notify again");
    }

    private static void malformedSnapshotKept() {
        long[] clock = {0, 0};
        GameRoutesController c = new GameRoutesController(new MotionEngine(), () -> clock[0], () -> clock[1]);
        check(c.acceptSnapshot(snap(null)), "Valid snapshot accepted");
        String good = c.snapshot();
        check(!c.acceptSnapshot("{not valid json"), "Malformed snapshot rejected");
        check(c.snapshot().equals(good), "Previous snapshot retained after malformed input");
    }

    private static void cooldownBlocksSelection() {
        long[] clock = {0, 10_000}; // wall clock = 10s
        MotionEngine engine = new MotionEngine(); engine.position(0, 0); engine.speed(30);
        GameRoutesController c = new GameRoutesController(engine, () -> clock[0], () -> clock[1]);
        c.setReady(true);
        String cooling = "\"routes\":[" +
            "{\"id\":\"A\",\"reversible\":true,\"cooldownFinishMs\":999999,\"points\":[[0.0,0.0],[0.01,0.0]]}]";
        c.acceptSnapshot("{" + cooling + ",\"current\":null}");
        c.start(3);
        check(!c.running(), "No eligible route while on cooldown");
        check(c.status().contains("No eligible"), "Reports no eligible routes");
    }

    /** Manual joystick input replaces the engine route; automation must cancel, not forge progress. */
    private static void manualTakeoverCancels() {
        long[] clock = {0, 0};
        MotionEngine engine = new MotionEngine(); engine.position(0, 0); engine.speed(30);
        GameRoutesController c = new GameRoutesController(engine, () -> clock[0], () -> clock[1]);
        c.setReady(true); c.acceptSnapshot(snap(null));
        c.start(1); c.tick(true);                 // -> AWAIT_START
        c.acceptSnapshot(snap(STARTED)); c.tick(true); // -> TRAVERSE
        check(engine.routeProgress().active, "Traversing before takeover");
        engine.input(1, 0);                        // user grabs the joystick
        c.tick(true);
        check(!c.running(), "Automation cancelled on manual takeover");
        check(c.status().toLowerCase().contains("stopped") || c.status().toLowerCase().contains("took over"),
            "Takeover reported in status");
    }

    private static void lostHostLeaseStops() {
        long[] clock = {0, 0};
        MotionEngine engine = new MotionEngine(); engine.position(0, 0); engine.speed(30);
        GameRoutesController c = new GameRoutesController(engine, () -> clock[0], () -> clock[1]);
        c.setReady(true); c.acceptSnapshot(snap(null));
        c.start(1);
        c.tick(false);
        check(!c.running(), "Automation stops when the host lease is lost");
        check(c.status().toLowerCase().contains("host"), "Host-unavailable status");
    }

    private static void refusesOverActiveRoute() {
        long[] clock = {0, 0};
        GameRoutesController c = new GameRoutesController(new MotionEngine(), () -> clock[0], () -> clock[1]);
        c.setReady(true);
        c.acceptSnapshot(snap(STARTED)); // a game Route is already active
        c.start(2);
        check(!c.running(), "Does not start over an active game Route");
        check(c.status().toLowerCase().contains("finish") || c.status().toLowerCase().contains("cancel"),
            "Asks the user to finish or cancel first");
    }

    /** An async start request that the game never confirms times out without a blind retry. */
    private static void asyncTimeoutStops() throws Exception {
        long[] clock = {0, 0};
        MotionEngine engine = new MotionEngine(); engine.position(0, 0); engine.speed(30);
        GameRoutesController c = new GameRoutesController(engine, () -> clock[0], () -> clock[1]);
        c.setReady(true); c.acceptSnapshot(snap(null));
        c.start(1); c.tick(true);                 // -> AWAIT_START, start requested
        lastCommand(c);
        clock[0] += GameRoutesController.REQUEST_TIMEOUT_MS + 1;
        c.acceptSnapshot(snap(null)); // native heartbeat continues, but start was never confirmed
        c.tick(true);
        check(!c.running(), "Stops after the start request times out");
        check(lastCommand(c) == null, "No blind retry command after timeout");
    }
}
