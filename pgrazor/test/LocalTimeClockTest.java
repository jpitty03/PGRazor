package dev.pgrazor;

import java.nio.file.Files;
import java.nio.file.Paths;
import java.time.Instant;
import java.util.Locale;
import java.util.TimeZone;

public final class LocalTimeClockTest {
    private static void equal(Object expected, Object actual) {
        if (!expected.equals(actual)) throw new AssertionError(expected + " != " + actual);
    }
    private static void starts(String prefix, String actual) {
        if (!actual.startsWith(prefix)) throw new AssertionError(prefix + " != " + actual);
    }
    private static long at(String iso) { return Instant.parse(iso).toEpochMilli(); }

    public static void main(String[] args) throws Exception {
        equal("America/Los_Angeles", TimeZoneLookup.zoneId(37.7749, -122.4194));
        equal("America/New_York", TimeZoneLookup.zoneId(40.7128, -74.0060));
        equal("Asia/Tokyo", TimeZoneLookup.zoneId(35.6762, 139.6503));
        equal("Asia/Kolkata", TimeZoneLookup.zoneId(28.6139, 77.2090));
        equal("Asia/Kathmandu", TimeZoneLookup.zoneId(27.7172, 85.3240));
        equal("Pacific/Auckland", TimeZoneLookup.zoneId(-36.8485, 174.7633));
        equal("Europe/Madrid", TimeZoneLookup.zoneId(41.6488, -0.8891));
        for (double[] point : new double[][]{{Double.NaN,0},{0,Double.POSITIVE_INFINITY},{91,0},{0,-181}}) {
            try { TimeZoneLookup.zoneId(point[0],point[1]); throw new AssertionError("Accepted invalid coordinate"); }
            catch (IllegalArgumentException expected) { }
        }
        LocalTimeClock clock = new LocalTimeClock();
        TimeZone.setDefault(TimeZone.getTimeZone("Pacific/Honolulu"));
        long now = at("2026-11-10T16:30:00Z");
        starts("08:30 ", clock.label(37.7749,-122.4194,now,true,Locale.US));
        // Teleport within the same minute must invalidate the old zone and formatted label.
        starts("01:30 ", clock.label(35.6762,139.6503,now,true,Locale.US));
        starts("1:30 AM ", clock.label(35.6762,139.6503,now,false,Locale.US));
        starts("22:00 ", clock.label(28.6139,77.2090,now,true,Locale.US));
        starts("22:15 ", clock.label(27.7172,85.3240,now,true,Locale.US));
        starts("01:59 ", clock.label(40.7128,-74.0060,at("2026-03-08T06:59:00Z"),true,Locale.US));
        starts("03:00 ", clock.label(40.7128,-74.0060,at("2026-03-08T07:00:00Z"),true,Locale.US));
        starts("01:59 ", clock.label(40.7128,-74.0060,at("2026-11-01T05:59:00Z"),true,Locale.US));
        starts("01:00 ", clock.label(40.7128,-74.0060,at("2026-11-01T06:00:00Z"),true,Locale.US));
        starts("23:00 ", clock.label(-33.8688,151.2093,at("2026-01-01T12:00:00Z"),true,Locale.US));
        starts("22:00 ", clock.label(-33.8688,151.2093,at("2026-07-01T12:00:00Z"),true,Locale.US));
        equal("--:--", clock.label(Double.NaN,0,now,true,Locale.US));
        starts("01:30 ", clock.label(35.6762,139.6503,now,true,Locale.US));
        // Clock corrections backwards should also replace the cached minute.
        starts("01:29 ", clock.label(35.6762,139.6503,now-60000,true,Locale.US));
        if (args.length > 0) {
            for (String line : Files.readAllLines(Paths.get(args[0]))) {
                String[] parts = line.split("\t");
                equal(parts[2], TimeZoneLookup.zoneId(Double.parseDouble(parts[0]),Double.parseDouble(parts[1])));
            }
        }
        System.out.println("PASS: offline lookup, teleport, fractional offsets, DST, device-zone independence, format and clock changes");
    }
}
