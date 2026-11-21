package dev.pgrazor;

import java.util.Locale;
import java.util.TreeMap;

/**
 * Rolling history of the player's lifetime counters, for one account. Pure Java. One sample is kept per
 * five-minute slot for 25 hours, plus the newest; the change over a window is the newest value minus the
 * value at the window's start, read between the two samples around it. When the history is shorter than
 * the window, the change since the oldest sample is reported with the time it covers.
 *
 * <p>Stardust has no lifetime counter, only a balance that also drops when it is spent. Each rise of the
 * balance between two readings is added to an earned total and each fall to a spent total; those totals
 * are kept like the other counters.
 */
public final class SessionStatsHistory {
    /** Counters in a reading and in a change. DUST is stardust earned, DUST_SPENT stardust spent. */
    public static final int XP = 0, CAUGHT = 1, SEEN = 2, STOPS = 3, KM = 4, HATCHED = 5, DUST = 6, DUST_SPENT = 7;
    /** A reading: the six game counters, then the stardust balance (-1 when it could not be read). */
    public static final int READ_BALANCE = 6, READ_COUNT = 7;
    static final int BALANCE = 8, COUNT = 9, GAME_COUNTERS = 6;
    public static final long HOUR_MS = 3600_000L, DAY_MS = 24 * HOUR_MS;
    static final long SLOT_MS = 5 * 60_000L, KEEP_MS = DAY_MS + HOUR_MS;
    private static final int MAX_SAMPLES = 400;
    private final TreeMap<Long, double[]> samples = new TreeMap<>();
    private long latestAt;
    private double[] latest;

    /** What changed over a window: the per-counter change and how much of the window the history covers. */
    public static final class Change {
        public final double[] values;
        public final long coveredMs, updatedAt;
        Change(double[] values, long coveredMs, long updatedAt) { this.values = values; this.coveredMs = coveredMs; this.updatedAt = updatedAt; }
        public double get(int index) { return values[index]; }
    }

