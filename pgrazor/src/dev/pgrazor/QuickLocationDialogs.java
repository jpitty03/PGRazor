package dev.pgrazor;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.text.InputType;
import android.view.ContextThemeWrapper;
import android.widget.EditText;
import android.widget.LinearLayout;
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
        EditText coordinates = input(ctx, "Latitude, longitude");
        form.addView(coordinates);
        AlertDialog dialog = new AlertDialog.Builder(ctx).setTitle("Teleport to coordinates").setView(form)
            .setNegativeButton("Cancel", null).setPositiveButton("Teleport", null).create();
        dialog.setOnShowListener(d -> dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v -> {
            double[] point;
            try { point=CoordinateInput.parse(coordinates.getText().toString()); }
            catch (IllegalArgumentException e) { coordinates.setError(e.getMessage()); return; }
            TeleportDialog.confirm(activity,point[0],point[1],(lat,lon)->{
                if(!target.teleport(lat,lon))return false;
                dialog.dismiss();return true;
            });
        }));
        show(activity,dialog);
    }

    /** Lists saved places; tapping one opens the cooldown confirmation. */
    public static void showFavorites(Activity activity, TeleportTarget target) {
        if (activity.isFinishing() || activity.isDestroyed() || !activity.hasWindowFocus()) return;
        new Thread(() -> {
            SavedPlaces.Snapshot snapshot;
            try { snapshot = new SavedPlaces(activity).read(); } catch (RuntimeException e) { snapshot = null; }
            final SavedPlaces.Snapshot snap = snapshot;
            activity.runOnUiThread(() -> {
                if (activity.isFinishing() || activity.isDestroyed() || !activity.hasWindowFocus()) return;
                AlertDialog.Builder b = new AlertDialog.Builder(themed(activity)).setTitle("Favorites").setNegativeButton("Close", null);
                if (snap == null) { b.setMessage("Saved places could not be read."); show(activity,b.create()); return; }
                if (snap.entries.isEmpty()) {
                    b.setMessage(snap.damaged ? "Saved places could not be read." : "No saved places yet. Add places from the World map.");
                    show(activity,b.create()); return;
                }
                String[] names = new String[snap.entries.size()];
                for (int i = 0; i < names.length; i++) {
                    SavedPlaces.Entry e = snap.entries.get(i);
                    names[i] = String.format(Locale.US, "%s\n%.6f, %.6f", e.name, e.latitude, e.longitude);
                }
                b.setItems(names, (d, which) -> TeleportDialog.confirm(activity,snap.entries.get(which).latitude,snap.entries.get(which).longitude,target::teleport));
                show(activity,b.create());
                if (snap.damaged) Toast.makeText(activity, "Some saved places could not be read. Valid entries are shown.", Toast.LENGTH_LONG).show();
            });
        }, "pgrazor-favorites-read").start();
    }

    private static void show(Activity activity,AlertDialog dialog){
        DialogLifecycle.bind(activity,dialog,null);dialog.show();
    }

    private static EditText input(Context ctx, String hint) {
        EditText e = new EditText(ctx); e.setSingleLine(true); e.setHint(hint);
        e.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        return e;
    }
    private static Context themed(Activity a) {
        return new ContextThemeWrapper(a, Control.darkTheme() ? android.R.style.Theme_Material_Dialog_Alert : android.R.style.Theme_Material_Light_Dialog_Alert);
    }
    private static int dp(Context c, float x) { return Math.round(x * c.getResources().getDisplayMetrics().density); }
}
