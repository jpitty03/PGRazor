package dev.pgrazor;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.graphics.drawable.ColorDrawable;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.view.View;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

/**
 * "Auto Rocket" dialog in Automation. Edits a detached {@link AutoRocketPreferences} draft; every change
 * is saved through {@link AutoRocket#configure}, and Start freezes it for the session. Settings are
 * locked while a session runs.
 */
public final class AutoRocketDialog {
    private AutoRocketDialog() {}

    public static void show(Activity activity) {
        if (activity == null || activity.isFinishing() || activity.isDestroyed()) return;
        Context ui = MapTheme.wrap(activity);
        LinearLayout body = new LinearLayout(ui); body.setOrientation(LinearLayout.VERTICAL);
        int pad = MapTheme.dp(ui, 16); body.setPadding(pad, pad, pad, pad);

        final AutoRocketPreferences draft = AutoRocket.preferences();

        TextView description = new TextView(ui);
        description.setText("Goes to the nearest Team GO Rocket stop the game has loaded, battles, catches the shadow Pokémon "
            + "with its Premier Balls, then moves on to the next. Battles use the Rocket auto attack and shield, Smart battling "
            + "and Smart healing if they are on. Stops at the battle limit or after two losses in a row; Stop ends the session "
            + "after the current battle.");
        body.addView(description);

        TextView status = new TextView(ui); body.addView(status);

        final TextView targets = button(body, "");
        final TextView maxBattles = button(body, "");
        final TextView travel = button(body, "");
        final TextView distance = button(body, "");
        final TextView catches = button(body, "");
        TextView note = new TextView(ui);
        note.setText("Leaders need an equipped Rocket Radar. Giovanni is never battled automatically.");
        body.addView(note);

        final Button start = new Button(ui); start.setAllCaps(false); start.setText("Start"); body.addView(start);
        final Button stop = new Button(ui); stop.setAllCaps(false); stop.setText("Stop"); body.addView(stop);

        final View[] config = {targets, maxBattles, travel, distance, catches};
        final Runnable update = () -> {
            boolean running = AutoRocket.running();
            status.setText(AutoRocket.status());
            targets.setText("Battle: " + AutoRocketPreferences.TARGET_LABELS[draft.targets]);
            maxBattles.setText("Maximum battles: " + draft.maxBattles);
            travel.setText("Travel to stops: " + (draft.walk ? "Walk" : "Teleport"));
            distance.setText("Stay within: " + AutoRocketPreferences.distanceSummary(draft.maxDistanceKm));
            catches.setText("Caught Pokémon: " + (draft.useTransferRules ? "Use my transfer rules" : "Keep all"));
            for (View v : config) SettingsUi.enabled(v, !running);
            start.setEnabled(!running);
            stop.setEnabled(running);
        };

        targets.setOnClickListener(v -> choose(activity, "Battle", new String[]{
            "Grunts only\nRegular grunts at invaded PokéStops",
            "Grunts and leaders\nAlso Arlo, Cliff and Sierra when a Rocket Radar is equipped"
        }, i -> { draft.targets = i; apply(activity, draft, update); }));
        maxBattles.setOnClickListener(v -> intInput(activity, draft, update, "Maximum battles ("
            + AutoRocketPreferences.MAX_BATTLES_MIN + "-" + AutoRocketPreferences.MAX_BATTLES_MAX + ")",
            AutoRocketPreferences.MAX_BATTLES_MIN, AutoRocketPreferences.MAX_BATTLES_MAX, draft.maxBattles));
        travel.setOnClickListener(v -> choose(activity, "Travel to stops", new String[]{
            "Walk\nMove there at walking speed",
            "Teleport\nJump straight to the stop (waits for cooldown before battling)"
        }, i -> { draft.walk = i == 0; apply(activity, draft, update); }));
        distance.setOnClickListener(v -> {
            final int[] choices = AutoRocketPreferences.DISTANCE_CHOICES_KM;
            String[] labels = new String[choices.length];
            for (int i = 0; i < choices.length; i++)
                labels[i] = choices[i] == 0 ? "No limit\nGo to the nearest Rocket stop wherever it is"
                    : choices[i] + " km\nOnly stops within " + choices[i] + " km of where you press Start";
            choose(activity, "Stay within", labels, i -> { draft.maxDistanceKm = choices[i]; apply(activity, draft, update); });
        });
        catches.setOnClickListener(v -> choose(activity, "Caught Pokémon", new String[]{
            "Keep all\nEvery shadow Pokémon caught is kept",
            "Use my transfer rules\nEach catch goes through your Auto transfer rules (Auto transfer must be on). "
                + "With Keep shadow Pokémon on in those rules, shadows are always kept"
        }, i -> { draft.useTransferRules = i == 1; apply(activity, draft, update); }));

        start.setOnClickListener(v -> {
            try { AutoRocket.start(draft); }
            catch (IllegalArgumentException | IllegalStateException refused) { toast(activity, refused.getMessage()); }
            update.run();
        });
        stop.setOnClickListener(v -> { AutoRocket.stop(); update.run(); });

        update.run();
        ScrollView scroll = new ScrollView(ui); scroll.addView(body);
        AlertDialog dialog = new AlertDialog.Builder(ui).setTitle("Auto Rocket").setView(scroll)
            .setPositiveButton("Done", null).create();
        Handler handler = new Handler(Looper.getMainLooper());
        Runnable refresh = new Runnable() {
            public void run() { if (dialog.isShowing()) { update.run(); handler.postDelayed(this, 1000); } }
        };
        DialogLifecycle.bind(activity, dialog, () -> handler.removeCallbacks(refresh));
        dialog.setOnShowListener(d -> {
            MapTheme.style(body); MapTheme.styleButtons(dialog);
            dialog.getWindow().setBackgroundDrawable(new ColorDrawable(MapTheme.surface()));
            handler.post(refresh);
        });
        dialog.show();
    }

