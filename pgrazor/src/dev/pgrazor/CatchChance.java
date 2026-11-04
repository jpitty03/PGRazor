package dev.pgrazor;

import org.json.JSONArray;
import org.json.JSONObject;

/**
 * The open encounter's catch chance as the native module reports it ("catch-chance" events), and
 * the overlay text for it. Chances come from the server's per-ball capture probability with the
 * active berry, PGRazor's throw rating and curveball applied; the server's probability already includes the type medal bonus.
 */
public final class CatchChance {
    private CatchChance() {}
    private static final Object LOCK = new Object();
    private static boolean active;
    private static String text = "", status = "";

    /** Native "catch-chance" events: an encounter snapshot, or a module status line. */
    public static void publish(String detail) {
        try {
            JSONObject o = new JSONObject(detail);
            String phase = o.optString("phase", "");
            synchronized (LOCK) {
                if ("encounter".equals(phase)) {
                    active = o.optBoolean("active", false) && o.optBoolean("show", false);
                    text = active ? describe(o) : "";
                } else if ("error".equals(phase)) {
                    status = o.optString("message", "");
                }
            }
        } catch (Exception ignored) {}
    }
    public static boolean visible() { synchronized (LOCK) { return active && !text.isEmpty(); } }
    public static String text() { synchronized (LOCK) { return text; } }
    public static String status() { synchronized (LOCK) { return status; } }

    static String ballName(int item) {
        switch (item) { case 1: return "Poké"; case 2: return "Great"; case 3: return "Ultra"; case 4: return "Master"; default: return "Ball " + item; }
    }
    /**
     * "Poké 41% · Great 55% · ▸Ultra 66%" with the selected ball marked (auto ball moves the mark).
     * The chances already include the berry, throw rating and curveball.
     */
    static String describe(JSONObject o) {
        JSONArray balls = o.optJSONArray("balls");
        if (balls == null || balls.length() == 0) return "";
        int current = o.optInt("current", 0);
        StringBuilder line = new StringBuilder();
        for (int i = 0; i < balls.length(); i++) {
            JSONObject b = balls.optJSONObject(i);
            if (b == null) continue;
            int item = b.optInt("item");
            if (line.length() > 0) line.append(" · ");
            if (item == current) line.append("▸");
            line.append(ballName(item)).append(' ')
                .append(Math.round(b.optDouble("chance", 0) * 100)).append('%');
        }
        return line.toString();
    }
}
