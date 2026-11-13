package dev.pgrazor.events;

import android.content.Context;
import android.content.SharedPreferences;

/** Event preferences are separate from Control; runtime capability is never persisted. */
public final class EventSettings {
    public static final String HALLOWEEN_2026 = "halloween_2026";
    public static final String GO_WILD_NOVEMBER_2026 = "go_wild_november_2026";
    private static final String FILE = "pgrazor-events-v1";
    private static final String BOX_COLLECTOR_KEY = HALLOWEEN_2026 + ".box_collector";
    // Go Wild November 2026: two independent, default-off opt-ins that only change the ball used for
    // Mighty Pokemon. Neither enables its base feature (VGP+ / Auto Ball) and neither is capability
    // gated: the native owner validates permitted Safari Balls and falls back to the normal ball rules.
    private static final String WILD_VGP_SAFARI_KEY = GO_WILD_NOVEMBER_2026 + ".vgp_plus_safari";
    private static final String WILD_AUTO_BALL_SAFARI_KEY = GO_WILD_NOVEMBER_2026 + ".auto_ball_safari";
    // Retired preferences: the strict candy-only consent and the experimental
    // empty-box scanner. Their keys are cleared to the default whenever the
    // collector setting is written so stale consent cannot linger on disk. The
    // preferences shell exposes only putBoolean, so "removing" a key means
    // writing its default-off value back; old consent is never read or honored.
    private static final String[] OBSOLETE_KEYS = {
        HALLOWEEN_2026 + ".candy_boxes",
        HALLOWEEN_2026 + ".box_scan_empty",
    };
    private static volatile boolean boxCollectorCapability;

    private EventSettings() {}

    private static SharedPreferences preferences(Context context) {
        return context.getApplicationContext().getSharedPreferences(FILE, Context.MODE_PRIVATE);
    }

    // --- Nearby box collector ------------------------------------------------
    // The single Halloween behavior: auto-open every visible server box and skip
    // (never catch) any Pokemon encounter screen. Its own fresh, default-off
    // opt-in; no prior consent is ever migrated into this key.

    public static boolean boxCollectorRequested(Context context) {
        return preferences(context).getBoolean(BOX_COLLECTOR_KEY, false);
    }

    /** Only the verified native collector integration may publish this process-local capability. */
    public static void publishBoxCollectorCapability(boolean available) {
        boxCollectorCapability = available;
    }

    public static boolean boxCollectorAvailable() {
        return boxCollectorCapability;
    }

    /** Refuse enabling until the native box collector reports itself available. */
    public static synchronized boolean setBoxCollectorRequested(Context context, boolean enabled) {
        if (enabled && !boxCollectorCapability) return false;
        return persist(context, BOX_COLLECTOR_KEY, enabled);
    }

    /** Transport must read this effective gate, never the saved preference alone. */
    public static boolean boxCollectorEnabled(Context context) {
        return boxCollectorCapability && boxCollectorRequested(context);
    }

    // --- Go Wild November 2026 Safari balls ----------------------------------
    // Two independent toggles. Each only applies to Mighty Pokemon and only while its own base
    // feature is already on; neither turns that feature on. There is no capability lock: the native
    // owner independently validates owned permitted Safari Balls and otherwise uses the normal ball
    // rules, so the saved preference is read directly.

    public static boolean wildVgpSafari(Context context) {
        return preferences(context).getBoolean(WILD_VGP_SAFARI_KEY, false);
    }

    public static boolean wildAutoBallSafari(Context context) {
        return preferences(context).getBoolean(WILD_AUTO_BALL_SAFARI_KEY, false);
    }

    public static synchronized boolean setWildVgpSafari(Context context, boolean enabled) {
        return persistSingle(context, WILD_VGP_SAFARI_KEY, enabled);
    }

    public static synchronized boolean setWildAutoBallSafari(Context context, boolean enabled) {
        return persistSingle(context, WILD_AUTO_BALL_SAFARI_KEY, enabled);
    }

    /**
     * Write one Go Wild preference with the same commit-failure rollback as the Halloween collector,
     * but WITHOUT touching any Halloween key. The two events persist separately: changing a Go Wild
     * toggle must never disturb (nor re-clear) Halloween's collector preference or its retired keys.
     */
    private static boolean persistSingle(Context context, String key, boolean enabled) {
        SharedPreferences preferences = preferences(context);
        boolean previous = preferences.getBoolean(key, false);
        if (preferences.edit().putBoolean(key, enabled).commit()) return true;
        preferences.edit().putBoolean(key, previous).commit();
        return false;
    }

    /**
     * Write {@code key} and clear every retired preference in one commit.
     * Android updates its in-memory map even when the disk commit fails, so the
     * previous intent for the written key and each cleared key is restored
     * before reporting the failure, leaving no preference half-applied.
     */
    private static boolean persist(Context context, String key, boolean enabled) {
        SharedPreferences preferences = preferences(context);
        boolean previous = preferences.getBoolean(key, false);
        boolean[] obsoletePrevious = new boolean[OBSOLETE_KEYS.length];
        SharedPreferences.Editor editor = preferences.edit().putBoolean(key, enabled);
        for (int i = 0; i < OBSOLETE_KEYS.length; i++) {
            obsoletePrevious[i] = preferences.getBoolean(OBSOLETE_KEYS[i], false);
            editor.putBoolean(OBSOLETE_KEYS[i], false);
        }
        if (editor.commit()) return true;
        SharedPreferences.Editor restore = preferences.edit().putBoolean(key, previous);
        for (int i = 0; i < OBSOLETE_KEYS.length; i++) {
            restore.putBoolean(OBSOLETE_KEYS[i], obsoletePrevious[i]);
        }
        restore.commit();
        return false;
    }
}
