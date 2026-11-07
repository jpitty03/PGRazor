package dev.pgrazor;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.graphics.drawable.ColorDrawable;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

/**
 * Automation hub (Shundo Sniper and one-shot tasks), opened from the quick menu and mirrored in the settings panel's
 * "Automation" section. Shundo Sniper runs until stopped; every other task runs once when the user starts it, with no recurring
 * toggles. The Daily Incense Session and Remote Gym Feeding dialogs drive the coordinator-owned
 * {@link OneShotAutomation} runtime; Mass Evolve reuses the existing {@link BatchEvolutionSettings}.
 * Chrome follows {@link MapTheme}; polling and dialogs stay scoped through {@link DialogLifecycle}.
 */
public final class AutomationPanel {
    private AutomationPanel() {}

    /** The compact menu listing the automation tasks. */
    public static void show(Activity activity) {
        if (activity == null || activity.isFinishing() || activity.isDestroyed()) return;
        Context ui = MapTheme.wrap(activity);
        LinearLayout body = new LinearLayout(ui); body.setOrientation(LinearLayout.VERTICAL);
        int pad = MapTheme.dp(ui, 16); body.setPadding(pad, pad, pad, pad);
        TextView intro = new TextView(ui);
        intro.setText("Each task runs when you start it.");
        body.addView(intro);
        final AlertDialog[] holder = new AlertDialog[1];
        entry(ui, body, "Shundo Sniper (Experimental)", () -> { if (holder[0] != null) holder[0].dismiss(); ShundoSniper.show(activity); });
        entry(ui, body, "Daily Incense Session", () -> { if (holder[0] != null) holder[0].dismiss(); showIncense(activity); });
        entry(ui, body, "Remote Gym Feeding", () -> { if (holder[0] != null) holder[0].dismiss(); showRemoteFeed(activity); });
        entry(ui, body, "Send & Open Gifts", () -> { if (holder[0] != null) holder[0].dismiss(); GiftBatchDialog.show(activity); });
        entry(ui, body, "Mass Evolve", () -> { if (holder[0] != null) holder[0].dismiss(); BatchEvolutionSettings.show(activity); });
        ScrollView scroll = new ScrollView(ui); scroll.addView(body);
        AlertDialog dialog = new AlertDialog.Builder(ui).setTitle("Automation").setView(scroll)
            .setPositiveButton("Close", null).create();
        holder[0] = dialog;
        DialogLifecycle.bind(activity, dialog, null);
        dialog.setOnShowListener(d -> {
            MapTheme.style(body); MapTheme.styleButtons(dialog);
            dialog.getWindow().setBackgroundDrawable(new ColorDrawable(MapTheme.surface()));
        });
        dialog.show();
    }

