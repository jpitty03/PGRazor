package dev.pgrazor;

/** Transient map-catch feedback. A lost terminal event cannot leave a permanent busy badge. */
final class MapCatchStatus {
    private String label = "";
    private long expires;
    synchronized void clear() { label = ""; expires = 0; }
    synchronized void event(String phase, int result, long now) {
        switch (phase) {
            case "tap": case "encounter": case "berry": case "berry-result": case "throw":
                label = "CATCH"; expires = now + 20000; break;
            case "catch-result":
                if (result == 1) finish("CAUGHT", now);
                else if (result == 3) finish("FLED", now);
                else if (result == 2 || result == 4) { label = "RETRY"; expires = now + 20000; }
                else finish("STOPPED", now);
                break;
            case "caught": finish("CAUGHT", now); break;
            case "fled": finish("FLED", now); break;
            case "stopped": case "gone": case "not-caught": finish("STOPPED", now); break;
            default: break; // Readiness, fallback and duplicate taps cannot start or prolong a catch.
        }
    }
    private void finish(String value, long now) { label = value; expires = now + 1500; }
    synchronized String label(long now) { return now < expires ? label : ""; }
}
