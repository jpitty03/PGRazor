package dev.pgrazor;

import java.util.ArrayList;
import java.util.List;

/**
 * Deterministic nearest-neighbour route planning over loaded map POIs.
 *
 * Pure coordinate logic with no Android or JSON dependency, so it unit-tests in plain
 * Java and is reusable by {@link AutoWalkDialog}. The dialog converts the game's POI
 * snapshot into plain latitude/longitude pairs and hands them here; this class only ever
 * reads coordinates and never moves the avatar.
 */
public final class AutoWalkPlanner {
    /** Safety ceiling shared with the route engine; the UI maximum is the loaded POI count. */
    public static final int MAX_STOPS = MotionEngine.MAX_ROUTE_STOPS;
    private static final double EARTH_RADIUS_METERS = 6371008.8;
    /** POIs within this many metres of one already kept are treated as the same stop. */
    static final double DEDUPE_METERS = 1.0;

    private AutoWalkPlanner() {}

    /** A plan plus the counts needed to explain honestly how many stops were really loaded. */
    public static final class Plan {
        /** Ordered [lat,lon] waypoints in visit order; length is 0..min(requested, available). */
        public final double[][] route;
        /** Stops the user asked for (already validated to 1..MAX_STOPS). */
        public final int requested;
        /** Unique valid POIs that were actually loaded. */
        public final int available;
        Plan(double[][] route, int requested, int available) {
            this.route = route; this.requested = requested; this.available = available;
        }
        public int selected() { return route.length; }
        public boolean empty() { return route.length == 0; }
        /** True when fewer stops were loaded than the user asked for; the caller must not pretend otherwise. */
        public boolean fewerThanRequested() { return route.length < requested; }
    }

    static boolean valid(double lat, double lon) {
        return Double.isFinite(lat) && Double.isFinite(lon) && Math.abs(lat) <= 90 && Math.abs(lon) <= 180;
    }

    /** Independent great-circle distance in metres; only its ordering is relied on. */
    static double meters(double lat1, double lon1, double lat2, double lon2) {
        double a = Math.sin(Math.toRadians(lat2 - lat1) / 2), b = Math.sin(Math.toRadians(lon2 - lon1) / 2);
        double h = a * a + Math.cos(Math.toRadians(lat1)) * Math.cos(Math.toRadians(lat2)) * b * b;
        return 2 * EARTH_RADIUS_METERS * Math.asin(Math.sqrt(Math.min(1, Math.max(0, h))));
    }

    /** Finite, in-range, de-duplicated POIs in first-seen order; deterministic for identical input. */
    public static double[][] filter(double[][] points) {
        List<double[]> kept = new ArrayList<>();
        if (points != null) for (double[] p : points) {
            if (p == null || p.length < 2) continue;
            double lat = p[0], lon = p[1];
            if (!valid(lat, lon)) continue;
            boolean duplicate = false;
            for (double[] k : kept) if (meters(k[0], k[1], lat, lon) <= DEDUPE_METERS) { duplicate = true; break; }
            if (!duplicate) kept.add(new double[]{lat, lon});
        }
        return kept.toArray(new double[kept.size()][]);
    }

    /** Convenience overload for callers holding a list of pairs. */
    public static Plan plan(double originLat, double originLon, List<double[]> points, int count) {
        return plan(originLat, originLon, points == null ? null : points.toArray(new double[points.size()][]), count);
    }

    /**
     * Build a nearest-neighbour route of up to {@code count} unique POIs, starting from the avatar.
     * Ties break on first-seen (filtered) order, so identical inputs always yield the same route.
     * The route is never padded to {@code count}: it holds only POIs that were actually loaded.
     */
    public static Plan plan(double originLat, double originLon, double[][] points, int count) {
        if (!valid(originLat, originLon)) throw new IllegalArgumentException("Avatar position is not a valid coordinate.");
        if (count < 1 || count > MAX_STOPS) throw new IllegalArgumentException("Choose between 1 and " + MAX_STOPS + " stops.");
        double[][] pool = filter(points);
        int target = Math.min(count, pool.length);
        double[][] route = new double[target][];
        boolean[] used = new boolean[pool.length];
        double curLat = originLat, curLon = originLon;
        for (int step = 0; step < target; step++) {
            int best = -1;
            double bestDist = Double.POSITIVE_INFINITY;
            for (int i = 0; i < pool.length; i++) {
                if (used[i]) continue;
                double d = meters(curLat, curLon, pool[i][0], pool[i][1]);
                if (d < bestDist) { bestDist = d; best = i; } // strict < keeps the earliest index on ties
            }
            used[best] = true;
            route[step] = new double[]{pool[best][0], pool[best][1]};
            curLat = pool[best][0]; curLon = pool[best][1];
        }
        return new Plan(route, count, pool.length);
    }
}
