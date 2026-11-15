package dev.pgrazor;

public final class EncounterWindowTest {
    static int checks;
    static void eq(Object expected, Object actual) {
        checks++;
        if (expected == null ? actual != null : !expected.equals(actual))
            throw new AssertionError(expected + " != " + actual);
    }

    public static void main(String[] args) {
        final long hour = 60 * 60 * 1000L, start = 1800000000000L;
        EncounterWindow w = new EncounterWindow();
        eq(0, w.total(start));

        // Each encounter id counts once, whoever reports it and however often.
        eq(true, w.count("100", start)); eq(false, w.count("100", start + 5000)); eq(true, w.count("101", start + 5000));
        eq(false, w.count("", start)); eq(false, w.count(null, start)); eq(false, w.count("102", 0));
        eq(2, w.total(start + 5000));

        // A rolling window: counts fall out 24 hours after they were made, not at midnight.
        for (int i = 0; i < 50; i++) w.count("later-" + i, start + 6 * hour);
        eq(52, w.total(start + 23 * hour));
        eq(50, w.total(start + 24 * hour + EncounterWindow.SLOT_MS));   // the first two are older than 24 hours
        eq(50, w.total(start + 29 * hour));
        eq(0, w.total(start + 30 * hour + EncounterWindow.SLOT_MS));

        // Saved and loaded again (a restart): the counts and their ages survive, the ids do not need to.
        EncounterWindow a = new EncounterWindow();
        for (int i = 0; i < 30; i++) a.count("a-" + i, start);
        for (int i = 0; i < 12; i++) a.count("b-" + i, start + 3 * hour);
        String stored = a.save(start + 3 * hour);
        EncounterWindow b = new EncounterWindow(); b.load(stored, start + 4 * hour);
        eq(42, b.total(start + 4 * hour));
        eq(true, b.count("a-0", start + 4 * hour)); eq(43, b.total(start + 4 * hour));
        eq(13, b.total(start + 24 * hour + EncounterWindow.SLOT_MS));   // the first 30 have aged out

        // Malformed or stale storage never breaks the counter.
        EncounterWindow c = new EncounterWindow();
        c.load("junk,5:,:7,-3:4,10:-2," + (start / EncounterWindow.SLOT_MS) + ":9,x:y", start + hour);
        eq(9, c.total(start + hour));
        c.load(null, start); eq(0, c.total(start));
        c.load(stored, start + 40 * hour); eq(0, c.total(start + 40 * hour));   // everything is older than a day
        // The clock set back: counts from "the future" are dropped rather than kept for too long.
        c.load(stored, start - 2 * hour); eq(0, c.total(start - 2 * hour));

        System.out.println("EncounterWindow: " + checks + " checks passed");
    }
}
