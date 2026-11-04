package dev.pgrazor;

import android.app.Activity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.widget.Toast;
import java.util.Locale;

/** Copies coordinates as "lat, lon" at 6 decimals, the precision the map and Teleport show. */
final class CoordinateClipboard {
    private CoordinateClipboard() {}

    static String text(double latitude, double longitude) {
        return String.format(Locale.US, "%.6f, %.6f", latitude, longitude);
    }

    /** Copies and confirms with a short toast naming what was copied ({@code done}). */
    static void copy(Activity activity, double latitude, double longitude, String done) {
        if (!Double.isFinite(latitude) || !Double.isFinite(longitude)) return;
        ClipboardManager clipboard = (ClipboardManager) activity.getSystemService(Context.CLIPBOARD_SERVICE);
        if (clipboard == null) return;
        String text = text(latitude, longitude);
        clipboard.setPrimaryClip(ClipData.newPlainText("Coordinates", text));
        Toast.makeText(activity, done + ": " + text, Toast.LENGTH_SHORT).show();
    }
}
