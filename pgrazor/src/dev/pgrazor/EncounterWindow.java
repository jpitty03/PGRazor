package dev.pgrazor;

import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.TreeMap;

/**
 * Encounters in a rolling 24 hours, for one account. Pure Java. Counts are kept in ten-minute slots, so the
 * total is exact to within ten minutes; each encounter id is counted once however many times it is reported.
 */
public final class EncounterWindow {
    public static final long WINDOW_MS = 24 * 60 * 60 * 1000L, SLOT_MS = 10 * 60 * 1000L;
    private static final int MAX_IDS = 8192, MAX_SLOTS = 200;
    private final TreeMap<Long, Integer> slots = new TreeMap<>();
    private final LinkedHashMap<String, Boolean> ids = new LinkedHashMap<String, Boolean>() {
        @Override protected boolean removeEldestEntry(Map.Entry<String, Boolean> eldest) { return size() > MAX_IDS; }
    };

    private void prune(long now) {
        long oldest = (now - WINDOW_MS) / SLOT_MS;
        for (Iterator<Long> it = slots.keySet().iterator(); it.hasNext();) {
            long slot = it.next();
            // A slot from the future (the clock was set back) is dropped too.
            if (slot <= oldest || slot > now / SLOT_MS) it.remove();
        }
    }
    /** Counts one encounter; false when this id was counted before or is not usable. */
    public synchronized boolean count(String id, long now) {
        if (id == null || id.isEmpty() || id.length() > 64 || now <= 0) return false;
        if (ids.put(id, Boolean.TRUE) != null) return false;
        prune(now);
        slots.merge(now / SLOT_MS, 1, Integer::sum);
        return true;
    }
    /** Encounters in the 24 hours up to now. */
    public synchronized int total(long now) {
        prune(now);
        long sum = 0;
        for (int n : slots.values()) sum += n;
        return (int) Math.min(Integer.MAX_VALUE, sum);
    }
    /** {@code slot:count,...} for storage; ids are not kept (a restarted game has no opened Pokemon on the map). */
    public synchronized String save(long now) {
        prune(now);
        StringBuilder out = new StringBuilder();
        for (Map.Entry<Long, Integer> e : slots.entrySet()) {
            if (out.length() > 0) out.append(',');
            out.append(e.getKey()).append(':').append(e.getValue());
        }
        return out.toString();
    }
    /** Replaces the counts with a stored value; anything malformed is skipped. */
    public synchronized void load(String stored, long now) {
        slots.clear(); ids.clear();
        if (stored == null || stored.isEmpty() || stored.length() > 8192) return;
        for (String part : stored.split(",")) {
            int colon = part.indexOf(':');
            if (colon <= 0 || slots.size() >= MAX_SLOTS) continue;
            try {
                long slot = Long.parseLong(part.substring(0, colon));
                int n = Integer.parseInt(part.substring(colon + 1));
                if (slot > 0 && n > 0 && n <= 1000000) slots.merge(slot, n, Integer::sum);
            } catch (NumberFormatException ignored) {}
        }
        prune(now);
    }
}
