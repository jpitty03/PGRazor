package dev.pgrazor;

/** Shundo Sniper's timing and stop decisions, kept free of Android so they can be tested on their own. */
final class ShundoPacing {
    private ShundoPacing() {}

    // The feed lists about 4000 spawns and the sniper checks about 90 an hour, so a full queue can wait
    // 5 minutes. A short queue (a species filter, a quiet feed) or a failed read looks again after 1 minute.
    static final long FEED_MS = 300_000, FEED_SOON_MS = 60_000;
    static final int FEED_LOW_QUEUE = 20;

    // LivePokeMap flags tourist accounts that pull wide viewports continuously at a fixed cadence,
    // so default to 5 minutes and add +/-20% jitter.
    static final int[] LIVE_INTERVALS = {30, 60, 180, 300, 600};
    static final String[] LIVE_INTERVAL_LABELS = {"30 seconds", "1 minute", "3 minutes", "5 minutes", "10 minutes"};
    static final int LIVE_DEFAULT_S = 300;
    static final double LIVE_JITTER_FRACTION = 0.2;
    static final long LIVE_FLOOR_MS = 15_000;

    // Measured: a healthy 44-minute run never had more than 2 empty stops in a row.
    static final int EMPTY_STOPS_LIMIT = 4;
    // A search that ran this few times had the map hidden (a menu was open), so it says nothing about the map.
    static final int EMPTY_MIN_SCANS = 30;

    /** A location adapter alone is not enough: native searches also need a verified account and items. */
    static String waitReason(boolean locationReady, boolean locationKnown, boolean active,
                             boolean accountAllowed, boolean gameplayReady) {
        if (!locationReady || !locationKnown) return "Waiting for the game map";
        if (!active) return "Paused: return to the game and enable location";
        if (!accountAllowed) return "Waiting for game account; open Switch account for details";
        if (!gameplayReady) return "Waiting for game readiness";
        return "";
    }

    /** How long after the last feed read the next one is due. */
    static long feedDelay(int queued, boolean lastReadFailed) {
        return lastReadFailed || queued < FEED_LOW_QUEUE ? FEED_SOON_MS : FEED_MS;
    }

    /** Valid LivePokeMap refresh interval in seconds, else {@link #LIVE_DEFAULT_S}. */
    static int liveInterval(String raw) {
        for (int s : LIVE_INTERVALS) if (String.valueOf(s).equals(raw)) return s;
        return LIVE_DEFAULT_S;
    }

    /**
     * Milliseconds until the next LivePokeMap poll, honoring the user setting, server cadence floor,
     * and +/-20% jitter.
     */
    static long liveDelay(int intervalSeconds, long serverRefreshMs, double jitterUnit) {
        int chosen = liveInterval(String.valueOf(intervalSeconds));
        long base = Math.max(chosen * 1000L, Math.max(0L, serverRefreshMs));
        double clamped = Double.isFinite(jitterUnit) ? Math.max(0.0, Math.min(1.0, jitterUnit)) : 0.5;
        double scale = 1.0 + (clamped * 2.0 - 1.0) * LIVE_JITTER_FRACTION;
        return Math.max(LIVE_FLOOR_MS, Math.round(base * scale));
    }

    /**
     * Next LivePokeMap poll deadline after changing the interval, counted from {@code lastLiveMs}
     * so lengthening the interval does not fire an immediate extra poll while shortening runs as soon as due.
     */
    static long rescheduleLive(long lastLiveMs, int intervalSeconds, double jitterUnit) {
        return lastLiveMs <= 0 ? 0L : lastLiveMs + liveDelay(intervalSeconds, 0L, jitterUnit);
    }

    /** Counts stops in a row where the map showed no wild Pokémon at all, the sign the map stopped loading. */
    static final class EmptyStops {
        private int inARow;

        /** Records a spawn that was not found; true once enough stops in a row have been empty. */
        boolean missed(int wildSeen, int scans) {
            if (wildSeen > 0) inARow = 0;
            else if (scans >= EMPTY_MIN_SCANS) inARow++;
            return inARow >= EMPTY_STOPS_LIMIT;
        }

        void reset() { inARow = 0; }
    }
}
