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
