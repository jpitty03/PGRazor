package dev.pgrazor;

import java.util.Properties;
import java.util.TreeSet;

/**
 * Pure Java Auto-Raid configuration. No Android dependencies: the coordinator-owned {@code AutoRaid}
 * controller holds the authoritative instance and freezes a {@link #copy()} for the duration of a
 * running session, while {@link AutoRaidDialog} edits its own detached copy. Fields are public so the
 * dialog and controller can read and set them directly; {@link #validate()} is the single source of
 * truth for what a complete, launchable configuration looks like, and {@link #read(Properties)}
 * repairs each persisted field independently so one corrupt value never discards the rest.
 *
 * <p>There is no running flag here and none is persisted: whether a session is active is runtime state
 * owned entirely by the controller.
 */
public final class AutoRaidPreferences {
    /** Raid tier bits, in display order. A mask of {@link #TIER_ANY} (no bits) joins any tier. */
    public static final int TIER_ONE_STAR = 1, TIER_THREE_STAR = 2, TIER_FIVE_STAR = 4,
        TIER_MEGA = 8, TIER_SHADOW = 16, TIER_PRIMAL = 32, TIER_ULTRA_BEAST = 64;
    public static final int TIER_ANY = 0, TIER_MASK_ALL = 127;
    public static final String[] TIER_LABELS =
        {"1-star", "3-star", "5-star / Legendary", "Mega", "Shadow", "Primal", "Ultra Beast"};

    public static final int MAX_BOSSES = 200, MIN_BOSS_ID = 1, MAX_BOSS_ID = 2000;
    public static final int MAX_RAIDS_DEFAULT = 5, MAX_RAIDS_MIN = 1, MAX_RAIDS_MAX = 100;
    public static final int MIN_PLAYERS_DEFAULT = 1, MIN_PLAYERS_MIN = 1, MIN_PLAYERS_MAX = 20;
    public static final int WAIT_DEFAULT = 60, WAIT_MIN = 15, WAIT_MAX = 120;

    /** Bonus-catch berry item ids sent to the controller. */
    public static final int BERRY_NONE = 0, BERRY_RAZZ = 701, BERRY_GOLDEN_RAZZ = 706;

    /** Tier filter bitmask; {@link #TIER_ANY} joins any tier. */
    public int tierMask = TIER_ANY;
    /** Canonical comma-separated National Dex IDs (sorted, unique, 1-2000); empty joins any boss. */
    public String bosses = "";
    /** Stop the session once this many raids have been completed. */
    public int maxRaids = MAX_RAIDS_DEFAULT;
    /** Stop after catching a shiny. The shiny raid still counts toward {@link #maxRaids}. */
    public boolean stopOnShiny = false;
    /** Minimum number of trainers required in the lobby before engaging. */
    public int minPlayers = MIN_PLAYERS_DEFAULT;
    /** Seconds to wait in a lobby for others before starting or skipping. */
    public int waitSeconds = WAIT_DEFAULT;
    /** Travel to the raid by walking instead of teleporting (teleport is the default). */
    public boolean walk = false;
    /** Use the free daily raid pass. */
    public boolean freePass = true;
    /** Use Premium Battle passes. Remote Raid passes are never used. */
    public boolean premiumPass = false;
    /** Throw a Razz Berry on bonus catches. */
    public boolean razzBerry = false;
    /**
     * Prefer a Golden Razz Berry on bonus catches. When this is set the controller always falls back to
     * a regular Razz Berry once no Golden remain, whether or not {@link #razzBerry} is also set.
     */
    public boolean goldenRazzBerry = false;

    /**
     * The bonus-catch berry item id sent to the controller: {@link #BERRY_GOLDEN_RAZZ} when Golden is
     * chosen (Golden preferred, with a regular Razz fallback regardless of the {@link #razzBerry} box),
     * {@link #BERRY_RAZZ} when only the Razz box is chosen, or {@link #BERRY_NONE} when neither is.
     */
    public synchronized int berryMode() {
        return goldenRazzBerry ? BERRY_GOLDEN_RAZZ : razzBerry ? BERRY_RAZZ : BERRY_NONE;
    }

    /**
     * The canonical form of a comma-separated boss list: whitespace trimmed, IDs sorted ascending and
     * de-duplicated. A blank string means "any boss". Throws {@link IllegalArgumentException} for a
     * null input, a non-integer token, an ID outside 1-2000, or more than {@link #MAX_BOSSES} entries.
     */
    public static String normalizeBosses(String value) {
        if (value == null) throw new IllegalArgumentException("Enter comma-separated boss Dex IDs, or leave blank for any.");
        if (value.length() > 20000) throw new IllegalArgumentException("Enter at most " + MAX_BOSSES + " boss IDs.");
        String trimmed = value.trim();
        if (trimmed.isEmpty()) return "";
        String[] parts = trimmed.split(",", -1);
        if (parts.length > MAX_BOSSES) throw new IllegalArgumentException("Enter at most " + MAX_BOSSES + " boss IDs.");
        TreeSet<Integer> ids = new TreeSet<>();
        for (String part : parts) {
            int id;
            try { id = Integer.parseInt(part.trim()); }
            catch (NumberFormatException e) { throw new IllegalArgumentException("Use comma-separated National Dex IDs (1-2000)."); }
            if (id < MIN_BOSS_ID || id > MAX_BOSS_ID) throw new IllegalArgumentException("Boss IDs must be 1-2000.");
            ids.add(id);
        }
        StringBuilder out = new StringBuilder();
        for (int id : ids) { if (out.length() > 0) out.append(','); out.append(id); }
        return out.toString();
    }

