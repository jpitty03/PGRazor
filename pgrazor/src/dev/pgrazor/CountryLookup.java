package dev.pgrazor;

import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

/**
 * PGRazorX: offline country of a coordinate, for the cooldown history flags. Borders are Natural Earth 1:50m
 * (public domain) bundled as assets/pgrazor/country-borders.dat: one country per line, its ISO alpha-2 code and rings of
 * lon,lat in 1/100 degree (east of 180 for rings crossing the antimeridian; first point absolute, then deltas; a ring starting with '!' is a hole). A point inside a
 * country's rings (even-odd) belongs to it; a point just off a simplified coast takes the nearest border within
 * about 25 km. No network, no Google services.
 */
public final class CountryLookup {
    private static final int NEAR = 25; // 1/100 degree (~25 km) for points just outside a simplified coastline
    private final List<String> codes = new ArrayList<>();
    private final List<int[][]> rings = new ArrayList<>();
    private final List<int[]> boxes = new ArrayList<>(); // minX, minY, maxX, maxY per country line

    public static CountryLookup read(InputStream in) throws IOException {
        CountryLookup table = new CountryLookup();
        BufferedReader reader = new BufferedReader(new InputStreamReader(in, StandardCharsets.UTF_8));
        for (String line; (line = reader.readLine()) != null; ) table.add(line);
        return table;
    }

    public static CountryLookup parse(String text) {
        CountryLookup table = new CountryLookup();
        for (String line : text.split("\n")) table.add(line);
        return table;
    }

    private void add(String line) {
        line = line.trim();
        if (line.isEmpty() || line.startsWith("#")) return;
        String[] parts = line.split(" ");
        if (parts.length < 2 || !parts[0].matches("[A-Z]{2}")) return;
        int[][] own = new int[parts.length - 1][];
        int[] box = {Integer.MAX_VALUE, Integer.MAX_VALUE, Integer.MIN_VALUE, Integer.MIN_VALUE};
        int count = 0;
        for (int i = 1; i < parts.length; i++) {
            String ring = parts[i].startsWith("!") ? parts[i].substring(1) : parts[i];
            String[] values = ring.split(",");
            if (values.length < 6 || values.length % 2 != 0) continue;
            int[] points = new int[values.length];
            int x = 0, y = 0;
            for (int j = 0; j < values.length; j += 2) {
                int dx = Integer.parseInt(values[j]), dy = Integer.parseInt(values[j + 1]);
                x = j == 0 ? dx : x + dx; y = j == 0 ? dy : y + dy;
                points[j] = x; points[j + 1] = y;
                box[0] = Math.min(box[0], x); box[1] = Math.min(box[1], y); box[2] = Math.max(box[2], x); box[3] = Math.max(box[3], y);
            }
            own[count++] = points;
        }
        if (count == 0) return;
        int[][] kept = new int[count][];
        System.arraycopy(own, 0, kept, 0, count);
        codes.add(parts[0]); rings.add(kept); boxes.add(box);
    }

    public int size() { return codes.size(); }

    /** ISO alpha-2 code, or null when the point is in no country (open sea). */
    public String code(double latitude, double longitude) {
        if (!Double.isFinite(latitude) || !Double.isFinite(longitude) || Math.abs(latitude) > 90 || Math.abs(longitude) > 180) return null;
        double y = latitude * 100;
        // Rings that cross the antimeridian are stored east of 180, so western longitudes are also tried +360.
        double[] xs = longitude < 0 ? new double[]{longitude * 100, longitude * 100 + 36000} : new double[]{longitude * 100};
        for (double x : xs) for (int c = 0; c < codes.size(); c++) {
            int[] box = boxes.get(c);
            if (x < box[0] || x > box[2] || y < box[1] || y > box[3]) continue;
            boolean inside = false;
            for (int[] ring : rings.get(c)) if (contains(ring, x, y)) inside = !inside;
            if (inside) return codes.get(c);
        }
        String nearest = null;
        double best = (double) NEAR * NEAR;
        for (double x : xs) for (int c = 0; c < codes.size(); c++) {
            int[] box = boxes.get(c);
            if (x < box[0] - NEAR || x > box[2] + NEAR || y < box[1] - NEAR || y > box[3] + NEAR) continue;
            for (int[] ring : rings.get(c)) {
                for (int i = 0, n = ring.length; i < n; i += 2) {
                    int j = (i + 2) % n;
                    double d = segment(x, y, ring[i], ring[i + 1], ring[j], ring[j + 1]);
                    if (d < best) { best = d; nearest = codes.get(c); }
                }
            }
        }
        return nearest;
    }

    /** Flag emoji for a coordinate, or "" when it is in no country. */
    public String flag(double latitude, double longitude) {
        String code = code(latitude, longitude);
        return code == null ? "" : flag(code);
    }

    public static String flag(String code) {
        return new String(Character.toChars(0x1F1E6 + code.charAt(0) - 'A')) + new String(Character.toChars(0x1F1E6 + code.charAt(1) - 'A'));
    }

    private static boolean contains(int[] ring, double x, double y) {
        boolean inside = false;
        for (int i = 0, n = ring.length, j = n - 2; i < n; j = i, i += 2) {
            double xi = ring[i], yi = ring[i + 1], xj = ring[j], yj = ring[j + 1];
            if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi) inside = !inside;
        }
        return inside;
    }

    /** Squared distance from (x,y) to the segment a-b. */
    private static double segment(double x, double y, double ax, double ay, double bx, double by) {
        double dx = bx - ax, dy = by - ay, length = dx * dx + dy * dy;
        double t = length == 0 ? 0 : Math.max(0, Math.min(1, ((x - ax) * dx + (y - ay) * dy) / length));
        double px = ax + t * dx - x, py = ay + t * dy - y;
        return px * px + py * py;
    }
}
