package dev.pgrazor;

import android.os.SystemClock;
import java.util.function.Consumer;
import java.util.function.DoubleSupplier;

/**
 * Owns the Shiny Scanner runtime-only state: the native status text and the latest found-shiny
 * result (encounter id, species, CP and where it was seen). Extracted from Control so this state
 * has a single owner with explicit injected dependencies, mirroring {@link VgpController} and
 * {@link CatchOutcomeController} rather than growing Control further.
 *
 * <p>The persistent enable preference and the ready flag stay as Control fields at the bridge
 * boundary, alongside the other feature readiness gates. The found result is runtime only (never
 * persisted): it expires after {@link #RESULT_TTL_MS} or once the player has moved more than
 * {@link #RESULT_FAR_METERS} from where it was seen, and is dropped outright whenever the scanner
 * is disabled or settings reload. Controller state is read by the UI only through the accessors
 * Control exposes; the controller never resolves names or touches Android views itself.
 *
 * <p>Locking: {@link #lock} serializes the compound result read/write so a display never sees a
 * half-updated sighting. Status is a plain volatile. The current-location suppliers and the event
 * sink are invoked with no lock held.
 */
final class ShinyScannerController {
    /** A found shiny is retained for ten minutes before it is considered stale. */
    static final long RESULT_TTL_MS = 10 * 60 * 1000L;
    /** A found shiny is dropped once the player walks/teleports this far from where it was seen. */
    static final double RESULT_FAR_METERS = 300;

    /** Serializes the compound found-result read/write; never held across the injected suppliers. */
    private final Object lock = new Object();
    private volatile String status = "Waiting for scanner support";
    private String encounterId = "";
    private int species, cp;
    private double latitude = Double.NaN, longitude = Double.NaN;
    private long foundAt;

    private final DoubleSupplier currentLatitude, currentLongitude;
    private final Consumer<String> event;

    ShinyScannerController(DoubleSupplier currentLatitude, DoubleSupplier currentLongitude, Consumer<String> event) {
        this.currentLatitude = currentLatitude; this.currentLongitude = currentLongitude; this.event = event;
    }

    void setStatus(String value) { status = value == null ? "" : value.substring(0, Math.min(value.length(), 320)); }
    String status() { return status; }

    /**
     * Records a natively confirmed shiny sighting. Fails closed: a malformed encounter id, species,
     * CP or coordinate is rejected and the previous result is left untouched. A valid sighting
     * replaces any earlier one and is never persisted.
     */
    boolean found(String encounterId, int species, int cp, double latitude, double longitude) {
        if (encounterId == null || encounterId.isEmpty() || encounterId.length() > 128) return false;
        if (species <= 0 || species > 100000 || cp < 0 || cp > 1000000) return false;
        if (!Double.isFinite(latitude) || !Double.isFinite(longitude) || Math.abs(latitude) > 90 || Math.abs(longitude) > 180) return false;
        synchronized (lock) {
            this.encounterId = encounterId; this.species = species; this.cp = cp;
            this.latitude = latitude; this.longitude = longitude; this.foundAt = SystemClock.elapsedRealtime();
        }
        event.accept("species=" + species + " cp=" + cp);
        return true;
    }

    /** Drops the retained result (scanner disabled or settings reload); the status is left to the caller. */
    void clearResult() {
        synchronized (lock) { foundAt = 0; encounterId = ""; species = 0; cp = 0; latitude = longitude = Double.NaN; }
    }

    /** A result is live only inside the TTL window and while the player stays near where it was seen. */
    boolean hasResult() {
        long seenAt; double lat, lon;
        synchronized (lock) { seenAt = foundAt; lat = latitude; lon = longitude; }
        if (seenAt == 0) return false;
        long age = SystemClock.elapsedRealtime() - seenAt;
        if (age < 0 || age > RESULT_TTL_MS) return false;
        double curLat = currentLatitude.getAsDouble(), curLon = currentLongitude.getAsDouble();
        if (Double.isFinite(curLat) && Double.isFinite(curLon) && Double.isFinite(lat) && Double.isFinite(lon)
                && distanceMeters(curLat, curLon, lat, lon) > RESULT_FAR_METERS) return false;
        return true;
    }

    int species() { synchronized (lock) { return species; } }
    int cp() { synchronized (lock) { return cp; } }

    private static double distanceMeters(double lat1, double lon1, double lat2, double lon2) {
        double r = 6371000, dLat = Math.toRadians(lat2 - lat1), dLon = Math.toRadians(lon2 - lon1);
        double a = Math.sin(dLat / 2) * Math.sin(dLat / 2)
                + Math.cos(Math.toRadians(lat1)) * Math.cos(Math.toRadians(lat2)) * Math.sin(dLon / 2) * Math.sin(dLon / 2);
        return 2 * r * Math.asin(Math.min(1, Math.sqrt(a)));
    }
}
