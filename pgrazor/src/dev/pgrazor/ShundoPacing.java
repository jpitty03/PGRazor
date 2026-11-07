package dev.pgrazor;

/** Shundo Sniper's timing and stop decisions, kept free of Android so they can be tested on their own. */
final class ShundoPacing {
    private ShundoPacing() {}

    // The feed lists about 4000 spawns and the sniper checks about 90 an hour, so a full queue can wait
    // 5 minutes. A short queue (a species filter, a quiet feed) or a failed read looks again after 1 minute.
    static final long FEED_MS = 300_000, FEED_SOON_MS = 60_000;
    static final int FEED_LOW_QUEUE = 20;

    // Measured: a healthy 44-minute run never had more than 2 empty stops in a row.
    static final int EMPTY_STOPS_LIMIT = 4;
    // A search that ran this few times had the map hidden (a menu was open), so it says nothing about the map.
    static final int EMPTY_MIN_SCANS = 30;

    /** How long after the last feed read the next one is due. */
    static long feedDelay(int queued, boolean lastReadFailed) {
        return lastReadFailed || queued < FEED_LOW_QUEUE ? FEED_SOON_MS : FEED_MS;
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
