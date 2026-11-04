package dev.pgrazor;

import java.util.Locale;
import org.json.JSONArray;
import org.json.JSONObject;

/**
 * The open encounter's catch chance as the native module reports it ("catch-chance" events), and
 * the overlay text for it. Chances come from the server's per-ball capture probability with the
 * active berry, PGRazor's throw rating and curveball applied; type medals are not included.
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
    static String berryName(int item) {
        switch (item) {
            case 701: return "Razz"; case 703: return "Nanab"; case 705: return "Pinap";
            case 706: return "Golden Razz"; case 707: return "Silver Nanab"; case 708: return "Silver Pinap";
            default: return "";
        }
    }
    /**
     * "Catch  Poké 41% · Great 55% · ▸Ultra 66%" with the selected ball marked, then what the
     * chances include: "Razz · Excellent curve" (or "No berry or throw bonus").
     */
    static String describe(JSONObject o) {
        JSONArray balls = o.optJSONArray("balls");
        if (balls == null || balls.length() == 0) return "";
        int current = o.optInt("current", 0);
        StringBuilder line = new StringBuilder("Catch  ");
        for (int i = 0; i < balls.length(); i++) {
            JSONObject b = balls.optJSONObject(i);
            if (b == null) continue;
            int item = b.optInt("item");
            if (i > 0) line.append(" · ");
            if (item == current) line.append("▸");
            line.append(ballName(item)).append(' ')
                .append(Math.round(b.optDouble("chance", 0) * 100)).append('%');
            if (b.optInt("count", 1) == 0) line.append(" (0)");
        }
        // What the chances include, then (separately) what Auto ball chose and the target it used.
        String berry = berryName(o.optInt("berry", 0)), grade = o.optString("throw", "");
        boolean curve = o.optBoolean("curve", false);
        String throwText = grade.isEmpty() ? (curve ? "Curveball" : "") : grade + (curve ? " curveball" : " throw");
        StringBuilder with = new StringBuilder();
        if (!berry.isEmpty() || !throwText.isEmpty()) {
            with.append("With ").append(berry);
            if (!berry.isEmpty() && !throwText.isEmpty()) with.append(", ");
            with.append(throwText);
        } else with.append("No berry or throw bonus");
        if (o.optBoolean("auto", false)) {
            int pick = o.optInt("pick", 0);
            with.append(" · Auto ball: ").append(pick > 0 ? ballName(pick) : "none")
                .append(String.format(Locale.US, " (target %d%%)", o.optInt("target", 75)));
        }
        return line + "\n" + with;
    }
}
