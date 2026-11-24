package dev.pgrazor;

import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.text.ParseException;
import java.text.SimpleDateFormat;
import java.util.Locale;
import java.util.TimeZone;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * Helpers for "spots": 100% IV spawns a feed lists without an encounter id. Plain Java (no Android or
 * org.json), so the device-free tests can check them.
 */
final class FeedSpots {
    private FeedSpots() {}
    private static final Pattern COORDS = Pattern.compile("(-?\\d{1,2}\\.\\d{3,}),\\s*(-?\\d{1,3}\\.\\d{3,})");

    /**
     * Same key as the server feed's spot_key(): "s" + the first 16 hex digits of SHA-1 over
     * "dex:lat:lon:despawn minute" (coordinates to 5 decimals, about a metre).
     */
    static String key(int dex, double lat, double lon, long despawnMs) {
        String text = String.format(Locale.US, "%d:%.5f:%.5f:%d", dex, lat, lon, despawnMs / 60_000);
        try {
            byte[] h = MessageDigest.getInstance("SHA-1").digest(text.getBytes(StandardCharsets.US_ASCII));
            StringBuilder b = new StringBuilder("s");
            for (int i = 0; i < 8; i++) b.append(String.format(Locale.US, "%02x", h[i] & 0xff));
            return b.toString();
        } catch (java.security.NoSuchAlgorithmException e) { throw new IllegalStateException(e); }
    }

    static boolean isKey(String key) { return key != null && key.matches("s[0-9a-f]{16}"); }

    /** The first "lat,lon" in text (HTML allowed), or null when there is none or it is out of range. */
    static double[] coords(String text) {
        if (text == null) return null;
        Matcher m = COORDS.matcher(text);
        if (!m.find()) return null;
        double lat = Double.parseDouble(m.group(1)), lon = Double.parseDouble(m.group(2));
        return lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180 && !(lat == 0 && lon == 0) ? new double[]{lat, lon} : null;
    }

    /** "yyyy-MM-dd HH:mm:ss" read as if it were UTC, in ms, or Long.MIN_VALUE. */
    static long naiveUtc(String text) {
        if (text == null || !text.matches("\\d{4}-\\d{2}-\\d{2} \\d{2}:\\d{2}:\\d{2}")) return Long.MIN_VALUE;
        SimpleDateFormat f = new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US);
        f.setTimeZone(TimeZone.getTimeZone("UTC")); f.setLenient(false);
        try { return f.parse(text).getTime(); } catch (ParseException e) { return Long.MIN_VALUE; }
    }

    /**
     * A list's clock minus UTC, in whole hours, for lists that give local times with no zone. Its newest
     * spawn started moments ago, so that start read as UTC is ahead of now by the offset. Null when that
     * does not give a believable offset (the newest start in the future, or over an hour old).
     */
    static Long offsetMs(long newestStartAsUtc, long now) {
        if (newestStartAsUtc == Long.MIN_VALUE) return null;
        long hours = Math.round((newestStartAsUtc - now) / 3_600_000.0);
        if (hours < -12 || hours > 14) return null;
        long age = now - (newestStartAsUtc - hours * 3_600_000);
        return age >= -120_000 && age <= 3_600_000 ? hours * 3_600_000 : null;
    }
}
