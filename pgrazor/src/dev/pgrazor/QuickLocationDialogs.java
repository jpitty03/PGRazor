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
 * {@link TeleportTarget} (the host validates readiness and calls {@link Control#teleport}, or
 * {@link Control#walkTo} for the coordinate modal's Walk button),
 * and reuse the shared {@link SavedPlaces} store. Persistence reads run off the UI thread.
 */
public final class QuickLocationDialogs {
    private QuickLocationDialogs() {}

    /** Applies a destination; returns false when it was rejected so the modal can stay open. */
    public interface TeleportTarget { boolean teleport(double latitude, double longitude); }

    /** A simple latitude/longitude modal (no speed field): Teleport there, or Walk there at the current speed. */
    public static void showTeleport(Activity activity, TeleportTarget target, TeleportTarget walk) {
        if (activity.isFinishing() || activity.isDestroyed() || !activity.hasWindowFocus()) return;
        Context ctx = themed(activity);
        LinearLayout form = new LinearLayout(ctx); form.setOrientation(LinearLayout.VERTICAL);
        int p = dp(ctx, 22); form.setPadding(p, dp(ctx, 8), p, dp(ctx, 8));
        // Where you are now, with a button to copy it.
        final double hereLat = Control.engine.latitude, hereLon = Control.engine.longitude;
        LinearLayout here = new LinearLayout(ctx); here.setGravity(android.view.Gravity.CENTER_VERTICAL);
        android.widget.TextView hereText = new android.widget.TextView(ctx);
        hereText.setText("You are at\n" + CoordinateClipboard.text(hereLat, hereLon)); hereText.setTextSize(14);
        here.addView(hereText, new LinearLayout.LayoutParams(0, -2, 1));
        android.widget.Button copy = new android.widget.Button(ctx); copy.setText("Copy"); copy.setAllCaps(false); copy.setTextSize(13);
        copy.setContentDescription("Copy your current coordinates");
        MapTheme.setButtonBackground(copy);
        copy.setOnClickListener(v -> CoordinateClipboard.copy(activity, hereLat, hereLon, "Your location copied"));
        here.addView(copy, new LinearLayout.LayoutParams(dp(ctx, 76), dp(ctx, 48)));
        android.widget.Button favorite = new android.widget.Button(ctx); favorite.setText("Favorite"); favorite.setAllCaps(false); favorite.setTextSize(13);
        favorite.setContentDescription("Save your current location to Favorites");
        MapTheme.setButtonBackground(favorite);
        favorite.setOnClickListener(v -> saveFavorite(activity, hereLat, hereLon));
        LinearLayout.LayoutParams favoriteLp = new LinearLayout.LayoutParams(dp(ctx, 92), dp(ctx, 48)); favoriteLp.leftMargin = dp(ctx, 6);
        here.addView(favorite, favoriteLp);
        LinearLayout.LayoutParams hereLp = new LinearLayout.LayoutParams(-1, -2); hereLp.bottomMargin = dp(ctx, 8);
        form.addView(here, hereLp);
        EditText coordinates = input(ctx, "Latitude, longitude");
        form.addView(coordinates);
        AlertDialog dialog = new AlertDialog.Builder(ctx).setTitle("Go to coordinates").setView(form)
            .setNegativeButton("Cancel", null).setNeutralButton("Walk", null).setPositiveButton("Teleport", null).create();
        dialog.setOnShowListener(d -> {
            dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v -> {
                double[] point;
                try { point=CoordinateInput.parse(coordinates.getText().toString()); }
                catch (IllegalArgumentException e) { coordinates.setError(e.getMessage()); return; }
                TeleportDialog.confirm(activity,point[0],point[1],(lat,lon)->{
                    if(!target.teleport(lat,lon))return false;
                    dialog.dismiss();return true;
                });
            });
            // Walking needs no cooldown check: it starts from where you are at the current speed.
            dialog.getButton(AlertDialog.BUTTON_NEUTRAL).setOnClickListener(v -> {
                double[] point;
                try { point=CoordinateInput.parse(coordinates.getText().toString()); }
                catch (IllegalArgumentException e) { coordinates.setError(e.getMessage()); return; }
                if(walk.teleport(point[0],point[1])){
                    dialog.dismiss();
                    Toast.makeText(activity,"Walking to "+CoordinateClipboard.text(point[0],point[1]),Toast.LENGTH_SHORT).show();
                }
            });
        });
        show(activity,dialog);
    }

    /** Lists favorites; tapping one offers Teleport, Walk here, Rename and Delete. */
    public static void showFavorites(Activity activity, TeleportTarget target, TeleportTarget walk) {
        if (!activity.hasWindowFocus()) return;
        listFavorites(activity, target, walk);
    }
    // Reopened after Rename / Delete / Cancel, when the closing dialog may still hold window focus.
    private static void listFavorites(Activity activity, TeleportTarget target, TeleportTarget walk) {
        if (activity.isFinishing() || activity.isDestroyed()) return;
        new Thread(() -> {
            SavedPlaces.Snapshot snapshot;
            try { snapshot = new SavedPlaces(activity).read(); } catch (RuntimeException e) { snapshot = null; }
            final SavedPlaces.Snapshot snap = snapshot;
            activity.runOnUiThread(() -> {
                if (activity.isFinishing() || activity.isDestroyed()) return;
                AlertDialog.Builder b = new AlertDialog.Builder(themed(activity)).setTitle("Favorites").setNegativeButton("Close", null);
                if (snap == null) { b.setMessage("Favorites could not be read."); show(activity,b.create()); return; }
                if (snap.entries.isEmpty()) {
                    b.setMessage(snap.damaged ? "Favorites could not be read." : "No favorites yet. Add places from the World map or the Teleport dialog.");
                    show(activity,b.create()); return;
                }
                show(activity,ChoiceList.attach(b.create(),favoriteLabels(snap),
                    which -> favoriteActions(activity,snap.entries.get(which),target,walk,null,() -> listFavorites(activity,target,walk))));
                if (snap.damaged) Toast.makeText(activity, "Some favorites could not be read. Valid entries are shown.", Toast.LENGTH_LONG).show();
            });
        }, "pgrazor-favorites-read").start();
    }

    /** The list rows shared by the quick menu and the World map: name, then coordinates. */
    static String[] favoriteLabels(SavedPlaces.Snapshot snap) {
        String[] names = new String[snap.entries.size()];
        for (int i = 0; i < names.length; i++) {
            SavedPlaces.Entry e = snap.entries.get(i);
            names[i] = String.format(Locale.US, "%s\n%.6f, %.6f", e.name, e.latitude, e.longitude);
        }
        return names;
    }

    /**
     * Options for one favorite, the same in the quick menu and on the World map: Show on map (map only,
     * when showOnMap is given), Teleport (through the cooldown confirmation), Walk here (when walk is
     * given), Rename and Delete. Cancel, Rename and Delete go back to the list through reopen.
     */
    static void favoriteActions(Activity activity, SavedPlaces.Entry entry, TeleportTarget teleport, TeleportTarget walk,
                                java.util.function.Consumer<SavedPlaces.Entry> showOnMap, Runnable reopen) {
        if (activity.isFinishing() || activity.isDestroyed()) return;
        java.util.List<String> labels = new java.util.ArrayList<>();
        java.util.List<Runnable> actions = new java.util.ArrayList<>();
        if (showOnMap != null) { labels.add("Show on map\nPut the pin here"); actions.add(() -> showOnMap.accept(entry)); }
        labels.add("Teleport\nCooldown when you arrive: " + TeleportDialog.duration(Cooldown.remainingAt(entry.latitude, entry.longitude)));
        actions.add(() -> TeleportDialog.confirm(activity, entry.latitude, entry.longitude, teleport::teleport));
        if (walk != null) {
            labels.add("Walk here\nAt your current walk speed");
            actions.add(() -> {
                if (walk.teleport(entry.latitude, entry.longitude))
                    Toast.makeText(activity, "Walking to " + entry.name, Toast.LENGTH_SHORT).show();
            });
        }
        labels.add("Rename\nGive this place a new name"); actions.add(() -> renamePlace(activity, entry, reopen));
        labels.add("Delete\nRemove it from your Favorites"); actions.add(() -> deletePlace(activity, entry, reopen));
        AlertDialog dialog = new AlertDialog.Builder(themed(activity)).setTitle(entry.name)
            .setNegativeButton("Cancel", (d, w) -> reopen.run()).create();
        show(activity, ChoiceList.attach(dialog, labels.toArray(new String[0]), which -> actions.get(which).run()));
    }

    private static void renamePlace(Activity activity, SavedPlaces.Entry entry, Runnable reopen) {
        Context ctx = themed(activity);
        EditText name = input(ctx, "Place name (up to " + SavedPlaces.NAME_LIMIT + " characters)");
        name.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_CAP_SENTENCES);
        name.setFilters(new android.text.InputFilter[]{new android.text.InputFilter.LengthFilter(SavedPlaces.NAME_LIMIT)});
        name.setText(entry.name); name.setSelection(name.getText().length());
        LinearLayout form = new LinearLayout(ctx); form.setOrientation(LinearLayout.VERTICAL);
        int p = dp(ctx, 22); form.setPadding(p, dp(ctx, 8), p, dp(ctx, 8)); form.addView(name);
        AlertDialog dialog = new AlertDialog.Builder(ctx).setTitle("Rename place").setView(form)
            .setNegativeButton("Cancel", (d, w) -> reopen.run()).setPositiveButton("Save", null).create();
        dialog.setOnShowListener(d -> dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v -> {
            final String value = name.getText().toString();
            dialog.getButton(AlertDialog.BUTTON_POSITIVE).setEnabled(false);
            new Thread(() -> {
                String error = null;
                try { if (!new SavedPlaces(activity).rename(entry.id, value)) error = "This place no longer exists."; }
                catch (java.io.IOException | RuntimeException e) { error = e.getMessage() == null ? "Could not rename this place." : e.getMessage(); }
                final String failed = error;
                activity.runOnUiThread(() -> {
                    if (!dialog.isShowing()) return;
                    if (failed != null) { dialog.getButton(AlertDialog.BUTTON_POSITIVE).setEnabled(true); name.setError(failed); return; }
                    dialog.dismiss(); reopen.run();
                });
            }, "pgrazor-favorites-rename").start();
        }));
        show(activity, dialog);
    }

    private static void deletePlace(Activity activity, SavedPlaces.Entry entry, Runnable reopen) {
        AlertDialog confirm = new AlertDialog.Builder(themed(activity)).setTitle("Delete favorite?")
            .setMessage(entry.name + " will be removed from your Favorites. The current location stays unchanged.")
            .setNegativeButton("Cancel", (d, w) -> reopen.run()).setPositiveButton("Delete", null).create();
        confirm.setOnShowListener(d -> confirm.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v -> {
            confirm.getButton(AlertDialog.BUTTON_POSITIVE).setEnabled(false);
            new Thread(() -> {
                boolean ok;
                try { new SavedPlaces(activity).remove(entry.id); ok = true; } catch (java.io.IOException | RuntimeException e) { ok = false; }
                final boolean deleted = ok;
                activity.runOnUiThread(() -> {
                    if (!confirm.isShowing()) return;
                    if (!deleted) { confirm.getButton(AlertDialog.BUTTON_POSITIVE).setEnabled(true); Toast.makeText(activity, "Could not delete place. Please retry.", Toast.LENGTH_LONG).show(); return; }
                    confirm.dismiss();
                    Toast.makeText(activity, "Deleted " + entry.name, Toast.LENGTH_SHORT).show();
                    reopen.run();
                });
            }, "pgrazor-favorites-delete").start();
        }));
        show(activity, confirm);
    }

    private static void show(Activity activity,AlertDialog dialog){
        DialogLifecycle.bind(activity,dialog,null);dialog.show();
    }

    /** Names and saves a location as a favorite (the World map's Favorites). */
    private static void saveFavorite(Activity activity, double latitude, double longitude) {
        if (activity.isFinishing() || activity.isDestroyed()) return;
        Context ctx = themed(activity);
        LinearLayout form = new LinearLayout(ctx); form.setOrientation(LinearLayout.VERTICAL);
        int p = dp(ctx, 22); form.setPadding(p, dp(ctx, 8), p, dp(ctx, 8));
        android.widget.TextView where = new android.widget.TextView(ctx); where.setText(CoordinateClipboard.text(latitude, longitude)); where.setTextSize(14);
        form.addView(where);
        EditText name = input(ctx, "Place name (up to " + SavedPlaces.NAME_LIMIT + " characters)");
        name.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_CAP_SENTENCES);
        name.setFilters(new android.text.InputFilter[]{new android.text.InputFilter.LengthFilter(SavedPlaces.NAME_LIMIT)});
        form.addView(name);
        AlertDialog dialog = new AlertDialog.Builder(ctx).setTitle("Add to Favorites").setView(form)
            .setNegativeButton("Cancel", null).setPositiveButton("Save", null).create();
        dialog.setOnShowListener(d -> dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v -> {
            final SavedPlaces.Entry entry;
            try { entry = new SavedPlaces.Entry(name.getText().toString(), latitude, longitude); }
            catch (IllegalArgumentException e) { name.setError(e.getMessage()); return; }
            dialog.getButton(AlertDialog.BUTTON_POSITIVE).setEnabled(false);
            new Thread(() -> {
                String error = null;
                try { new SavedPlaces(activity).add(entry); }
                catch (java.io.IOException | RuntimeException e) { error = e.getMessage() == null ? "Could not save this place." : e.getMessage(); }
                final String failed = error;
                activity.runOnUiThread(() -> {
                    if (!dialog.isShowing()) return;
                    if (failed != null) { dialog.getButton(AlertDialog.BUTTON_POSITIVE).setEnabled(true); name.setError(failed); return; }
                    dialog.dismiss();
                    Toast.makeText(activity, "Saved " + entry.name + " to Favorites", Toast.LENGTH_SHORT).show();
                });
            }, "pgrazor-favorites-save").start();
        }));
        show(activity, dialog);
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
