package dev.pgrazor;

import java.text.SimpleDateFormat;
import java.util.Arrays;
import java.util.Date;
import java.util.HashSet;
import java.util.Locale;
import java.util.Set;
import java.util.TimeZone;

/** UI-independent local clock. Geographic boundaries are offline; DST rules come from Android. */
final class LocalTimeClock {
    private static final Set<String> AVAILABLE = new HashSet<>(Arrays.asList(TimeZone.getAvailableIDs()));
    private String zoneId;
    private Locale locale;
    private boolean use24;
    private long minute = Long.MIN_VALUE;
    private SimpleDateFormat formatter;
    private String text = "--:--";

    String label(double latitude, double longitude, long now, boolean twentyFourHour, Locale language) {
        final String zone;
        try { zone = TimeZoneLookup.zoneId(latitude, longitude); }
        catch (IllegalArgumentException invalid) { return "--:--"; }
        // TimeZone silently returns GMT for unknown IDs. Never show a convincing wrong clock.
        if (!AVAILABLE.contains(zone)) return "--:--";
        if (!zone.equals(zoneId) || !language.equals(locale) || use24 != twentyFourHour) {
            zoneId = zone; locale = language; use24 = twentyFourHour;
            formatter = new SimpleDateFormat(use24 ? "HH:mm z" : "h:mm a z", locale);
            formatter.setTimeZone(TimeZone.getTimeZone(zone));
            minute = Long.MIN_VALUE;
        }
        long currentMinute = Math.floorDiv(now, 60000L);
        if (currentMinute != minute) {
            text = formatter.format(new Date(now));
            minute = currentMinute;
        }
        return text;
    }
}
