package dev.pgrazor;

/**
 * Narrow shell of Control exposing only the members AutoRaid touches, plus
 * test-visible counters/flags. No game, no Android UI: tests set readiness,
 * battle options, walk/teleport availability, and sibling-walk state directly,
 * and read back how many times the controller persisted, walked, teleported, or
 * reported an event (and the last event tag).
 *
 * <p>{@code battle} mirrors the in-battle option bitmask the controller preflight
 * reads; AutoRaid refuses to start unless bit 0 (Raid auto quick attack) is set,
 * so it defaults to 1 here and a test clears it to exercise that refusal.
 */
public class Control {
    public static MotionEngine engine = new MotionEngine();
    public static boolean ready = true, gameRoute, planning, vgp = true, walkUnavailable;
    public static int saves, walks, teleports, events, battle = 1;
    public static String lastEvent = "";

    public static boolean automationReady() { return ready; }
    public static int battleOptions() { return battle; }
    public static boolean gameRoutesRunning() { return gameRoute; }
    public static boolean automationPlanning() { return planning; }

    public static void walkRoute(double[][] points) {
        if (walkUnavailable) throw new IllegalArgumentException("map unavailable");
        walks++;
        engine.walkRoute(points);
    }

    public static void teleport(double lat, double lon) {
        teleports++;
        engine.position(lat, lon);
        engine.stop();
    }

    public static void saveSettings() { saves++; }
    public static void setVgpEnabled(boolean value) { vgp = value; }
    public static void event(String kind, String detail) { events++; lastEvent = kind; }

    /**
     * Main-thread bridge the controller posts its deferred travel through. The real
     * Control takes its own state lock here (matching the reload/export lock order)
     * before re-entering {@link AutoRaid#travel}; the single-threaded tests just
     * forward, which is enough to exercise the travel re-validation path.
     */
    public static void autoRaidTravel(long received, long serial, double lat, double lon) {
        AutoRaid.travel(received, serial, lat, lon);
    }
}
