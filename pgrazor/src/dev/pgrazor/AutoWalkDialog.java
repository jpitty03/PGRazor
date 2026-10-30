package dev.pgrazor;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.text.InputFilter;
import android.text.InputType;
import android.view.Gravity;
import android.view.View;
import android.view.ContextThemeWrapper;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import android.os.Handler;
import android.os.Looper;

/**
 * Modal that starts an auto-walk, either over nearby map POIs the game has already loaded (Nearby
 * mode) or along a GPX route the user imported or saved (GPX mode).
 *
 * <p>Nearby mode only reads the shared POI snapshot (never scans or teleports): while open it renews
 * the snapshot through {@link Control#setAutoWalkPlanning(boolean)}, reports whether data is
 * loading/empty/stale, and builds a deterministic nearest-neighbour route of 1–{@link
 * AutoWalkPlanner#MAX_STOPS} stops with {@link AutoWalkPlanner}. GPX mode (reached via Import GPX /
 * Saved routes, both delegated to {@link GpxRouteDialog}) needs no POIs at all and walks the chosen
 * route in its exact saved order. Either way Start applies only an explicit user action and hands
 * the route to {@link Control#startAutoWalk}.</p>
 */
public final class AutoWalkDialog {
    private AutoWalkDialog() {}

    /** Safety ceiling; the selectable maximum follows the loaded nearby locations. */
    private static final int NEARBY_LIMIT = AutoWalkPlanner.MAX_STOPS;

    /**
     * Route imported or picked through {@link GpxRouteDialog}. The system file picker pauses the
     * host and dismisses this dialog, so the choice is stashed here and restored the next time the
     * dialog is shown. Cleared when a route is started or the user returns to Nearby mode.
     */
    private static volatile GpxRoute pendingGpx;

    /** Bound to one dialog lifetime; all timers and the snapshot request stop on dismiss/destroy. */
    public static AlertDialog show(Activity activity) {
        if(activity.isFinishing()||activity.isDestroyed())return null;
        Context ctx = new ContextThemeWrapper(activity, Control.darkTheme()
            ? android.R.style.Theme_Material_Dialog_Alert : android.R.style.Theme_Material_Light_Dialog_Alert);

        LinearLayout layout = new LinearLayout(ctx);
        layout.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(activity, 18);
        layout.setPadding(pad, dp(activity, 8), pad, 0);

        TextView descr = label(ctx, "Visit nearby PokéStops and Gyms, nearest first.", 13);
        layout.addView(descr);

        LinearLayout gpxRow = new LinearLayout(ctx);
        gpxRow.setOrientation(LinearLayout.HORIZONTAL);
        gpxRow.setPadding(0, dp(activity, 8), 0, 0);
        Button importBtn = button(ctx, "Import GPX");
        Button savedBtn = button(ctx, "Saved routes");
        Button nearbyBtn = button(ctx, "Nearby"); nearbyBtn.setVisibility(View.GONE);
        nearbyBtn.setContentDescription("Return to nearby PoI planning");
        gpxRow.addView(importBtn, new LinearLayout.LayoutParams(0, dp(activity, 46), 1));
        gpxRow.addView(savedBtn, new LinearLayout.LayoutParams(0, dp(activity, 46), 1));
        gpxRow.addView(nearbyBtn, new LinearLayout.LayoutParams(0, dp(activity, 46), 1));
        layout.addView(gpxRow);

        LinearLayout countRow = new LinearLayout(ctx);
        countRow.setOrientation(LinearLayout.HORIZONTAL);
        countRow.setGravity(Gravity.CENTER_VERTICAL);
        countRow.setPadding(0, dp(activity, 10), 0, 0);
        TextView countLabel = label(ctx, "Stops (loading):", 15);
        countRow.addView(countLabel,
            new LinearLayout.LayoutParams(0, -2, 1));
        EditText countField = new EditText(ctx);
        countField.setInputType(InputType.TYPE_CLASS_NUMBER);
        countField.setFilters(new InputFilter[]{new InputFilter.LengthFilter(String.valueOf(NEARBY_LIMIT).length())});
        countField.setHint("All");
        countField.setText("50");
        countField.setSingleLine(true);
        countField.setContentDescription("Number of stops to visit; leave blank for all nearby locations");
        countRow.addView(countField, new LinearLayout.LayoutParams(dp(activity, 72), -2));
        layout.addView(countRow);

        CheckBox loop = new CheckBox(ctx);
        loop.setText("Loop route");
        loop.setTextSize(14);
        loop.setChecked(false);
        layout.addView(loop);

        TextView status = label(ctx, "Waiting for nearby game locations…", 13);
        status.setPadding(0, dp(activity, 10), 0, 0);
        layout.addView(status);
        TextView gpxInfo = label(ctx, "", 13);
        gpxInfo.setPadding(0, dp(activity, 10), 0, 0);
        gpxInfo.setVisibility(View.GONE);
        layout.addView(gpxInfo);
        Button saveGpx = new Button(ctx);saveGpx.setText("Save GPX");saveGpx.setAllCaps(false);saveGpx.setVisibility(View.GONE);layout.addView(saveGpx);
        TextView running = label(ctx, "", 13);
        running.setPadding(0, dp(activity, 4), 0, dp(activity, 4));
        layout.addView(running);

        AlertDialog dialog = new AlertDialog.Builder(ctx)
            .setTitle("Auto-walk")
            .setView(layout)
            .setNegativeButton("Close", null)
            .setNeutralButton("Stop auto-walk", null)
            .setPositiveButton("Start", null)
            .create();

        Controller controller = new Controller(activity, dialog, descr, countLabel, countRow, countField, loop,
            status, gpxInfo, running, importBtn, savedBtn, nearbyBtn, saveGpx);
        DialogLifecycle.bind(activity,dialog,controller::close);
        dialog.setOnShowListener(d -> controller.attach());
        dialog.show();
        return dialog;
    }