    /** Saves the draft through the controller, reverting it and reporting any rejection. */
    private static void apply(Activity activity, AutoRocketPreferences draft, Runnable update) {
        try { AutoRocket.configure(draft); }
        catch (IllegalArgumentException | IllegalStateException refused) {
            java.util.Properties saved = new java.util.Properties();
            AutoRocket.preferences().write(saved); draft.read(saved);
            toast(activity, refused.getMessage());
        }
        update.run();
    }

    private interface Chosen { void pick(int index); }

    private static void choose(Activity a, String title, String[] labels, Chosen chosen) {
        Context ui = MapTheme.wrap(a);
        AlertDialog d = new AlertDialog.Builder(ui).setTitle(title).setNegativeButton("Cancel", null).create();
        ChoiceList.attach(d, labels, chosen::pick);
        DialogLifecycle.bind(a, d, null);
        d.setOnShowListener(x -> { MapTheme.styleButtons(d);
            d.getWindow().setBackgroundDrawable(new ColorDrawable(MapTheme.surface()));
        });
        d.show();
    }

    private static void intInput(Activity a, AutoRocketPreferences draft, Runnable update, String title, int min, int max, int current) {
        Context ui = MapTheme.wrap(a);
        EditText edit = new EditText(ui); edit.setSingleLine(true); edit.setText(String.valueOf(current));
        edit.setInputType(InputType.TYPE_CLASS_NUMBER);
        AlertDialog d = new AlertDialog.Builder(ui).setTitle(title).setView(edit)
            .setPositiveButton("Save", null).setNegativeButton("Cancel", null).create();
        DialogLifecycle.bind(a, d, null);
        d.setOnShowListener(x -> { MapTheme.style(edit); MapTheme.styleButtons(d);
            d.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(w -> {
                try {
                    int n = Integer.parseInt(edit.getText().toString().trim());
                    if (n < min || n > max) throw new NumberFormatException();
                    draft.maxBattles = n; apply(a, draft, update); d.dismiss();
                } catch (NumberFormatException e) { edit.setError("Enter a whole number from " + min + " to " + max + "."); }
            });
        });
        d.show();
    }

    private static TextView button(LinearLayout body, String text) {
        Button b = new Button(body.getContext()); b.setText(text); b.setAllCaps(false); body.addView(b); return b;
    }

    private static void toast(Activity activity, String message) {
        Toast.makeText(activity, message == null || message.isEmpty() ? "Could not update Auto Rocket." : message,
            Toast.LENGTH_SHORT).show();
    }
}
