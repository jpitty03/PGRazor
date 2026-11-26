package dev.pgrazor;

import java.util.Properties;

/**
 * Pure Java Auto Rocket configuration, held by {@link AutoRocket} and edited as a detached copy by
 * {@link AutoRocketDialog}. {@link #read(Properties)} repairs each persisted field on its own; no
 * running flag is stored.
 */
public final class AutoRocketPreferences {
    public static final int TARGETS_GRUNTS = 0, TARGETS_GRUNTS_AND_LEADERS = 1;
    public static final String[] TARGET_LABELS = {"Grunts only", "Grunts and leaders"};
    public static final int MAX_BATTLES_DEFAULT = 20, MAX_BATTLES_MIN = 1, MAX_BATTLES_MAX = 999;
    /** Allowed distance limits in kilometres; 0 is no limit. */
    public static final int[] DISTANCE_CHOICES_KM = {0, 1, 2, 5, 10};

    /** Which Rocket battles to take on: {@link #TARGETS_GRUNTS} or {@link #TARGETS_GRUNTS_AND_LEADERS}. */
    public int targets = TARGETS_GRUNTS;
    /** Stop after this many won or lost battles that reached the end. */
    public int maxBattles = MAX_BATTLES_DEFAULT;
    /** Walk to each stop (the default) instead of teleporting. */
    public boolean walk = true;
    /** Only battle within this many kilometres of where the session started; 0 is no limit. */
    public int maxDistanceKm = 2;
    /** Hand each caught shadow Pokémon to the transfer rules; false keeps every catch. */
    public boolean useTransferRules = false;

    public static boolean validDistance(int km) {
        for (int choice : DISTANCE_CHOICES_KM) if (choice == km) return true;
        return false;
    }
    public static String distanceSummary(int km) { return km <= 0 ? "No limit" : km + " km"; }

    public synchronized AutoRocketPreferences copy() {
        AutoRocketPreferences c = new AutoRocketPreferences();
        c.targets = targets;
        c.maxBattles = maxBattles;
        c.walk = walk;
        c.maxDistanceKm = maxDistanceKm;
        c.useTransferRules = useTransferRules;
        return c;
    }

    /** Throws {@link IllegalArgumentException} describing the first field that cannot be launched. */
    public synchronized void validate() {
        if (targets != TARGETS_GRUNTS && targets != TARGETS_GRUNTS_AND_LEADERS)
            throw new IllegalArgumentException("Choose grunts only, or grunts and leaders.");
        if (maxBattles < MAX_BATTLES_MIN || maxBattles > MAX_BATTLES_MAX)
            throw new IllegalArgumentException("Maximum battles must be " + MAX_BATTLES_MIN + "-" + MAX_BATTLES_MAX + ".");
        if (!validDistance(maxDistanceKm))
            throw new IllegalArgumentException("Choose a distance limit of 1, 2, 5 or 10 km, or no limit.");
    }

    private static int number(Properties p, String key, int fallback, int min, int max) {
        try { int n = Integer.parseInt(p.getProperty("autoRocket" + key, "")); return n >= min && n <= max ? n : fallback; }
        catch (NumberFormatException e) { return fallback; }
    }
    private static boolean flag(Properties p, String key, boolean fallback) {
        String s = p.getProperty("autoRocket" + key);
        return "true".equals(s) ? true : "false".equals(s) ? false : fallback;
    }

    public synchronized void read(Properties p) {
        targets = number(p, "Targets", TARGETS_GRUNTS, TARGETS_GRUNTS, TARGETS_GRUNTS_AND_LEADERS);
        maxBattles = number(p, "MaxBattles", MAX_BATTLES_DEFAULT, MAX_BATTLES_MIN, MAX_BATTLES_MAX);
        walk = flag(p, "Walk", true);
        maxDistanceKm = number(p, "MaxDistanceKm", 2, 0, 10);
        if (!validDistance(maxDistanceKm)) maxDistanceKm = 2;
        useTransferRules = flag(p, "TransferRules", false);
    }

    public synchronized void write(Properties p) {
        p.setProperty("autoRocketTargets", String.valueOf(targets));
        p.setProperty("autoRocketMaxBattles", String.valueOf(maxBattles));
        p.setProperty("autoRocketWalk", String.valueOf(walk));
        p.setProperty("autoRocketMaxDistanceKm", String.valueOf(maxDistanceKm));
        p.setProperty("autoRocketTransferRules", String.valueOf(useTransferRules));
    }
}
