package dev.pgrazor;

public final class ShundoPacingTest {
    private static void check(boolean condition, String what) {
        if (!condition) throw new AssertionError(what);
    }

    public static void main(String[] args) {
        check(ShundoPacing.waitReason(true,true,true,true,true).isEmpty(), "healthy search is unchanged");
        check(ShundoPacing.waitReason(true,true,true,false,false).contains("game account"), "location working during account handoff must not permit teleport");
        check(ShundoPacing.waitReason(true,true,true,true,false).contains("game readiness"), "native prerequisites are required");
        check(ShundoPacing.waitReason(true,true,false,true,true).startsWith("Paused"), "background or disabled location cannot teleport");
        check(!ShundoPacing.waitReason(false,true,true,true,true).isEmpty(), "adapter absent");
        check(!ShundoPacing.waitReason(true,false,true,true,true).isEmpty(), "location unknown");
        check(ShundoPacing.feedDelay(4000, false) == ShundoPacing.FEED_MS, "a full queue waits the long interval");
        check(ShundoPacing.feedDelay(ShundoPacing.FEED_LOW_QUEUE, false) == ShundoPacing.FEED_MS, "the threshold itself is not low");
        check(ShundoPacing.feedDelay(ShundoPacing.FEED_LOW_QUEUE - 1, false) == ShundoPacing.FEED_SOON_MS, "a short queue reads again soon");
        check(ShundoPacing.feedDelay(0, false) == ShundoPacing.FEED_SOON_MS, "an empty queue reads again soon");
        check(ShundoPacing.feedDelay(4000, true) == ShundoPacing.FEED_SOON_MS, "a failed read retries soon");

        check(ShundoPacing.LIVE_INTERVALS.length == ShundoPacing.LIVE_INTERVAL_LABELS.length, "live interval labels match options");
        for (int s : new int[]{30, 60, 180, 300, 600})
            check(ShundoPacing.liveInterval(String.valueOf(s)) == s, "known live interval kept: " + s);
        check(ShundoPacing.liveInterval("45") == ShundoPacing.LIVE_DEFAULT_S, "unknown live interval defaults to 5 minutes");
        check(ShundoPacing.liveInterval("") == ShundoPacing.LIVE_DEFAULT_S, "empty live interval defaults to 5 minutes");
        check(ShundoPacing.liveInterval(null) == ShundoPacing.LIVE_DEFAULT_S, "null live interval defaults to 5 minutes");
        check(ShundoPacing.liveDelay(300, 15_000, 0.5) == 300_000, "neutral jitter preserves configured 5 minute delay");
        check(ShundoPacing.liveDelay(30, 15_000, 0.5) == 30_000, "explicit 30 second choice is honored");
        check(ShundoPacing.liveDelay(60, 15_000, 0.5) == 60_000, "explicit 1 minute choice is honored");
        check(ShundoPacing.liveDelay(180, 15_000, 0.5) == 180_000, "explicit 3 minute choice is honored");
        check(ShundoPacing.liveDelay(600, 15_000, 0.5) == 600_000, "explicit 10 minute choice is honored");
        check(ShundoPacing.liveDelay(30, 15_000, 0.0) == 24_000, "low jitter reduces delay by 20% above floor");
        check(ShundoPacing.liveDelay(30, 15_000, 1.0) == 36_000, "high jitter increases delay by 20%");
        check(ShundoPacing.liveDelay(30, 15_000, -5.0) == 24_000, "out-of-range low jitter is clamped");
        check(ShundoPacing.liveDelay(30, 15_000, 5.0) == 36_000, "out-of-range high jitter is clamped");
        check(ShundoPacing.liveDelay(30, 45_000, 0.5) == 45_000, "server refresh floor is respected");
        check(ShundoPacing.liveDelay(30, -10_000, 0.5) == 30_000, "negative server refresh is ignored");
        check(ShundoPacing.liveDelay(30, 15_000, Double.NaN) == 30_000, "non-finite jitter falls back to neutral");
        check(ShundoPacing.rescheduleLive(0, 600, 0.5) == 0, "unpolled session stays ready immediately");
        check(ShundoPacing.rescheduleLive(100_000, 600, 0.5) == 700_000, "lengthening interval waits from last poll without immediate fetch");
        check(ShundoPacing.rescheduleLive(100_000, 60, 0.5) == 160_000, "shortening interval becomes due once elapsed from last poll");

        ShundoPacing.EmptyStops stops = new ShundoPacing.EmptyStops();
        check(!stops.missed(0, 78) && !stops.missed(0, 78) && !stops.missed(0, 78), "three empty stops are not enough");
        check(stops.missed(0, 78), "the fourth empty stop in a row stops the sniper");

        stops = new ShundoPacing.EmptyStops();
        stops.missed(0, 78); stops.missed(0, 78); stops.missed(0, 78);
        check(!stops.missed(3, 78), "seeing wild Pokémon clears the count");
        check(!stops.missed(0, 78) && !stops.missed(0, 78) && !stops.missed(0, 78), "the count starts over after that");

        stops = new ShundoPacing.EmptyStops();
        stops.missed(0, 78); stops.missed(0, 78); stops.missed(0, 78);
        stops.reset();
        check(!stops.missed(0, 78), "a spawn found (reset) clears the count");

        stops = new ShundoPacing.EmptyStops();
        for (int i = 0; i < 10; i++) check(!stops.missed(0, ShundoPacing.EMPTY_MIN_SCANS - 1), "a hidden map never counts");
        stops.missed(0, 78); stops.missed(0, 78); stops.missed(0, 78);
        check(!stops.missed(0, 0), "a hidden map neither counts nor clears");
        check(stops.missed(0, 78), "the count carries on once the map is back");
    }
}
