import dev.pgrazor.MotionEngine;
import dev.pgrazor.AutoWalkPlanner;

/** Real-engine loop behaviour and pure planner validation; no Android dependencies. */
public class AutoWalkTest {
    static final double EARTH = 6371008.8;
    static void check(boolean ok, String message) { if (!ok) throw new AssertionError(message); }
    static void near(double actual, double expected, double tolerance, String message) {
        check(Double.isFinite(actual) && Math.abs(actual - expected) <= tolerance, message + ": " + actual + " expected " + expected);
    }
    static void rejects(Runnable body, String message) {
        try { body.run(); throw new AssertionError(message); } catch (IllegalArgumentException expected) {}
    }
    // Independent haversine, rather than the engine's/planner's own measurement.
    static double meters(double lat1, double lon1, double lat2, double lon2) {
        double a = Math.sin(Math.toRadians(lat2 - lat1) / 2), b = Math.sin(Math.toRadians(lon2 - lon1) / 2);
        double h = a * a + Math.cos(Math.toRadians(lat1)) * Math.cos(Math.toRadians(lat2)) * b * b;
        return 2 * EARTH * Math.asin(Math.sqrt(Math.min(1, Math.max(0, h))));
    }
    static MotionEngine at(double latitude, double longitude) {
        MotionEngine e = new MotionEngine(); e.position(latitude, longitude); e.speed(3.6); return e; // 1 m/s
    }
    static final double DEG = Math.toDegrees(1.0 / EARTH); // one metre expressed in degrees on the equator

    static void testLoopEndAndBudget() {
        // A one-shot route stops at its final stop; the same route looping keeps walking past it.
        double[][] route = {{0, 0}, {0, 0.3 * DEG}};
        MotionEngine one = at(0, 0); one.speed(7.2); one.walkRoute(route, false); // 2 m/s -> 0.5 m / frame
        one.step(.25);
        check(!one.walking, "One-shot route stops at the final stop");
        near(one.longitude, 0.3 * DEG, DEG * 1e-3, "One-shot ends exactly at the last stop");

        MotionEngine loop = at(0, 0); loop.speed(7.2); loop.walkRoute(route, true);
        check(loop.walking && loop.enabled, "Loop route starts walking");
        loop.step(.25);
        // Reaches the last stop (0.3 m east) then wraps and spends only the unused 0.2 m back toward the
        // first stop, ending 0.1 m east. A fresh per-leg budget would have carried it past the first stop.
        check(loop.walking, "Looping route keeps walking past the final stop");
        near(loop.latitude, 0, 1e-9, "Equator loop stays on the equator");
        near(loop.longitude, 0.1 * DEG, DEG * 1e-3, "Wrap keeps only the frame's unused distance, no extra budget");
        near(meters(0, 0.3 * DEG, loop.latitude, loop.longitude), 0.2, 1e-4, "Exactly the leftover 0.2 m is spent after the wrap");
        check(loop.routeProgress().total == 2 && loop.routeProgress().active, "Loop keeps its two-stop route active");

        // Long-run: it never stops on its own and stays bounded within the route span.
        MotionEngine run = at(0, 0); run.walkRoute(new double[][]{{0, 2 * DEG}, {0, 4 * DEG}}, true);
        for (int i = 0; i < 400; i++) run.step(.25); // 100 m of travel over many laps; must terminate each frame
        check(run.walking, "Loop runs indefinitely without stopping");
        near(run.latitude, 0, 1e-9, "Loop stays on the equator");
        check(run.longitude >= -1e-9 && run.longitude <= 4 * DEG + 1e-6, "Loop stays within its longitude span");
    }

    static void testZeroSpeedResume() {
        MotionEngine e = at(0, 0); e.walkTo(0, 1); e.step(.25);
        double lat = e.latitude, lon = e.longitude;
        check(lon > 0 && e.walking, "Route is under way before pausing");
        e.speed(0);
        check(e.speedKmh == 0 && e.metersPerSecond() == 0, "Zero speed pauses travel without an automatic positive speed");
        e.step(.25);
        check(e.latitude == lat && e.longitude == lon && e.walking, "Zero speed holds position without losing the route");
        MotionEngine.RouteProgress paused = e.routeProgress();
        check(paused.active && paused.total == 1, "Route stays intact while paused at zero speed");
        e.speed(1.8); e.step(.25);
        check(e.longitude > lon, "Positive speed resumes the same route from where it paused");
        // Zero is the only sub-0.5 value allowed.
        rejects(() -> at(0, 0).speed(0.3), "Rejects speed between 0 and 0.5");
        rejects(() -> at(0, 0).speed(-1), "Rejects negative speed");
        rejects(() -> at(0, 0).speed(31), "Rejects speed above 30");
        rejects(() -> at(0, 0).speed(Double.NaN), "Rejects NaN speed");
    }

