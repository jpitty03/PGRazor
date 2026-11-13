package android.os;

/** Monotonic clock shell; tests advance {@code now} to drive the watchdog. */
public class SystemClock {
    public static long now = 1000;
    public static long elapsedRealtime() { return now; }
}
