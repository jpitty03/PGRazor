package dev.pgrazor;

public final class SessionStatsHistoryTest {
    static int checks;
    static void check(boolean ok, String what) { checks++; if (!ok) throw new AssertionError(what); }
    static void near(double expected, double actual, String what) { check(Math.abs(expected - actual) < 1e-6, what + ": " + expected + " != " + actual); }
    static double[] v(double xp, double caught, double seen, double stops, double km, double hatched, double dust) {
        return new double[]{xp, caught, seen, stops, km, hatched, dust};
    }
    static double[] v(double xp, double caught, double seen, double stops, double km, double hatched) {
        return v(xp, caught, seen, stops, km, hatched, -1);
    }

    public static void main(String[] args) {
        final long MIN = 60_000L, T0 = 1_795_000_000_000L;
        SessionStatsHistory h = new SessionStatsHistory();
        check(h.change(SessionStatsHistory.HOUR_MS) == null, "no reading yet");
        check(!h.record(T0, v(0, 0, 0, 0, 0, 0)), "an empty proto is not a reading");
        check(!h.record(T0, new double[]{1, 2}), "wrong shape");
        check(!h.record(T0, v(100, Double.NaN, 0, 0, 0, 0)), "NaN");
        check(!h.record(T0, v(100, 0, 0, 0, 0, 0, -5)), "a balance below -1");

        // One reading a minute for 3 hours: +100 XP, +1 catch, +2 seen, +0.01 km and +100 stardust a minute.
        for (int m = 0; m <= 180; m++)
            check(h.record(T0 + m * MIN, v(1_000_000 + 100.0 * m, 5000 + m, 8000 + 2 * m, 3000 + m / 10, 1234.5 + 0.01 * m, 700, 50_000 + 100.0 * m)), "record " + m);
        SessionStatsHistory.Change hour = h.change(SessionStatsHistory.HOUR_MS);
        near(6000, hour.get(SessionStatsHistory.XP), "hour XP");
        near(60, hour.get(SessionStatsHistory.CAUGHT), "hour caught");
        near(120, hour.get(SessionStatsHistory.SEEN), "hour seen");
        near(0.6, hour.get(SessionStatsHistory.KM), "hour km");
        near(0, hour.get(SessionStatsHistory.HATCHED), "hour hatched");
        near(6000, hour.get(SessionStatsHistory.DUST), "hour stardust earned");
        near(0, hour.get(SessionStatsHistory.DUST_SPENT), "nothing spent");
        check(hour.coveredMs == SessionStatsHistory.HOUR_MS, "hour fully covered");
        // Only 3 hours known: the day reports the change since the first reading and says so.
        SessionStatsHistory.Change day = h.change(SessionStatsHistory.DAY_MS);
        near(18000, day.get(SessionStatsHistory.XP), "day XP since first reading");
        check(day.coveredMs == 180 * MIN, "day covers 3 h");

        // Spending 10,000 stardust on a power-up counts as spent, not as negative earnings; earning resumes.
        check(h.record(T0 + 181 * MIN, v(1_018_100, 5181, 8362, 3018, 1236.31, 700, 68_100 - 10_000)), "power-up");
        check(h.record(T0 + 182 * MIN, v(1_018_200, 5182, 8364, 3018, 1236.32, 701, 58_100 + 100)), "record 182");
        // The reading nets that minute's +100 against the 10,000 spent.
        near(9_900, h.change(SessionStatsHistory.HOUR_MS).get(SessionStatsHistory.DUST_SPENT), "spent");
        near(5900, h.change(SessionStatsHistory.HOUR_MS).get(SessionStatsHistory.DUST), "earned keeps its pace");
        near(6000, h.change(SessionStatsHistory.HOUR_MS).get(SessionStatsHistory.XP), "interpolated hour XP");
        near(1, h.change(SessionStatsHistory.HOUR_MS).get(SessionStatsHistory.HATCHED), "hatched counted");
        // A reading without the balance neither earns nor spends, and the next one compares with the last known.
        check(h.record(T0 + 183 * MIN, v(1_018_300, 5183, 8366, 3018, 1236.33, 701)), "no balance");
        check(h.record(T0 + 184 * MIN, v(1_018_400, 5184, 8368, 3018, 1236.34, 701, 58_400)), "balance again");
        near(5900, h.change(SessionStatsHistory.HOUR_MS).get(SessionStatsHistory.DUST), "earned across a missing balance");

        // Save and load keep the numbers; the rounded km does not look like a reset afterwards.
        String saved = h.save();
        SessionStatsHistory r = new SessionStatsHistory();
        r.load(saved, T0 + 184 * MIN);
        near(6000, r.change(SessionStatsHistory.HOUR_MS).get(SessionStatsHistory.XP), "reloaded hour XP");
        near(9_900, r.change(SessionStatsHistory.HOUR_MS).get(SessionStatsHistory.DUST_SPENT), "reloaded spent");
        check(r.record(T0 + 185 * MIN, v(1_018_500, 5185, 8370, 3018, 1236.3496, 701, 58_500)), "km a hair lower is fine");
        near(6000, r.change(SessionStatsHistory.HOUR_MS).get(SessionStatsHistory.XP), "hour after reload (steady 100 XP a minute)");
        near(5900, r.change(SessionStatsHistory.HOUR_MS).get(SessionStatsHistory.DUST), "stardust after reload (the power-up minute earned nothing)");
        check(r.size() > 0, "history kept");

        // History saved before stardust was tracked still loads, with no stardust history.
        SessionStatsHistory old = new SessionStatsHistory();
        old.load((T0) + ":1000;1;2;3;4.5;6|" + (T0 + 10 * MIN) + ":2000;2;3;3;4.6;6", T0 + 10 * MIN);
        near(1000, old.change(SessionStatsHistory.HOUR_MS).get(SessionStatsHistory.XP), "old format XP");
        check(old.record(T0 + 11 * MIN, v(2100, 2, 3, 3, 4.6, 6, 900)), "first balance after old format");
        near(0, old.change(SessionStatsHistory.HOUR_MS).get(SessionStatsHistory.DUST), "first balance earns nothing");

        // A counter going down (another account) starts the history again.
        check(r.record(T0 + 186 * MIN, v(50_000, 10, 20, 5, 3, 1, 100)), "other account reading");
        near(0, r.change(SessionStatsHistory.HOUR_MS).get(SessionStatsHistory.XP), "fresh history");
        near(0, r.change(SessionStatsHistory.HOUR_MS).get(SessionStatsHistory.DUST), "fresh stardust");
        check(r.change(SessionStatsHistory.DAY_MS).coveredMs == 0, "nothing covered yet");

        // A day later the oldest samples are dropped and the day window is full.
        SessionStatsHistory d = new SessionStatsHistory();
        for (int m = 0; m <= 26 * 60; m += 5) d.record(T0 + m * MIN, v(1_000_000 + 10.0 * m, 0, 0, 0, 0, 0, 10.0 * m));
        SessionStatsHistory.Change full = d.change(SessionStatsHistory.DAY_MS);
        near(14400, full.get(SessionStatsHistory.XP), "full day XP");
        near(14400, full.get(SessionStatsHistory.DUST), "full day stardust");
        check(full.coveredMs == SessionStatsHistory.DAY_MS, "day fully covered");
        check(d.size() <= 25 * 12 + 2, "pruned to about 25 hours: " + d.size());
        d.load("garbage|12:1;2|" + saved.substring(0, 20), T0);
        check(d.change(SessionStatsHistory.HOUR_MS) == null, "malformed storage is ignored");
        System.out.println("SessionStatsHistory: " + checks + " checks passed");
    }
}