    static void testCoincidentLoopBounded() {
        // A fully coincident looping route must settle and never spin (this test hangs if it does).
        MotionEngine e = at(0, 0);
        double[][] coincident = new double[MotionEngine.MAX_ROUTE_STOPS][2]; // all {0,0}
        e.walkRoute(coincident, true);
        check(e.walking && e.enabled, "Coincident loop starts walking with a bounded first lap");
        for (int i = 0; i < 20; i++) e.step(.25);
        check(e.latitude == 0 && e.longitude == 0, "Coincident loop never moves");
        check(e.walking, "Coincident loop keeps walking without spinning");

        // A zero-distance (duplicate) leg mixed with real legs keeps progressing and looping.
        MotionEngine d = at(0, 0);
        d.walkRoute(new double[][]{{0, 2 * DEG}, {0, 2 * DEG}, {0, 4 * DEG}}, true);
        for (int i = 0; i < 300; i++) d.step(.25);
        check(d.walking, "Loop with a duplicate stop keeps walking");
        near(d.latitude, 0, 1e-9, "Duplicate-stop loop stays on the equator");
        check(d.longitude >= -1e-9 && d.longitude <= 4 * DEG + 1e-6, "Duplicate-stop loop stays within its span");
    }

    static void testCancellationClearsLoop() {
        double[][] route = {{0, 2 * DEG}, {0, 4 * DEG}};
        MotionEngine e = at(0, 0); e.walkRoute(route, true); e.step(.25);
        check(e.walking, "Loop is under way");
        e.stop();
        check(!e.walking && e.routeProgress().total == 0, "Stop clears the looping route");
        double lat = e.latitude, lon = e.longitude; e.step(.25);
        check(e.latitude == lat && e.longitude == lon, "Stopped loop never resumes");
        // After cancelling a loop, a fresh one-shot route must not inherit looping and must stop at its end.
        e.speed(7.2); e.walkRoute(new double[][]{{0, 0}, {0, 0.3 * DEG}}, false); e.step(.25); e.step(.25);
        check(!e.walking, "A one-shot route started after a loop does not keep looping");

        e = at(0, 0); e.walkRoute(route, true); e.input(1, 0);
        check(!e.walking && e.routeProgress().total == 0, "Manual input cancels the looping route");
        e = at(0, 0); e.walkRoute(route, true); e.pause();
        check(!e.enabled && !e.walking && e.routeProgress().total == 0, "Pause cancels the looping route");
        e.enabled = true; double plat = e.latitude, plon = e.longitude; e.step(.25);
        check(e.latitude == plat && e.longitude == plon, "Re-enabling never resumes a cancelled loop");
    }

    static void testPlannerCountAndValidation() {
        double[][] pts = {{0, 10 * DEG}, {0, 30 * DEG}, {0, 20 * DEG}}; // A(10m), B(30m), C(20m) east of origin
        check(AutoWalkPlanner.MAX_STOPS == MotionEngine.MAX_ROUTE_STOPS, "Nearby planner supports the route engine capacity");
        rejects(() -> AutoWalkPlanner.plan(0, 0, pts, 0), "Rejects a zero stop count");
        rejects(() -> AutoWalkPlanner.plan(0, 0, pts, AutoWalkPlanner.MAX_STOPS + 1), "Rejects a count above the ceiling");
        rejects(() -> AutoWalkPlanner.plan(Double.NaN, 0, pts, 3), "Rejects a non-finite avatar position");
        rejects(() -> AutoWalkPlanner.plan(0, 181, pts, 3), "Rejects an out-of-range avatar position");

        AutoWalkPlanner.Plan empty = AutoWalkPlanner.plan(0, 0, new double[0][], 10);
        check(empty.empty() && empty.available == 0 && empty.selected() == 0, "No loaded POIs yields an empty plan");
        check(empty.fewerThanRequested(), "An empty plan is honestly fewer than requested");

        AutoWalkPlanner.Plan capped = AutoWalkPlanner.plan(0, 0, pts, 2);
        check(capped.available == 3 && capped.selected() == 2, "Plan never pads beyond available, and honours the count");
        check(!capped.fewerThanRequested(), "Two of three for a count of two is not fewer than requested");

        AutoWalkPlanner.Plan over = AutoWalkPlanner.plan(0, 0, pts, 5);
        check(over.available == 3 && over.selected() == 3 && over.fewerThanRequested(), "Requesting more than loaded selects all and reports fewer");
    }