    /** Daily Adventure Incense geodesic session: a single manual run, stopped by the user. */
    public static void showIncense(Activity activity) {
        if (activity == null || activity.isFinishing() || activity.isDestroyed()) return;
        Context ui = MapTheme.wrap(activity);
        LinearLayout body = new LinearLayout(ui); body.setOrientation(LinearLayout.VERTICAL);
        int pad = MapTheme.dp(ui, 16); body.setPadding(pad, pad, pad, pad);
        TextView description = new TextView(ui);
        description.setText("Activate Daily Adventure Incense in the game first. This steers a straight line at 10.5 km/h "
            + "for up to 15 minutes. Stop any time. "
            + "Galarian birds open automatically for you to handle. Walking and VGP stop. Move the joystick to resume idle walking.");
        body.addView(description);
        TextView headingLabel = new TextView(ui); headingLabel.setText("Heading (0–359°)"); body.addView(headingLabel);
        EditText headingInput = new EditText(ui); headingInput.setSingleLine(true);
        headingInput.setInputType(InputType.TYPE_CLASS_NUMBER); headingInput.setText(defaultHeading());
        body.addView(headingInput);
        TextView status = new TextView(ui); status.setText(OneShotAutomation.incenseStatus()); body.addView(status);
        Button start = new Button(ui); start.setAllCaps(false); start.setText("Start session"); body.addView(start);
        Button stop = new Button(ui); stop.setAllCaps(false); stop.setText("Stop session"); body.addView(stop);
        ScrollView scroll = new ScrollView(ui); scroll.addView(body);
        AlertDialog dialog = new AlertDialog.Builder(ui).setTitle("Daily Incense Session").setView(scroll)
            .setPositiveButton("Close", null).create();
        Handler handler = new Handler(Looper.getMainLooper());
        Runnable update = () -> {
            boolean running = OneShotAutomation.incenseRunning();
            SettingsUi.enabled(headingInput, !running);
            start.setEnabled(!running); stop.setEnabled(running||OneShotAutomation.idleBlocked());
            status.setText(OneShotAutomation.incenseStatus());
        };
        Runnable refresh = new Runnable() { public void run() { if (dialog.isShowing()) { update.run(); handler.postDelayed(this, 1000); } } };
        start.setOnClickListener(v -> {
            int heading;
            try { heading = parseHeading(headingInput.getText().toString()); }
            catch (IllegalArgumentException invalid) { headingInput.setError(invalid.getMessage()); return; }
            try { OneShotAutomation.startIncense(heading); }
            catch (IllegalArgumentException | IllegalStateException refused) { toast(activity, refused.getMessage()); }
            update.run();
        });
        stop.setOnClickListener(v -> { OneShotAutomation.stopIncense(); update.run(); });
        DialogLifecycle.bind(activity, dialog, () -> handler.removeCallbacks(refresh));
        dialog.setOnShowListener(d -> {
            MapTheme.style(body); MapTheme.styleButtons(dialog);
            dialog.getWindow().setBackgroundDrawable(new ColorDrawable(MapTheme.surface()));
            handler.post(refresh);
        });
        dialog.show();
    }