    private static boolean finite(double x) { return !Double.isNaN(x) && !Double.isInfinite(x); }
    /** Records a reading taken at {@code now}. A game counter that went down means a reset or another
     *  account, so the history starts again. Returns false for an unusable reading. */
    public synchronized boolean record(long now, double[] reading) {
        if (now <= 0 || reading == null || reading.length != READ_COUNT || !(reading[XP] > 0)) return false;
        for (int i = 0; i < READ_COUNT; i++) if (!finite(reading[i]) || reading[i] < (i == READ_BALANCE ? -1 : 0)) return false;
        double[] v = new double[COUNT];
        System.arraycopy(reading, 0, v, 0, GAME_COUNTERS);
        double balance = reading[READ_BALANCE] >= 0 ? reading[READ_BALANCE] : -1;
        double[] before = continues(v, now) ? latest : null;
        if (before != null) {
            v[DUST] = before[DUST]; v[DUST_SPENT] = before[DUST_SPENT];
            if (balance >= 0 && before[BALANCE] >= 0) {
                double d = balance - before[BALANCE];
                if (d > 0) v[DUST] += d; else v[DUST_SPENT] -= d;
            }
            if (balance < 0) balance = before[BALANCE];
        }
        v[BALANCE] = balance;
        put(now, v);
        return true;
    }
    // Whether a sample carries on from the newest one; when not, the history is cleared first.
    private boolean continues(double[] v, long now) {
        if (latest == null) return false;
        boolean ok = now >= latestAt;  // the clock went back otherwise
        // Km is stored rounded, so it may read a hair lower than the saved value.
        for (int i = 0; ok && i < GAME_COUNTERS; i++) if (v[i] < latest[i] - (i == KM ? 0.01 : 0.5)) ok = false;
        if (!ok) { samples.clear(); latest = null; }
        return ok;
    }
    private void put(long now, double[] v) {
        long slot = now / SLOT_MS;
        if (!samples.containsKey(slot)) {
            double[] s = new double[COUNT + 1];
            s[0] = now;
            System.arraycopy(v, 0, s, 1, COUNT);
            samples.put(slot, s);
        }
        latest = v; latestAt = now;
        prune(now);
    }
    private void prune(long now) {
        long oldest = (now - KEEP_MS) / SLOT_MS;
        while (!samples.isEmpty() && (samples.firstKey() < oldest || samples.size() > MAX_SAMPLES)) samples.pollFirstEntry();
    }
    /** The change over the {@code windowMs} up to the newest reading, or null with no reading yet. */
    public synchronized Change change(long windowMs) {
        if (latest == null || samples.isEmpty()) return null;
        long start = latestAt - windowMs;
        double[] before = null, after = null;
        for (double[] s : samples.values()) {
            if ((long) s[0] <= start) before = s;
            else { after = s; break; }
        }
        double[] base = new double[COUNT];
        long covered;
        if (before == null) {  // history shorter than the window
            double[] first = samples.firstEntry().getValue();
            System.arraycopy(first, 1, base, 0, COUNT);
            covered = latestAt - (long) first[0];
        } else if (after == null) {
            System.arraycopy(before, 1, base, 0, COUNT);
            covered = windowMs;
        } else {
            double span = after[0] - before[0], f = span <= 0 ? 0 : (start - before[0]) / span;
            for (int i = 0; i < COUNT; i++) base[i] = before[i + 1] + (after[i + 1] - before[i + 1]) * f;
            covered = windowMs;
        }
        double[] out = new double[DUST_SPENT + 1];
        for (int i = 0; i < out.length; i++) out[i] = Math.max(0, latest[i] - base[i]);
        return new Change(out, Math.max(0, covered), latestAt);
    }
    /** {@code at:v0;v1;...|...} for storage, oldest first, with the newest reading last. */
    public synchronized String save() {
        StringBuilder out = new StringBuilder();
        for (double[] s : samples.values()) append(out, (long) s[0], s, 1);
        if (latest != null) append(out, latestAt, latest, 0);
        return out.toString();
    }
    private static void append(StringBuilder out, long at, double[] v, int from) {
        if (out.length() > 0) out.append('|');
        out.append(at).append(':');
        for (int i = 0; i < COUNT; i++) {
            if (i > 0) out.append(';');
            double x = v[from + i];
            out.append(x == Math.rint(x) ? Long.toString((long) x) : String.format(Locale.US, "%.3f", x));
        }
    }
    /** Replaces the history with a stored value; anything malformed is skipped. Entries saved before
     *  stardust was tracked load with no stardust history. */
    public synchronized void load(String stored, long now) {
        samples.clear(); latest = null; latestAt = 0;
        if (stored == null || stored.isEmpty() || stored.length() > 80_000) return;
        for (String part : stored.split("\\|")) {
            int colon = part.indexOf(':');
            if (colon <= 0) continue;
            try {
                long at = Long.parseLong(part.substring(0, colon));
                String[] fields = part.substring(colon + 1).split(";");
                if ((fields.length != COUNT && fields.length != GAME_COUNTERS) || at <= 0 || at > now) continue;
                double[] v = new double[COUNT];
                v[BALANCE] = -1;
                for (int i = 0; i < fields.length; i++) v[i] = Double.parseDouble(fields[i]);
                boolean ok = v[XP] > 0;
                for (int i = 0; i < COUNT; i++) ok &= finite(v[i]) && v[i] >= (i == BALANCE ? -1 : 0);
                if (!ok) continue;
                if (latest != null && !continues(v, at)) { /* history cleared; this sample starts it again */ }
                put(at, v);
            } catch (NumberFormatException ignored) {}
        }
        if (latest != null) prune(now);
        if (samples.isEmpty()) { latest = null; latestAt = 0; }
    }
    synchronized int size() { return samples.size(); }
}