    static void testPlannerNearestNeighbourDeterminism() {
        // From origin: A(10m) nearest, then C(20m) before B(30m).
        double[][] pts = {{0, 10 * DEG}, {0, 30 * DEG}, {0, 20 * DEG}};
        AutoWalkPlanner.Plan plan = AutoWalkPlanner.plan(0, 0, pts, 3);
        check(plan.selected() == 3, "All three unique POIs selected");
        near(plan.route[0][1], 10 * DEG, DEG * 1e-9, "First stop is the nearest POI");
        near(plan.route[1][1], 20 * DEG, DEG * 1e-9, "Nearest-neighbour continues to the next closest");
        near(plan.route[2][1], 30 * DEG, DEG * 1e-9, "Nearest-neighbour finishes at the farthest");
        // Determinism: identical inputs give identical routes.
        AutoWalkPlanner.Plan again = AutoWalkPlanner.plan(0, 0, pts, 3);
        for (int i = 0; i < 3; i++) check(again.route[i][1] == plan.route[i][1], "Repeat plans are identical");
        // Ties break on first-seen order (strict less-than keeps the earliest index).
        AutoWalkPlanner.Plan tie = AutoWalkPlanner.plan(0, 0, new double[][]{{0, -10 * DEG}, {0, 10 * DEG}}, 2);
        near(tie.route[0][1], -10 * DEG, DEG * 1e-9, "Equidistant tie resolves to the first-seen POI");
        near(tie.route[1][1], 10 * DEG, DEG * 1e-9, "Then the remaining equidistant POI");
    }

    static void testPlannerFilterAndDedupe() {
        // Exact duplicate and a sub-metre near-duplicate both collapse to one stop.
        double[][] dupes = {{0, 0}, {0, 0}, {1e-7, 0}}; // 1e-7 deg ~ 0.011 m, within the 1 m dedupe radius
        check(AutoWalkPlanner.filter(dupes).length == 1, "Exact and sub-metre duplicates collapse to one POI");
        // Points more than a metre apart are kept distinct.
        check(AutoWalkPlanner.filter(new double[][]{{0, 0}, {0, 2 * DEG}}).length == 2, "POIs over a metre apart stay distinct");
        // Invalid entries are dropped; the one valid point survives.
        double[][] mixed = {{Double.NaN, 0}, {0, Double.POSITIVE_INFINITY}, {91, 0}, {0, 181}, null, {1}, {0, 0}};
        double[][] kept = AutoWalkPlanner.filter(mixed);
        check(kept.length == 1 && kept[0][0] == 0 && kept[0][1] == 0, "Non-finite, out-of-range, null and malformed POIs are filtered out");
        // A plan over dirty input still routes only the clean, de-duplicated POIs.
        AutoWalkPlanner.Plan plan = AutoWalkPlanner.plan(0, 0, mixed, 5);
        check(plan.available == 1 && plan.selected() == 1, "Dirty input routes only its one valid POI");
        check(AutoWalkPlanner.filter(null).length == 0, "Null POI input filters to nothing");
    }

    public static void main(String[] args) {
        MotionEngine tiny=at(0,0);tiny.speed(30);tiny.walkRoute(new double[][]{{0,0},{0,0.000001*DEG}},true);
        tiny.step(.25);check(tiny.walking&&Double.isFinite(tiny.longitude),"Tiny nonzero loop returns within bounded frame work");
        testLoopEndAndBudget();
        testZeroSpeedResume();
        testCoincidentLoopBounded();
        testCancellationClearsLoop();
        testPlannerCountAndValidation();
        double[][] nearby = new double[512][2];
        for (int i = 0; i < nearby.length; i++) nearby[i] = new double[]{0, (i + 1) * 2 * DEG};
        AutoWalkPlanner.Plan all = AutoWalkPlanner.plan(0, 0, nearby, nearby.length);
        check(all.selected() == nearby.length, "All 512 loaded POIs are included beyond the old 50 limit");
        for (int i = 0; i < nearby.length; i++) near(all.route[i][1], nearby[i][1], 1e-12, "Every stop visited once in nearest order");
        MotionEngine full = at(0, 0); full.walkRoute(all.route, false);
        check(full.routeProgress().total == nearby.length, "Engine accepts the full nearby route");
        testPlannerNearestNeighbourDeterminism();
        testPlannerFilterAndDedupe();
        System.out.println("PASS: autowalk loop end and frame budget, zero-speed resume, bounded coincident/duplicate loops, "
            + "loop cancellation clears looping, planner count/validation, deterministic nearest-neighbour and filter/dedupe");
    }
}
