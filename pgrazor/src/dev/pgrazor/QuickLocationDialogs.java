package dev.pgrazor;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.text.InputType;
import android.view.ContextThemeWrapper;
import android.view.View;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Toast;
import java.util.Locale;

/**
 * Quick-teleport modals for the {@link QuickMenu}. Both delegate the actual move to a
 * {@link TeleportTarget} (the host validates readiness and calls {@link Control#teleport}),
 * and reuse the shared {@link SavedPlaces} store. Persistence reads run off the UI thread.
 */
public final class QuickLocationDialogs {
    private QuickLocationDialogs() {}

    /** Applies a destination; returns false when it was rejected so the modal can stay open. */
    public interface TeleportTarget { boolean teleport(double latitude, double longitude); }

    /** A simple latitude/longitude teleport modal (no speed field). */
    public static void showTeleport(Activity activity, TeleportTarget target) {
        if (activity.isFinishing() || activity.isDestroyed() || !activity.hasWindowFocus()) return;
        Context ctx = themed(activity);
        LinearLayout form = new LinearLayout(ctx); form.setOrientation(LinearLayout.VERTICAL);
        int p = dp(ctx, 22); form.setPadding(p, dp(ctx, 8), p, dp(ctx, 8));
        EditText coords = coordinateInput(ctx, "");
        form.addView(coords); form.addView(coordinateHint(ctx));
        AlertDialog dialog = new AlertDialog.Builder(ctx).setTitle("Teleport to coordinates").setView(form)
            .setNegativeButton("Cancel", null).setPositiveButton("Teleport", null).create();
        dialog.setOnShowListener(d -> dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v -> {
            double[] ll = parseLatLon(coords.getText().toString());
            String problem = latLonProblem(ll);
            if (problem != null) { coords.setError(problem); return; }
            TeleportDialog.confirm(activity,ll[0],ll[1],(lat,lon)->{
                if(!target.teleport(lat,lon))return false;
                dialog.dismiss();return true;
            });
        }));
        show(activity,dialog);
    }

    /** Lists saved places; tapping one opens the cooldown confirmation. */
    public static void showFavorites(Activity activity, TeleportTarget target) {
        if (activity.isFinishing() || activity.isDestroyed() || !activity.hasWindowFocus()) return;
        Context ctx = themed(activity);
        new Thread(() -> {
            SavedPlaces.Snapshot snapshot;
            try { snapshot = new SavedPlaces(activity).read(); } catch (RuntimeException e) { snapshot = null; }
            final SavedPlaces.Snapshot snap = snapshot;
            activity.runOnUiThread(() -> {
                if (activity.isFinishing() || activity.isDestroyed() || !activity.hasWindowFocus()) return;

                LinearLayout layout = new LinearLayout(ctx);
                layout.setOrientation(LinearLayout.VERTICAL);
                int p = dp(ctx, 12);
                layout.setPadding(p, p, p, p);

                AlertDialog.Builder b = new AlertDialog.Builder(ctx).setTitle("Favorites").setNegativeButton("Close", null);
                if (snap == null) {
                    b.setMessage("Saved places could not be read.");
                    show(activity, b.create());
                    return;
                }
                if (snap.entries.isEmpty()) {
                    b.setMessage(snap.damaged ? "Saved places could not be read." : "No saved places yet. Add places from the World map.");
                    show(activity, b.create());
                    return;
                }

                ScrollView scroll = new ScrollView(ctx);
                LinearLayout list = new LinearLayout(ctx);
                list.setOrientation(LinearLayout.VERTICAL);
                scroll.addView(list, new ScrollView.LayoutParams(-1, -2));
                layout.addView(scroll, new LinearLayout.LayoutParams(-1, -2));

                b.setView(layout);
                AlertDialog dialog = b.create();

                for (int i = 0; i < snap.entries.size(); i++) {
                    final SavedPlaces.Entry e = snap.entries.get(i);
                    Button btn = new Button(ctx);
                    btn.setText(String.format(Locale.US, "%s\n%.6f, %.6f", e.name, e.latitude, e.longitude));
                    btn.setAllCaps(false);
                    btn.setOnClickListener(v -> {
                        TeleportDialog.confirm(activity,e.latitude,e.longitude,(lat,lon)->{if(!target.teleport(lat,lon))return false;dialog.dismiss();return true;});
                    });
                    list.addView(btn, new LinearLayout.LayoutParams(-1, -2));
                }

                show(activity, dialog);
                if (snap.damaged) Toast.makeText(activity, "Some saved places could not be read. Valid entries are shown.", Toast.LENGTH_LONG).show();
            });
        }, "pgrazor-favorites-read").start();
    }

    private static void show(Activity activity,AlertDialog dialog){
        DialogLifecycle.bind(activity,dialog,null);dialog.show();
        View custom = dialog.findViewById(android.R.id.custom); if (custom != null) MapTheme.style(custom);
        MapTheme.styleButtons(dialog);
    }

    /**
     * Parses "latitude, longitude" typed or pasted on one line. Accepts the Google Maps form
     * "4.710989, -74.072092", space or semicolon separators, surrounding brackets/degree signs,
     * and decimal commas when the two values are separated by a space or semicolon ("4,71 -74,07").
     * Returns {lat, lon}, or null when it is not exactly two numbers. Range is checked separately.
     */
    static double[] parseLatLon(String raw) {
        if (raw == null) return null;
        String s = raw.trim().replaceAll("[()\\[\\]°]", "").trim();
        if (s.isEmpty()) return null;
        String[] parts;
        if (s.contains(";")) parts = s.split("\\s*;\\s*");
        else if (s.indexOf(',') >= 0 && s.indexOf(',') == s.lastIndexOf(',')) parts = s.split("\\s*,\\s*");
        else parts = s.split("\\s+");
        if (parts.length != 2) return null;
        double[] out = new double[2];
        for (int i = 0; i < 2; i++) {
            String p = parts[i].replaceAll(",$", "").replace(',', '.');
            try { out[i] = Double.parseDouble(p); } catch (NumberFormatException e) { return null; }
            if (Double.isNaN(out[i]) || Double.isInfinite(out[i])) return null;
        }
        return out;
    }
    /** A user-facing error for a parsed pair, or null when it is a valid position. */
    static String latLonProblem(double[] ll) {
        if (ll == null) return "Enter latitude, longitude (e.g. 4.710989, -74.072092)";
        if (!(ll[0] >= -90 && ll[0] <= 90)) return "Latitude must be between -90 and 90";
        if (!(ll[1] >= -180 && ll[1] <= 180)) return "Longitude must be between -180 and 180";
        return null;
    }
    /** One-line "lat, lon" field. Text input so comma and space are always on the keyboard. */
    static EditText coordinateInput(Context ctx, String value) {
        EditText e = new EditText(ctx); e.setSingleLine(true); e.setHint("Latitude, longitude"); e.setText(value);
        // Visible-password variation keeps the number row up and stops autocorrect from mangling digits.
        e.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        e.setSelectAllOnFocus(true);
        return e;
    }
    /** Small muted helper line under the coordinate field. */
    static android.widget.TextView coordinateHint(Context ctx) {
        android.widget.TextView t = new android.widget.TextView(ctx);
        t.setText("Paste from Google Maps, e.g. 4.710989, -74.072092");
        t.setTextSize(12); t.setTextColor(MapTheme.muted()); t.setPadding(dp(ctx, 4), dp(ctx, 2), dp(ctx, 4), dp(ctx, 10));
        return t;
    }
    private static Context themed(Activity a) {
        return MapTheme.wrap(a);
    }
    private static int dp(Context c, float x) { return Math.round(x * c.getResources().getDisplayMetrics().density); }
}