    /** Owns the repeating poll, the Nearby/GPX mode switch, the Start/Stop wiring, and tears down once. */
    private static final class Controller {
        private final Activity activity;
        private final AlertDialog dialog;
        private final TextView descr, countLabel, status, gpxInfo, running;
        private final LinearLayout countRow;
        private final EditText countField;
        private final CheckBox loop;
        private final Button importBtn, savedBtn, nearbyBtn, saveGpx;
        private final Handler main = new Handler(Looper.getMainLooper());
        private final Runnable poll = this::poll;
        private GpxRoute selectedGpx;
        private int selectedCount;
        private boolean closed;

        Controller(Activity activity, AlertDialog dialog, TextView descr, TextView countLabel, LinearLayout countRow, EditText countField,
                   CheckBox loop, TextView status, TextView gpxInfo, TextView running,
                   Button importBtn, Button savedBtn, Button nearbyBtn, Button saveGpx) {
            this.activity = activity; this.dialog = dialog; this.descr = descr; this.countRow = countRow;
            this.countLabel = countLabel; this.countField = countField; this.loop = loop; this.status = status; this.gpxInfo = gpxInfo;
            this.saveGpx=saveGpx;this.running = running; this.importBtn = importBtn; this.savedBtn = savedBtn; this.nearbyBtn = nearbyBtn;
        }

        void attach() {
            dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v -> start());
            dialog.getButton(AlertDialog.BUTTON_NEUTRAL).setOnClickListener(v -> stop());
            importBtn.setOnClickListener(v -> GpxRouteDialog.importGpx(activity, "Auto-walk",
                route -> { pendingGpx = route; onGpxChosen(route); }));
            savedBtn.setOnClickListener(v -> GpxRouteDialog.chooseSaved(activity,
                route -> { pendingGpx = route; onGpxChosen(route); }));
            nearbyBtn.setOnClickListener(v -> returnToNearby());
            saveGpx.setOnClickListener(v -> GpxRouteDialog.saveRoute(activity,()->selectedGpx));
            setSelected(pendingGpx); // restore a route picked while the picker (or a prior session) was open
            poll(); // begins the snapshot request and first UI update immediately
        }

        /** Delivered on the main thread from {@link GpxRouteDialog}; safe to ignore once closed. */
        private void onGpxChosen(GpxRoute route) {
            if(route==null)return;
            if(closed){GpxRouteDialog.whenResumed(activity,()->AutoWalkDialog.show(activity));return;}
            setSelected(route);
        }

