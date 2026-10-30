package dev.pgrazor.events;

import android.content.Context;
import android.content.SharedPreferences;

/** Event preferences are separate from Control; runtime capability is never persisted. */
public final class EventSettings {
    public static final String HALLOWEEN_2026 = "halloween_2026";
    private static final String FILE = "pgrazor-events-v1";
    private static final String BOX_COLLECTOR_KEY = HALLOWEEN_2026 + ".box_collector";
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
