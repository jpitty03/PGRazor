package dev.pgrazor;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.graphics.drawable.ColorDrawable;
import android.os.Handler;
import android.os.Looper;
import android.text.Editable;
import android.text.TextWatcher;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;
import java.util.List;
import java.util.Properties;
import org.json.JSONArray;
import org.json.JSONObject;

/**
 * Field research automation: auto-claim completed field research and auto-remove selected tasks.
 * Preferences persist with Control's other settings; the native coordinator reads {@link #config()}
 * on its transport loop and reports the observed task catalog and request results to
 * {@link #publish(String)}.
 */
public final class ResearchSettings {
    private ResearchSettings() {}
    private static final ResearchPreferences preferences = new ResearchPreferences();
    private static final Object LOCK = new Object();
    private static String message = "";
    private static int claimed, removed, caught;

    public static void read(Properties p) { preferences.read(p); }
    public static void write(Properties p) { preferences.write(p); }
    public static String config() { return preferences.config(); }

    /** Native "research" events: the observed catalog, or one request result. */
    public static void publish(String detail) {
        try {
            JSONObject o = new JSONObject(detail);
            String phase = o.optString("phase", "");
            if ("catalog".equals(phase)) {
                JSONArray tasks = o.optJSONArray("tasks");
                boolean changed = false;
                for (int i = 0; tasks != null && i < tasks.length(); i++) {
                    JSONObject t = tasks.optJSONObject(i);
                    if (t != null) changed |= preferences.observe(t.optString("t"), t.optString("d"), t.optString("r"));
                }
                if (changed) Control.saveSettings();
                return;
            }
            synchronized (LOCK) {
                if ("claimed".equals(phase)) claimed++;
                if ("removed".equals(phase)) removed++;
                if ("caught".equals(phase)) caught++;
                message = o.optString("message", "");
            }
        } catch (Exception ignored) {}
    }

    static String status() {
        synchronized (LOCK) {
            String counts = "This session: claimed " + claimed + ", removed " + removed + ", caught " + caught;
            return message.isEmpty() ? counts : counts + "\nLast: " + message;
        }
    }

    public static void show(Activity activity) {
        if (activity == null || activity.isFinishing() || activity.isDestroyed()) return;
        Context ui = MapTheme.wrap(activity);
        LinearLayout body = new LinearLayout(ui); body.setOrientation(LinearLayout.VERTICAL);
        int pad = MapTheme.dp(ui, 16); body.setPadding(pad, pad, pad, pad);

        CheckBox claim = new CheckBox(ui); claim.setText("Auto-claim completed field research");
        claim.setChecked(preferences.claim()); body.addView(claim);
        TextView claimHelp = new TextView(ui);
        claimHelp.setText("Claims field research once its goal is met. Pokémon encounter rewards wait in the game's research view for you to catch. Special, timed and story research stay manual.");
        body.addView(claimHelp);

        CheckBox remove = new CheckBox(ui); remove.setText("Auto-delete selected tasks");
        remove.setChecked(preferences.remove()); body.addView(remove);
        TextView removeHelp = new TextView(ui);
        removeHelp.setText("Deletes unfinished field research whose task is ticked below. Gym bonus tasks are never deleted.");
        body.addView(removeHelp);

        CheckBox catchEncounters = new CheckBox(ui); catchEncounters.setText("Auto-catch research encounters");
        catchEncounters.setChecked(preferences.catchEncounters()); body.addView(catchEncounters);
        TextView catchHelp = new TextView(ui);
        catchHelp.setText("Catches waiting field research encounters without the encounter screen, using your VGP+ ball and throw settings. Each throw uses a ball. Caught Pokémon follow your transfer rules, including Keep All. Works alongside VGP.");
        body.addView(catchHelp);

        EditText search = new EditText(ui); search.setHint("Search tasks or rewards"); search.setSingleLine(true);
        body.addView(search);
        TextView empty = new TextView(ui);
        empty.setText("No field research seen yet. Tasks appear here as the game gives them to you while PGRazor runs.");
        body.addView(empty);
        LinearLayout list = new LinearLayout(ui); list.setOrientation(LinearLayout.VERTICAL); body.addView(list);
        TextView status = new TextView(ui); status.setText(status()); body.addView(status);

        Runnable fill = () -> {
            list.removeAllViews();
            List<String[]> tasks = preferences.tasks(search.getText().toString());
            empty.setVisibility(tasks.isEmpty() ? android.view.View.VISIBLE : android.view.View.GONE);
            // Different tasks can share their text and reward (an event and a season "Win a raid");
            // the game's task ID tells those rows apart.
            java.util.Map<String, Integer> same = new java.util.HashMap<>();
            for (String[] task : tasks) same.merge(task[1] + "\n" + task[2], 1, Integer::sum);
            for (String[] task : tasks) {
                CheckBox box = new CheckBox(ui);
                String label = task[2].isEmpty() ? task[1] : task[1] + "\n" + task[2];
                if (same.get(task[1] + "\n" + task[2]) > 1) label += "\n" + task[0];
                box.setText(label);
                box.setChecked(preferences.selected(task[0]));
                box.setOnCheckedChangeListener((v, on) -> {
                    try { preferences.select(task[0], on); Control.saveSettings(); }
                    catch (IllegalArgumentException refused) { v.setChecked(!on); toast(activity, refused.getMessage()); }
                });
                list.addView(box);
            }
            MapTheme.style(list);
        };
        claim.setOnCheckedChangeListener((v, on) -> { preferences.setClaim(on); Control.saveSettings(); });
        remove.setOnCheckedChangeListener((v, on) -> { preferences.setRemove(on); Control.saveSettings(); });
        catchEncounters.setOnCheckedChangeListener((v, on) -> { preferences.setCatchEncounters(on); Control.saveSettings(); });
        search.addTextChangedListener(new TextWatcher() {
            public void beforeTextChanged(CharSequence s, int a, int b, int c) {}
            public void onTextChanged(CharSequence s, int a, int b, int c) {}
            public void afterTextChanged(Editable s) { fill.run(); }
        });
        fill.run();

        ScrollView scroll = new ScrollView(ui); scroll.addView(body);
        AlertDialog dialog = new AlertDialog.Builder(ui).setTitle("Research").setView(scroll)
            .setPositiveButton("Done", null).create();
        Handler handler = new Handler(Looper.getMainLooper());
        Runnable refresh = new Runnable() {
            public void run() { if (dialog.isShowing()) { status.setText(status()); handler.postDelayed(this, 1000); } }
        };
        DialogLifecycle.bind(activity, dialog, () -> handler.removeCallbacks(refresh));
        dialog.setOnShowListener(d -> {
            MapTheme.style(body); MapTheme.styleButtons(dialog);
            dialog.getWindow().setBackgroundDrawable(new ColorDrawable(MapTheme.surface()));
            handler.post(refresh);
        });
        dialog.show();
    }

    private static void toast(Activity activity, String text) {
        Toast.makeText(activity, text == null || text.isEmpty() ? "Not saved." : text, Toast.LENGTH_SHORT).show();
    }
}