        private void returnToNearby() {
            if (closed) return;
            pendingGpx = null;
            setSelected(null);
        }

        private void setSelected(GpxRoute route) {
            selectedGpx = route;
            selectedCount = route == null ? 0 : route.points().length;
            applyMode();
        }

        /** Switch chrome between Nearby planning and the selected GPX route; selection needs no POIs. */
        private void applyMode() {
            boolean gpx = selectedGpx != null;
            saveGpx.setVisibility(gpx ? View.VISIBLE : View.GONE);
            if(gpx)Control.setAutoWalkPlanning(false);
            descr.setText(gpx ? "Follow the selected GPX route in its saved order."
                : "Visit nearby PokéStops and Gyms, nearest first.");
            countRow.setVisibility(gpx ? View.GONE : View.VISIBLE);
            status.setVisibility(gpx ? View.GONE : View.VISIBLE);
            gpxInfo.setVisibility(gpx ? View.VISIBLE : View.GONE);
            nearbyBtn.setVisibility(gpx ? View.VISIBLE : View.GONE);
            if (gpx) Control.setAutoWalkPlanning(false); // GPX mode needs no POI snapshot; poll() renews it again in Nearby mode
            updateAvailability();
        }

        private void poll() {
            if (closed) return;
            if (activity.isFinishing() || activity.isDestroyed()) {
                if (dialog.isShowing()) dialog.dismiss(); else close();
                return;
            }
            if (selectedGpx == null) Control.setAutoWalkPlanning(true); // renew the nearby snapshot only in Nearby mode
            updateAvailability();
            updateRunning();
            main.removeCallbacks(poll);
            main.postDelayed(poll, 1000);
        }

        /** Loading / unavailable / empty / loaded are each reported plainly; Start follows what is loaded. */
        private void updateAvailability() {
            Button start = dialog.getButton(AlertDialog.BUTTON_POSITIVE);
            if (start == null) return;
            if (selectedGpx != null) {
                gpxInfo.setText(String.format(Locale.US, "GPX route: %s (%d %s). Start walks it in order.",
                    selectedGpx.name, selectedCount, selectedCount == 1 ? "stop" : "stops"));
                start.setEnabled(true);
                return;
            }
            String snapshot = Control.mapPoiSnapshot();
            boolean canStart = false;
            countLabel.setText("Stops (loading):");
            String message;
            if (snapshot.isEmpty()) {
                message = "Waiting for nearby game locations…";
            } else {
                double[][] points = parse(snapshot);
                if (points == null) {
                    message = "Nearby locations are unavailable right now.";
                } else {
                    int unique = AutoWalkPlanner.filter(points).length;
                    int maximum = Math.min(unique, NEARBY_LIMIT);
                    countLabel.setText(maximum == 0 ? "Stops (none nearby):" : "Stops (1–" + maximum + "; blank = all):");
                    if (unique == 0) {
                        message = "No nearby locations loaded yet. Move around in-game to load some.";
                    } else {
                        int requested=maximum;
                        try{requested=Integer.parseInt(countField.getText().toString().trim());}catch(NumberFormatException ignored){}
                        int selected=Math.min(Math.max(1,requested),maximum);
                        message = selected+" stops in route ("+unique+" nearby).";
                        canStart = true;
                    }
                }
            }
            status.setText(message);
            start.setEnabled(canStart);
        }

        private void updateRunning() {
            MotionEngine.RouteProgress progress = Control.engine.routeProgress();
            running.setText(Control.engine.speedKmh==0 ? "Speed is 0 km/h. Use the walking icon to resume travel." : progress.active
                ? String.format(Locale.US, "A route is running: %d of %d stops done.", progress.visited, progress.total)
                : "");
            dialog.getButton(AlertDialog.BUTTON_NEUTRAL).setEnabled(progress.active);
        }

