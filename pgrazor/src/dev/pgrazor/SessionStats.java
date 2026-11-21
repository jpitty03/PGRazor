package dev.pgrazor;

import android.content.Context;
import android.content.SharedPreferences;
import org.json.JSONObject;

/**
 * Session stats: what the signed-in account gained in the last hour and the last 24 hours. The native side
 * reports the game's lifetime counters about once a minute (see session_stats.cpp); this keeps a rolling
 * history per account in its own preferences file, outside the settings backup. The counters are the
 * account's own, so they include play on other devices between readings.
 */
public final class SessionStats {
    private SessionStats() {}
    private static final Object LOCK = new Object();
    private static final long SAVE_MS = 5 * 60_000L;
    private static final SessionStatsHistory history = new SessionStatsHistory();
    private static SharedPreferences store;
    private static String account = "";
    private static long savedAt;
    private static boolean dirty;

    private static String key() {
        String id = AccountProfiles.currentId();
        return id == null || id.isEmpty() ? "unknown" : id;
    }
    static void prepare(Context context) {
        if (context == null) return;
        synchronized (LOCK) {
            if (store != null) return;
            try { store = context.getApplicationContext().getSharedPreferences("pgrazor-session-stats", Context.MODE_PRIVATE); }
            catch (RuntimeException unavailable) { return; }
            account = "";
            follow(System.currentTimeMillis());
        }
    }
    // Under LOCK: switches to the signed-in account's history.
    private static void follow(long now) {
        String current = key();
        if (current.equals(account)) return;
        if (store != null && !account.isEmpty() && dirty) store.edit().putString(account, history.save()).apply();
        account = current;
        history.load(store == null ? "" : store.getString(current, ""), now);
        dirty = false; savedAt = now;
    }
    /** A native "session-stats" event: the counters read just now. */
    static void publish(String detail) {
        double[] values = new double[SessionStatsHistory.READ_COUNT];
        try {
            JSONObject o = new JSONObject(detail);
            if (!o.has("xp")) return;
            values[SessionStatsHistory.XP] = o.getDouble("xp");
            values[SessionStatsHistory.CAUGHT] = o.getDouble("caught");
            values[SessionStatsHistory.SEEN] = o.getDouble("seen");
            values[SessionStatsHistory.STOPS] = o.getDouble("stops");
            values[SessionStatsHistory.KM] = o.getDouble("km");
            values[SessionStatsHistory.HATCHED] = o.getDouble("hatched");
            values[SessionStatsHistory.READ_BALANCE] = o.optDouble("dust", -1);
        } catch (Exception malformed) { return; }
        long now = System.currentTimeMillis();
        synchronized (LOCK) {
            follow(now);
            if (!history.record(now, values)) return;
            dirty = true;
            if (store != null && now - savedAt >= SAVE_MS) { store.edit().putString(account, history.save()).apply(); savedAt = now; dirty = false; }
        }
    }
    /** The change over the last {@code windowMs} for the signed-in account, or null before the first reading. */
    static SessionStatsHistory.Change change(long windowMs) {
        synchronized (LOCK) { follow(System.currentTimeMillis()); return history.change(windowMs); }
    }
    static void flush() {
        synchronized (LOCK) { if (store != null && dirty && !account.isEmpty()) { store.edit().putString(account, history.save()).apply(); savedAt = System.currentTimeMillis(); dirty = false; } }
    }
}
