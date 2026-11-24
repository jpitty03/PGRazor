package dev.pgrazor;

public final class FeedSpotsTest {
    private static void check(boolean condition, String what) {
        if (!condition) throw new AssertionError(what);
    }

    public static void main(String[] args) {
        // Keys must match tools/hundo-feed spot_key(), so one spawn from the server and the phone dedupes by key.
        check(FeedSpots.key(517, 40.621838, -74.112847, 1795302900000L).equals("s5220b522e7c453b5"), "server key 1");
        check(FeedSpots.key(92, 24.7147661, 120.905207, 1795461738000L).equals("s5cb8afec38d5f674"), "server key 2");
        check(FeedSpots.isKey("s5220b522e7c453b5") && !FeedSpots.isKey("12345") && !FeedSpots.isKey("s5220B522e7c453b5") && !FeedSpots.isKey(null), "key format");

        double[] at = FeedSpots.coords("<button data-clipboard-text=\"52.03770,-0.76824\">52.03770,-0.76824</button>");
        check(at != null && at[0] == 52.0377 && at[1] == -0.76824, "coordinates from HTML");
        check(FeedSpots.coords("<b>no place</b>") == null && FeedSpots.coords("95.12345,10.12345") == null
            && FeedSpots.coords("0.000,0.000") == null && FeedSpots.coords(null) == null, "bad coordinates");

        long start = FeedSpots.naiveUtc("2026-11-24 22:08:04");
        check(start == 1795558084000L, "naive time " + start);
        check(FeedSpots.naiveUtc("2026-11-24 25:08:04") == Long.MIN_VALUE && FeedSpots.naiveUtc("24/11/2026") == Long.MIN_VALUE, "bad times");

        // The site's clock was UTC+7 when written: a spawn that started 3 minutes ago at 22:08:04 local.
        long now = start - 7 * 3_600_000L + 180_000;
        check(FeedSpots.offsetMs(start, now) == 7 * 3_600_000L, "UTC+7");
        check(FeedSpots.offsetMs(start - 9 * 3_600_000L, now) == -2 * 3_600_000L, "negative offsets");
        check(FeedSpots.offsetMs(start, now - 7 * 3_600_000L - 600_000) == null, "newest start in the future");
        check(FeedSpots.offsetMs(start - 40 * 60_000, now) == null, "newest start 43 minutes old: rounding could be off an hour");
        check(FeedSpots.offsetMs(start + 20 * 3_600_000L, now) == null, "beyond UTC+14");
        check(FeedSpots.offsetMs(Long.MIN_VALUE, now) == null, "no start times");
        System.out.println("FeedSpotsTest OK");
    }
}
