package dev.pgrazor;

import java.util.Properties;

/**
 * Pure Java Shiny Scanner settings. No Android dependencies. The native scanner reads {@link #config}; the
 * limit is measured against the account's encounters in the last 24 hours (see EncounterCounter), so it
 * leaves room for everything else that encounters Pokemon.
 */
public final class ShinyScanPreferences {
    /** Limits offered on encounters in a rolling 24 hours; 0 is no limit. */
    public static final int[] LIMIT_CHOICES = {2000, 5000, 8000, 0};
    public static final int LIMIT_DEFAULT = 8000;
    public static final int SPEED_CAREFUL = 0, SPEED_NORMAL = 1, SPEED_FAST = 2;
    public static final String[] SPEED_LABELS = {"Careful", "Normal", "Fast"};
    /** Least time between two checks, and how many may wait for an answer at once, per speed. */
    private static final int[] GAP_MS = {2000, 700, 300}, IN_FLIGHT = {1, 2, 3};
    private static final int NO_LIMIT_BUDGET = 1000000;

    public boolean enabled = false;
    public int speed = SPEED_NORMAL;
    /** Canonical comma-separated Dex ids (sorted, unique); empty checks every species. */
    public String species = "";
    public int dailyLimit = LIMIT_DEFAULT;
    /** Post a phone notification (sound and heads-up) when a shiny is found. */
    public boolean notify = true;
    /** Stop the current walk or route when a shiny is found. */
    public boolean stopWalking = true;
    /** Hold a found shiny: send no more checks, so its encounter stays open until it is caught or dismissed. */
    public boolean hold = true;

    public static boolean validLimit(int limit) {
        for (int choice : LIMIT_CHOICES) if (choice == limit) return true;
        return false;
    }
    public static String limitSummary(int limit) { return limit <= 0 ? "No limit" : limit + " in 24 hours"; }
    public static String speedSummary(int speed) {
        return SPEED_LABELS[speed];
    }

    /** Checks still allowed, given the account's encounters in the last 24 hours. */
    public synchronized int remaining(int encounters) {
        return dailyLimit <= 0 ? NO_LIMIT_BUDGET : Math.max(0, dailyLimit - Math.max(0, encounters));
    }

    /**
     * {@code N3|<session>|<on>|<gap ms>|<max in flight>|<budget>|<count>|<hold>|<catch>|<species,...>} for the
     * native scanner. {@code count} asks it to report every encounter for the encounter counter, scanner on or
     * off; {@code catchSeq} changes each time the player asks to catch the found shiny.
     */
    public synchronized String config(long session, boolean allowed, int encounters, boolean count, long catchSeq) {
        return "N3|" + Math.max(1, session) + "|" + (enabled && allowed ? 1 : 0) + "|" + GAP_MS[speed] + "|" + IN_FLIGHT[speed]
            + "|" + remaining(encounters) + "|" + (count ? 1 : 0) + "|" + (hold ? 1 : 0) + "|" + Math.max(0, catchSeq) + "|" + species;
    }

    public synchronized void validate() {
        if (speed < SPEED_CAREFUL || speed > SPEED_FAST) throw new IllegalArgumentException("Choose a scan speed.");
        if (!validLimit(dailyLimit)) throw new IllegalArgumentException("Choose a daily limit.");
        if (species == null || !AutoRaidPreferences.normalizeBosses(species).equals(species))
            throw new IllegalArgumentException("Species must be comma-separated Dex IDs (1-2000), sorted and unique.");
    }

    private static int number(Properties p, String key, int fallback, int min, int max) {
        try { int n = Integer.parseInt(p.getProperty(key, "")); return n >= min && n <= max ? n : fallback; }
        catch (NumberFormatException e) { return fallback; }
    }

    /** Reads every field independently, falling back to its default when the stored value is malformed. */
    public synchronized void read(Properties p) {
        enabled = "true".equals(p.getProperty("shinyScan"));
        speed = number(p, "shinyScanSpeed", SPEED_NORMAL, SPEED_CAREFUL, SPEED_FAST);
        try { species = AutoRaidPreferences.normalizeBosses(p.getProperty("shinyScanSpecies", "")); }
        catch (IllegalArgumentException e) { species = ""; }
        dailyLimit = number(p, "shinyScanDailyLimit", LIMIT_DEFAULT, 0, 1000000);
        if (!validLimit(dailyLimit)) dailyLimit = LIMIT_DEFAULT;
        notify = !"false".equals(p.getProperty("shinyScanNotify"));
        stopWalking = !"false".equals(p.getProperty("shinyScanStopWalk"));
        hold = !"false".equals(p.getProperty("shinyScanHold"));
    }

    public synchronized void write(Properties p) {
        p.setProperty("shinyScan", String.valueOf(enabled));
        p.setProperty("shinyScanSpeed", String.valueOf(speed));
        p.setProperty("shinyScanSpecies", species == null ? "" : species);
        p.setProperty("shinyScanDailyLimit", String.valueOf(dailyLimit));
        p.setProperty("shinyScanNotify", String.valueOf(notify));
        p.setProperty("shinyScanStopWalk", String.valueOf(stopWalking));
        p.setProperty("shinyScanHold", String.valueOf(hold));
    }
}