    /** Remote Gym Feeding: one batch across currently defended gyms, using surplus berries. */
    public static void showRemoteFeed(Activity activity) {
        if (activity == null || activity.isFinishing() || activity.isDestroyed()) return;
        Context ui = MapTheme.wrap(activity);
        LinearLayout body = new LinearLayout(ui); body.setOrientation(LinearLayout.VERTICAL);
        int pad = MapTheme.dp(ui, 16); body.setPadding(pad, pad, pad, pad);
        TextView description = new TextView(ui);
        description.setText("Feeds the gyms you currently defend once, using surplus berries. Nanab is fed down to its "
            + "reserve, then surplus Razz. Pinap is optional and off by default. A single batch runs now — there is no "
            + "recurring schedule. The game's own server limits still apply, so some feeds may be declined. No dust or "
            + "candy payout is promised.");
        body.addView(description);
        EditText nanab = numberField(ui, "0");
        TextView nanabLabel = new TextView(ui); nanabLabel.setText("Nanab Berry reserve (0–1000)"); body.addView(nanabLabel); body.addView(nanab);
        EditText razz = numberField(ui, "20");
        TextView razzLabel = new TextView(ui); razzLabel.setText("Razz Berry reserve (0–1000)"); body.addView(razzLabel); body.addView(razz);
        CheckBox pinap = new CheckBox(ui); pinap.setText("Also feed surplus Pinap berries"); pinap.setChecked(false); body.addView(pinap);
        EditText pinapReserve = numberField(ui, "50");
        TextView pinapLabel = new TextView(ui); pinapLabel.setText("Pinap Berry reserve (0–1000)"); body.addView(pinapLabel); body.addView(pinapReserve);
        TextView status = new TextView(ui); status.setText(OneShotAutomation.remoteFeedStatus()); body.addView(status);
        Button start = new Button(ui); start.setAllCaps(false); start.setText("Start feed"); body.addView(start);
        Button stop = new Button(ui); stop.setAllCaps(false); stop.setText("Stop feed"); body.addView(stop);
        ScrollView scroll = new ScrollView(ui); scroll.addView(body);
        AlertDialog dialog = new AlertDialog.Builder(ui).setTitle("Remote Gym Feeding").setView(scroll)
            .setPositiveButton("Close", null).create();
        Handler handler = new Handler(Looper.getMainLooper());
        Runnable update = () -> {
            boolean running = OneShotAutomation.remoteFeedRunning();
            SettingsUi.enabled(nanab, !running); SettingsUi.enabled(razz, !running); pinap.setEnabled(!running);
            SettingsUi.enabled(pinapReserve, !running && pinap.isChecked());
            start.setEnabled(!running); stop.setEnabled(running);
            status.setText(OneShotAutomation.remoteFeedStatus());
        };
        Runnable refresh = new Runnable() { public void run() { if (dialog.isShowing()) { update.run(); handler.postDelayed(this, 1000); } } };
        pinap.setOnCheckedChangeListener((v, on) -> update.run());
        start.setOnClickListener(v -> {
            int nanabReserve, razzReserve;
            try { nanabReserve = parseReserve(nanab.getText().toString()); }
            catch (IllegalArgumentException invalid) { nanab.setError(invalid.getMessage()); return; }
            try { razzReserve = parseReserve(razz.getText().toString()); }
            catch (IllegalArgumentException invalid) { razz.setError(invalid.getMessage()); return; }
            boolean feedPinap = pinap.isChecked();
            int pinapReserveValue;
            if (feedPinap) {
                try { pinapReserveValue = parseReserve(pinapReserve.getText().toString()); }
                catch (IllegalArgumentException invalid) { pinapReserve.setError(invalid.getMessage()); return; }
            } else {
                pinapReserveValue = reserveOr(pinapReserve.getText().toString(), 50);
            }
            try { OneShotAutomation.startRemoteFeed(nanabReserve, razzReserve, feedPinap, pinapReserveValue); }
            catch (IllegalArgumentException | IllegalStateException refused) { toast(activity, refused.getMessage()); }
            update.run();
        });
        stop.setOnClickListener(v -> { OneShotAutomation.stopRemoteFeed(); update.run(); });
        DialogLifecycle.bind(activity, dialog, () -> handler.removeCallbacks(refresh));
        dialog.setOnShowListener(d -> {
            MapTheme.style(body); MapTheme.styleButtons(dialog);
            dialog.getWindow().setBackgroundDrawable(new ColorDrawable(MapTheme.surface()));
            handler.post(refresh);
        });
        dialog.show();
    }

    private static Button entry(Context ui, LinearLayout body, String label, Runnable action) {
        Button b = new Button(ui); b.setAllCaps(false); b.setText(label); body.addView(b);
        b.setOnClickListener(v -> action.run()); return b;
    }

    private static EditText numberField(Context ui, String initial) {
        EditText field = new EditText(ui); field.setSingleLine(true);
        field.setInputType(InputType.TYPE_CLASS_NUMBER); field.setText(initial); return field;
    }

    /** The current engine bearing is the sensible default, falling back to due north if non-finite. */
    private static String defaultHeading() {
        double bearing = Control.engine.bearing;
        if (!Double.isFinite(bearing)) return "0";
        int heading = (((int) Math.round(bearing)) % 360 + 360) % 360;
        return String.valueOf(heading);
    }

    private static int parseHeading(String value) {
        try { int n = Integer.parseInt(value.trim()); if (n >= 0 && n <= 359) return n; }
        catch (NumberFormatException ignored) {}
        throw new IllegalArgumentException("Enter a heading from 0 to 359.");
    }

    private static int parseReserve(String value) {
        try { int n = Integer.parseInt(value.trim()); if (n >= 0 && n <= 1000) return n; }
        catch (NumberFormatException ignored) {}
        throw new IllegalArgumentException("Enter a whole number from 0 to 1000.");
    }

    private static int reserveOr(String value, int fallback) {
        try { int n = Integer.parseInt(value.trim()); if (n >= 0 && n <= 1000) return n; }
        catch (NumberFormatException ignored) {}
        return fallback;
    }

    private static void toast(Activity activity, String message) {
        Toast.makeText(activity, message == null || message.isEmpty() ? "Could not start." : message, Toast.LENGTH_SHORT).show();
    }
}