        private void start() {
            if (closed) return;
            if (!Control.canEnable()) { toast("Location adapter is not ready."); return; }
            if (selectedGpx != null) { startGpx(); return; }
            String snapshot = Control.mapPoiSnapshot();
            double[][] points = snapshot.isEmpty() ? null : parse(snapshot);
            if (points == null) { toast("Nearby locations are unavailable right now."); return; }
            int maximum = Math.min(AutoWalkPlanner.filter(points).length, NEARBY_LIMIT);
            if (maximum == 0) { toast("No nearby locations are loaded yet. Move around in-game, then try again."); return; }
            int count;
            String text = countField.getText().toString().trim();
            if (text.isEmpty()) count = maximum;
            else try { count = Integer.parseInt(text); }
            catch (NumberFormatException e) { countField.setError("Enter 1–" + maximum + ", or leave blank for all"); return; }
            if (count < 1 || count > maximum) { countField.setError("Enter 1–" + maximum + ", or leave blank for all"); return; }

            AutoWalkPlanner.Plan plan;
            try { plan = AutoWalkPlanner.plan(Control.engine.latitude, Control.engine.longitude, points, count); }
            catch (IllegalArgumentException e) { toast(e.getMessage()); return; }
            if (plan.empty()) { toast("No nearby locations are loaded yet. Move around in-game, then try again."); return; }

            boolean looping = loop.isChecked();
            try { Control.startAutoWalk(plan.route, looping); }
            catch (IllegalArgumentException e) { toast(e.getMessage()); return; }
            Control.event("autowalk-started", String.format(Locale.US,
                "requested=%d selected=%d available=%d loop=%s", plan.requested, plan.selected(), plan.available, looping));
            if (plan.fewerThanRequested())
                toast("Only " + plan.selected() + " of " + plan.requested + " requested stops are loaded; starting with " + plan.selected() + ".");
            else
                toast("Auto-walking " + plan.selected() + (plan.selected() == 1 ? " stop" : " stops") + (looping ? " on a loop." : "."));
            dialog.dismiss();
        }

        /** Starts the selected GPX route in its exact saved order; the Loop toggle applies as in Nearby mode. */
        private void startGpx() {
            GpxRoute route = selectedGpx;
            if (route == null) return;
            double[][] points = route.points();
            boolean looping = loop.isChecked();
            try { Control.startAutoWalk(points, looping); }
            catch (IllegalArgumentException e) { toast(e.getMessage()); return; }
            Control.event("autowalk-gpx-started", String.format(Locale.US, "name=%s stops=%d loop=%s", route.name, points.length, looping));
            toast("Walking “" + route.name + "”: " + points.length + (points.length == 1 ? " stop" : " stops") + (looping ? ", looping." : "."));
            pendingGpx = null; // consumed
            dialog.dismiss();
        }

        private void stop() {
            if (closed) return;
            if (!Control.engine.routeProgress().active) { updateRunning(); return; }
            Control.engine.stop();
            Control.saveState();
            Control.event("autowalk-stopped", "User stopped the active route from Auto-walk");
            updateRunning();
            toast("Auto-walk stopped.");
        }

        private void toast(String message) {
            if (!closed) Toast.makeText(activity, message, Toast.LENGTH_SHORT).show();
        }

        void close() {
            if (closed) return;
            closed = true;
            main.removeCallbacksAndMessages(null);
            Control.setAutoWalkPlanning(false);
        }
    }

    /** Null means a parse error or an explicit error snapshot; an empty array means no points yet. */
    private static double[][] parse(String snapshot) {
        try {
            JSONObject root = new JSONObject(snapshot);
            if (root.has("error")) return null;
            JSONArray array = root.optJSONArray("points");
            if (array == null) return new double[0][];
            List<double[]> points = new ArrayList<>();
            for (int i = 0; i < array.length(); i++) {
                JSONObject point = array.optJSONObject(i);
                if (point == null) continue;
                points.add(new double[]{point.optDouble("lat", Double.NaN), point.optDouble("lon", Double.NaN)});
            }
            return points.toArray(new double[points.size()][]);
        } catch (JSONException invalid) {
            return null;
        }
    }

    private static int dp(Activity a, float value) { return Math.round(value * a.getResources().getDisplayMetrics().density); }
    private static TextView label(Context c, String text, int size) { TextView v = new TextView(c); v.setText(text); v.setTextSize(size); return v; }
    private static Button button(Context c, String text) { Button b = new Button(c); b.setText(text); b.setAllCaps(false); b.setTextSize(13); return b; }
}
