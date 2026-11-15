package dev.pgrazor;

import android.content.Context;
import android.content.SharedPreferences;
import java.util.Locale;

/**
 * Encounter counter: how many Pokémon this account has encountered in the last 24 hours on this device while
 * PGRazor was running. The native scanner reports every wild Pokémon the game has opened (by the player, VGP+,
 * Shundo Sniper or the Shiny Scanner); checks the Shiny Scanner sent are counted even when they got no answer.
 * Kept per account in its own preferences file, outside the settings backup.
 */
public final class EncounterCounter {
    private EncounterCounter() {}
    private static final Object LOCK = new Object();
    private static final long SAVE_MS = 30000;
    private static final EncounterWindow window = new EncounterWindow();
    private static SharedPreferences store;
    private static String account = "";
    private static long savedAt;
    private static boolean dirty;

    private static String key() {
        String id = AccountProfiles.currentId();
        return id == null || id.isEmpty() ? "unknown" : id;
    }
    /** Opens the stored counts once; until then encounters are still counted in memory. */
    static void prepare(Context context) {
        if (context == null) return;
        synchronized (LOCK) {
            if (store != null) return;
            try { store = context.getApplicationContext().getSharedPreferences("pgrazor-encounters", Context.MODE_PRIVATE); }
            catch (RuntimeException unavailable) { return; }
            account = "";
            follow(System.currentTimeMillis(), true);
        }
    }
    /** Switches to the signed-in account's counts, keeping anything counted before the store was open. */
    private static void follow(long now, boolean merge) {
        String current = key();
        if (current.equals(account)) return;
        if (store != null && !account.isEmpty() && dirty) store.edit().putString(account, window.save(now)).apply();
        int carried = merge ? window.total(now) : 0;
        account = current;
        window.load(store == null ? "" : store.getString(current, ""), now);
        for (int i = 0; i < carried; i++) window.count("carried-" + i, now);
        dirty = carried > 0; savedAt = now;
    }
    /** One encounter, by encounter id; an id already counted is ignored. */
    static void count(String id) {
        if (!Control.encounterCounter()) return;
        long now = System.currentTimeMillis();
        synchronized (LOCK) {
            follow(now, false);
            if (!window.count(id, now)) return;
            dirty = true;
            if (store != null && now - savedAt >= SAVE_MS) { store.edit().putString(account, window.save(now)).apply(); savedAt = now; dirty = false; }
        }
    }
    /** Encounters in the last 24 hours for the signed-in account. */
    public static int total() {
        long now = System.currentTimeMillis();
        synchronized (LOCK) { follow(now, false); return window.total(now); }
    }
    static void flush() {
        long now = System.currentTimeMillis();
        synchronized (LOCK) { if (store != null && dirty && !account.isEmpty()) { store.edit().putString(account, window.save(now)).apply(); savedAt = now; dirty = false; } }
    }
    /** "1,240 encounters / 24 h", or empty while the counter is off. */
    static String label() {
        return Control.encounterCounter() ? String.format(Locale.US, "%,d encounters / 24 h", total()) : "";
    }
    /** The settings title: "PGRazor · 1,240 / 24 h". */
    static String title() {
        return Control.encounterCounter() ? String.format(Locale.US, "PGRazor · %,d / 24 h", total()) : "PGRazor";
    }
}
