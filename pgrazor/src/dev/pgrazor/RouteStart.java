package dev.pgrazor;

/**
 * Pure route-start helpers for resuming a GPX route part-way: the route point closest to a position,
 * great-circle distances, and the route reordered to begin at a chosen point.
 */
final class RouteStart {
    private RouteStart() {}
    private static final double EARTH_RADIUS_METERS = 6_371_008.8;

    /** Great-circle (haversine) distance in meters. */
    static double meters(double lat1, double lon1, double lat2, double lon2) {
        double p1 = Math.toRadians(lat1), p2 = Math.toRadians(lat2);
        double dp = p2 - p1, dl = Math.toRadians(lon2 - lon1);
        double h = Math.sin(dp / 2) * Math.sin(dp / 2) + Math.cos(p1) * Math.cos(p2) * Math.sin(dl / 2) * Math.sin(dl / 2);
        return 2 * EARTH_RADIUS_METERS * Math.asin(Math.min(1, Math.sqrt(h)));
    }

    /** Index of the route point closest to the position; the earliest wins a tie. */
    static int closest(double[][] points, double latitude, double longitude) {
        if (points == null || points.length == 0) throw new IllegalArgumentException("The route has no stops.");
        int best = 0; double bestMeters = Double.MAX_VALUE;
        for (int i = 0; i < points.length; i++) {
            double d = meters(latitude, longitude, points[i][0], points[i][1]);
            if (d < bestMeters) { bestMeters = d; best = i; }
        }
        return best;
    }

    /**
     * The route walked from {@code start}: the remaining stops for a one-way route, or the whole loop
     * beginning at {@code start} for a looping route.
     */
    static double[][] from(double[][] points, int start, boolean loop) {
        if (points == null || start < 0 || start >= points.length) throw new IllegalArgumentException("Choose a stop on the route.");
        int count = loop ? points.length : points.length - start;
        double[][] out = new double[count][];
        for (int i = 0; i < count; i++) out[i] = points[(start + i) % points.length].clone();
        return out;
    }

    /** "85 m" or "1.2 km". */
    static String distance(double meters) {
        if (meters < 1000) return Math.round(meters) + " m";
        return String.format(java.util.Locale.US, "%.1f km", meters / 1000);
    }
}
