package dev.pgrazor;

import java.util.Properties;

public final class ShinyScanPreferencesTest {
    static int checks;
    static void eq(Object expected, Object actual) {
        checks++;
        if (expected == null ? actual != null : !expected.equals(actual))
            throw new AssertionError(expected + " != " + actual);
    }
    static void reject(Runnable action) {
        checks++;
        try { action.run(); } catch (IllegalArgumentException expected) { return; }
        throw new AssertionError("Accepted invalid input");
    }

    public static void main(String[] args) {
        // Defaults: off, normal speed, every species, 8,000 encounters in 24 hours.
        ShinyScanPreferences p = new ShinyScanPreferences();
        eq(false, p.enabled); eq(ShinyScanPreferences.SPEED_NORMAL, p.speed); eq("", p.species); eq(8000, p.dailyLimit);
        p.validate();
        eq("N3|7|0|700|2|8000|0|1|0|", p.config(7, true, 0, false, 0));   // off stays off
        eq("N3|7|0|700|2|8000|1|1|0|", p.config(7, true, 0, true, 0));    // the counter can run with the scanner off
        p.enabled = true;
        eq("N3|7|1|700|2|8000|1|1|0|", p.config(7, true, 0, true, 0));
        eq("N3|7|0|700|2|8000|1|1|0|", p.config(7, false, 0, true, 0));   // another feature is in the way
        eq("N3|1|1|700|2|8000|0|1|0|", p.config(0, true, 0, false, 0));   // the session is never zero on the wire

        // Holding can be switched off, and a catch request travels as a number that changes.
        p.hold = false; eq("N3|7|1|700|2|8000|0|0|3|", p.config(7, true, 0, false, 3)); p.hold = true;
        eq("N3|7|1|700|2|8000|0|1|0|", p.config(7, true, 0, false, -5));   // never negative on the wire
        // Speeds and the species list travel with the command.
        p.speed = ShinyScanPreferences.SPEED_CAREFUL; eq("N3|7|1|2000|1|8000|0|1|0|", p.config(7, true, 0, false, 0));
        p.speed = ShinyScanPreferences.SPEED_FAST; p.species = "25,129";
        eq("N3|7|1|300|3|8000|0|1|0|25,129", p.config(7, true, 0, false, 0));

        // The limit is measured against the account's encounters in the last 24 hours.
        p.dailyLimit = 2000;
        eq(2000, p.remaining(0)); eq(1, p.remaining(1999)); eq(0, p.remaining(2000)); eq(0, p.remaining(2500));
        eq(2000, p.remaining(-5));                                   // a bad count never adds budget
        eq("N3|7|1|300|3|0|1|1|0|25,129", p.config(7, true, 2000, true, 0));
        eq("N3|7|1|300|3|500|1|1|0|25,129", p.config(7, true, 1500, true, 0));
        p.dailyLimit = 0; eq(1000000, p.remaining(999999));          // no limit

        // validate() rejects what the dialog never offers.
        ShinyScanPreferences bad = new ShinyScanPreferences();
        bad.speed = 3; reject(bad::validate); bad.speed = -1; reject(bad::validate); bad.speed = 1;
        bad.dailyLimit = 123; reject(bad::validate); bad.dailyLimit = 5000; bad.validate();
        bad.species = "129,25"; reject(bad::validate); bad.species = null; reject(bad::validate);
        eq(true, ShinyScanPreferences.validLimit(0)); eq(false, ShinyScanPreferences.validLimit(-1));
        eq("No limit", ShinyScanPreferences.limitSummary(0)); eq("5000 in 24 hours", ShinyScanPreferences.limitSummary(5000));

        // write()/read() round trip, and independent repair of malformed values.
        Properties saved = new Properties();
        p.dailyLimit = 5000; p.notify = false; p.stopWalking = false; p.hold = false; p.write(saved); eq(7, saved.size());
        ShinyScanPreferences restored = new ShinyScanPreferences(); restored.read(saved);
        eq(true, restored.enabled); eq(ShinyScanPreferences.SPEED_FAST, restored.speed); eq("25,129", restored.species);
        eq(5000, restored.dailyLimit); eq(false, restored.notify); eq(false, restored.stopWalking); eq(false, restored.hold);
        Properties corrupt = new Properties();
        corrupt.setProperty("shinyScan", "yes"); corrupt.setProperty("shinyScanSpeed", "9");
        corrupt.setProperty("shinyScanSpecies", "7,bad"); corrupt.setProperty("shinyScanDailyLimit", "123");
        ShinyScanPreferences repaired = new ShinyScanPreferences(); repaired.read(corrupt);
        eq(false, repaired.enabled); eq(ShinyScanPreferences.SPEED_NORMAL, repaired.speed); eq("", repaired.species);
        eq(8000, repaired.dailyLimit); eq(true, repaired.notify); eq(true, repaired.stopWalking); eq(true, repaired.hold);   // on unless switched off
        repaired.validate();

        System.out.println("ShinyScanPreferences: " + checks + " checks passed");
    }
}