    /** Human-readable tier filter: "Any tier" when no bits are set, else the selected labels in order. */
    public static String tierSummary(int mask) {
        if ((mask & TIER_MASK_ALL) == 0) return "Any tier";
        StringBuilder out = new StringBuilder();
        for (int i = 0; i < TIER_LABELS.length; i++)
            if ((mask & (1 << i)) != 0) { if (out.length() > 0) out.append(", "); out.append(TIER_LABELS[i]); }
        return out.toString();
    }

    /** A detached copy; the returned object shares no mutable state with this one. */
    public synchronized AutoRaidPreferences copy() {
        AutoRaidPreferences c = new AutoRaidPreferences();
        c.tierMask = tierMask;
        c.bosses = bosses;            // String is immutable, so sharing the reference is safe
        c.maxRaids = maxRaids;
        c.stopOnShiny = stopOnShiny;
        c.minPlayers = minPlayers;
        c.waitSeconds = waitSeconds;
        c.walk = walk;
        c.freePass = freePass;
        c.premiumPass = premiumPass;
        c.razzBerry = razzBerry;
        c.goldenRazzBerry = goldenRazzBerry;
        return c;
    }

    /** Throws {@link IllegalArgumentException} describing the first field that cannot be launched. */
    public synchronized void validate() {
        if (tierMask < 0 || (tierMask & ~TIER_MASK_ALL) != 0)
            throw new IllegalArgumentException("Choose raid tiers to include, or none for any tier.");
        if (bosses == null || !normalizeBosses(bosses).equals(bosses))
            throw new IllegalArgumentException("Boss list must be comma-separated Dex IDs (1-2000), sorted and unique.");
        if (maxRaids < MAX_RAIDS_MIN || maxRaids > MAX_RAIDS_MAX)
            throw new IllegalArgumentException("Maximum raids must be " + MAX_RAIDS_MIN + "-" + MAX_RAIDS_MAX + ".");
        if (minPlayers < MIN_PLAYERS_MIN || minPlayers > MIN_PLAYERS_MAX)
            throw new IllegalArgumentException("Minimum players must be " + MIN_PLAYERS_MIN + "-" + MIN_PLAYERS_MAX + ".");
        if (waitSeconds < WAIT_MIN || waitSeconds > WAIT_MAX)
            throw new IllegalArgumentException("Lobby wait must be " + WAIT_MIN + "-" + WAIT_MAX + " seconds.");
        if (!freePass && !premiumPass)
            throw new IllegalArgumentException("Allow the free daily pass or Premium passes before starting.");
    }

    private static int number(Properties p, String key, int fallback, int min, int max) {
        try { int n = Integer.parseInt(p.getProperty("autoRaid" + key, "")); return n >= min && n <= max ? n : fallback; }
        catch (NumberFormatException e) { return fallback; }
    }
    private static boolean flag(Properties p, String key, boolean fallback) {
        String s = p.getProperty("autoRaid" + key);
        return "true".equals(s) ? true : "false".equals(s) ? false : fallback;
    }

    /** Reads every field independently, falling back to its default when the stored value is malformed. */
    public synchronized void read(Properties p) {
        tierMask = number(p, "TierMask", TIER_ANY, 0, TIER_MASK_ALL);
        try { bosses = normalizeBosses(p.getProperty("autoRaidBosses", "")); }
        catch (IllegalArgumentException e) { bosses = ""; }
        maxRaids = number(p, "MaxRaids", MAX_RAIDS_DEFAULT, MAX_RAIDS_MIN, MAX_RAIDS_MAX);
        stopOnShiny = flag(p, "StopOnShiny", false);
        minPlayers = number(p, "MinPlayers", MIN_PLAYERS_DEFAULT, MIN_PLAYERS_MIN, MIN_PLAYERS_MAX);
        waitSeconds = number(p, "WaitSeconds", WAIT_DEFAULT, WAIT_MIN, WAIT_MAX);
        walk = flag(p, "Walk", false);
        freePass = flag(p, "FreePass", true);
        premiumPass = flag(p, "PremiumPass", false);
        razzBerry = flag(p, "RazzBerry", false);
        goldenRazzBerry = flag(p, "GoldenRazzBerry", false);
    }

    /** Writes every field under the {@code autoRaid} prefix (e.g. {@code autoRaidTierMask}). */
    public synchronized void write(Properties p) {
        p.setProperty("autoRaidTierMask", String.valueOf(tierMask));
        p.setProperty("autoRaidBosses", bosses == null ? "" : bosses);
        p.setProperty("autoRaidMaxRaids", String.valueOf(maxRaids));
        p.setProperty("autoRaidStopOnShiny", String.valueOf(stopOnShiny));
        p.setProperty("autoRaidMinPlayers", String.valueOf(minPlayers));
        p.setProperty("autoRaidWaitSeconds", String.valueOf(waitSeconds));
        p.setProperty("autoRaidWalk", String.valueOf(walk));
        p.setProperty("autoRaidFreePass", String.valueOf(freePass));
        p.setProperty("autoRaidPremiumPass", String.valueOf(premiumPass));
        p.setProperty("autoRaidRazzBerry", String.valueOf(razzBerry));
        p.setProperty("autoRaidGoldenRazzBerry", String.valueOf(goldenRazzBerry));
    }
}
