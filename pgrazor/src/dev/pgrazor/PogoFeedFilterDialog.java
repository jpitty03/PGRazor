package dev.pgrazor;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.SharedPreferences;
import android.text.Editable;
import android.text.InputType;
import android.text.TextWatcher;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.BaseAdapter;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.TreeSet;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

/**
 * Scrollable, themed modal that edits a PoGoCoordinates display-filter set. It never talks to the network
 * or constructs request bodies; it only mutates a caller-owned {@link JSONObject} copy of the current
 * filters and hands it back on Apply so the owner can validate it through
 * {@code PogoFeedFilters.fromJson(...)}, persist it and refetch.
 *
 * <p>Two modes are supported. In the full "Nearby" mode every one of the 21 viewport display-filter
 * keys is editable, plus local named presets. In "My Shinies" mode only the 8 keys the shinies
 * endpoint honours (species mode/list, IV min/max, level min/max, size XXS/XXL) are shown; the other
 * 13 keys are left untouched in the working copy so the user's Nearby settings are preserved.
 */
public final class PogoFeedFilterDialog {
    private PogoFeedFilterDialog() {}

    /** Selectable National Dex range for the species allowlist. */
    private static final int MIN_SPECIES_ID = 1, MAX_SPECIES_ID = 1025;

    /** Owner validates and applies the mutated JSON; returns true when accepted (dialog dismisses). */
    public interface ApplyHandler { boolean apply(JSONObject filters); }

    /** Lets the owner track child dialogs so they are torn down when the feed closes. */
    public interface DialogSink { void track(AlertDialog dialog); }

    /**
     * @param working a fresh {@code toViewportJson()} copy of the current filters (all 21 keys). It is
     *                mutated in place; the owner must only read it back through {@code onApply}.
     */
    public static void show(Activity activity, Context ui, boolean myShiniesMode, JSONObject working,
                            JSONObject speciesNames, SharedPreferences prefs,
                            DialogSink sink, ApplyHandler onApply) {
        if (activity == null || activity.isFinishing() || activity.isDestroyed() || working == null) return;
        new Editor(activity, ui, myShiniesMode, working, speciesNames, prefs, sink, onApply).open();
    }

    /** Holds the working JSON, the live control references and the preset/species sub-dialogs. */
    private static final class Editor {
        private final Activity activity;
        private final Context ui;
        private final boolean myShinies;
        private final JSONObject working;
        private final JSONObject speciesNames;
        private final SharedPreferences prefs;
        private final DialogSink sink;
        private final ApplyHandler onApply;

        private AlertDialog dialog;
        private LinearLayout content;   // repopulated on Reset / preset apply
        private TextView status;
        private final List<NumField> numbers = new ArrayList<>();
        private Button speciesButton;   // "Choose species (N)"; label refreshed on change
        private Button[] speciesModeButtons;
        private String editingPreset;   // PGRazorX: the saved preset being edited (Edit in Saved presets), or null

        Editor(Activity activity, Context ui, boolean myShinies, JSONObject working,
               JSONObject speciesNames, SharedPreferences prefs, DialogSink sink, ApplyHandler onApply) {
            this.activity = activity; this.ui = ui; this.myShinies = myShinies; this.working = working;
            this.speciesNames = speciesNames; this.prefs = prefs; this.sink = sink; this.onApply = onApply;
        }

        void open() {
            ScrollView scroll = new ScrollView(ui);
            scroll.setFillViewport(true);
            content = new LinearLayout(ui);
            content.setOrientation(LinearLayout.VERTICAL);
            int pad = dp(18);
            content.setPadding(pad, dp(10), pad, dp(10));
            scroll.addView(content, new ScrollView.LayoutParams(-1, -2));
            populate();

            dialog = new AlertDialog.Builder(ui)
                .setTitle(myShinies ? "My Shinies filters" : "Filters")
                .setView(scroll)
                .setPositiveButton("Apply", null)   // overridden so invalid input does not dismiss
                .setNeutralButton("Reset", null)     // overridden so Reset does not dismiss
                .setNegativeButton("Cancel", null)   // default dismiss: no change applied
                .create();
            if (sink != null) sink.track(dialog);
            FeedTheme.style(scroll); FeedTheme.styleGameButtons(scroll);
            android.view.Window w = dialog.getWindow();
            if (w != null) w.setBackgroundDrawable(FeedTheme.dialogBackground(ui));
            dialog.setOnShowListener(d -> {
                FeedTheme.styleButtons(dialog); FeedTheme.gameFooter(dialog, null);
                refreshSpeciesMode();
                dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v -> onApplyClicked());
                dialog.getButton(AlertDialog.BUTTON_NEUTRAL).setOnClickListener(v -> onResetClicked());
            });
            dialog.show();
        }

        // ------------------------------------------------------------------ content

        private void populate() {
            content.removeAllViews();
            numbers.clear();
            speciesButton = null; speciesModeButtons = null;

            status = label("", 12);
            status.setPadding(0, 0, 0, dp(6));
            content.addView(status);

            if (myShinies) {
                content.addView(caption("Applies to My Shinies only. Your other Nearby settings are kept."));
                speciesSection();
                ivLevelSection();
                sizeSection();
            } else {
                presetsSection();
                generalSection();
                ivLevelSection();
                alwaysShowSection();
                sizeSection();
                speciesSection();
                pvpSection();
                shiniesSection();
            }
            FeedTheme.style(content); FeedTheme.styleGameButtons(content);
            refreshSpeciesMode();
        }

        private void generalSection() {
            header("General");
            content.addView(checkbox("Show everything (overrides the filters below)", "show_all"));
        }

        private void ivLevelSection() {
            header("IV & level");
            content.addView(numberRow("Min IV", "min_iv", 0, 100));
            content.addView(numberRow("Max IV", "max_iv", 0, 100));
            content.addView(numberRow("Min level", "min_level", 0, 50));
            content.addView(numberRow("Max level", "max_level", 0, 50));
        }

        private void alwaysShowSection() {
            header("Always show (exceptions to the filters)");
            content.addView(checkbox("Perfect (100% IV)", "always_show_perfect"));
            content.addView(checkbox("Zero (0% IV)", "always_show_zero"));
            content.addView(checkbox("Rare species", "always_show_rare"));
            content.addView(checkbox("Notable size", "always_show_size"));
        }

        private void sizeSection() {
            header("Size");
            content.addView(checkbox("XXS filter", "filter_xxs"));
            content.addView(checkbox("XXL filter", "filter_xxl"));
        }

        private void pvpSection() {
            header("PvP");
            content.addView(checkbox("Great League", "show_pvp_great"));
            content.addView(checkbox("Ultra League", "show_pvp_ultra"));
            content.addView(numberRow("Max PvP rank", "pvp_max_rank", 1, PogoFeedFilters.MAX_PVP_RANK));
            content.addView(caption("With a league ticked, each refresh searches one by one, 10 s apart: first by IV, then Great League, then Ultra League. All results are added to the feed."));
        }

        private void shiniesSection() {
            header("Shiny");
            content.addView(checkbox("Shiny only", "shiny_only"));
            content.addView(checkbox("Background only", "background_only"));
        }

        private void speciesSection() {
            header("Species");
            LinearLayout modeRow = new LinearLayout(ui);
            modeRow.setOrientation(LinearLayout.HORIZONTAL);
            speciesModeButtons = new Button[2];
            final String[] modes = {"off", "allowlist"};
            final String[] labels = {"All species", "Allowlist"};
            for (int i = 0; i < 2; i++) {
                final String mode = modes[i];
                Button b = segButton(labels[i]);
                b.setOnClickListener(v -> { put("species_filter_mode", mode); refreshSpeciesMode(); });
                modeRow.addView(b, new LinearLayout.LayoutParams(0, dp(40), 1));
                speciesModeButtons[i] = b;
            }
            content.addView(modeRow);
            speciesButton = button("Choose species");
            speciesButton.setOnClickListener(v -> openSpeciesPicker());
            content.addView(speciesButton, new LinearLayout.LayoutParams(-1, dp(44)));
            refreshSpeciesButton();
        }

        private void presetsSection() {
            header("Presets");
            Button presets = button("Saved presets…");
            presets.setOnClickListener(v -> openPresets());
            content.addView(presets, new LinearLayout.LayoutParams(-1, dp(44)));
            // PGRazorX: while a saved preset is being edited, its changes are written back to it here.
            if (editingPreset != null) {
                final String n = editingPreset;
                content.addView(caption("Editing \"" + n + "\": change the filters below, then save."));
                LinearLayout row = new LinearLayout(ui);
                row.setOrientation(LinearLayout.HORIZONTAL);
                Button saveTo = button("Save to \"" + n + "\""), stop = button("Stop editing");
                row.addView(saveTo, new LinearLayout.LayoutParams(0, dp(44), 1));
                row.addView(stop, new LinearLayout.LayoutParams(0, dp(44), 1));
                content.addView(row, new LinearLayout.LayoutParams(-1, -2));
                saveTo.setOnClickListener(v -> {
                    JSONObject saved = readPresets();
                    if (!saved.has(n)) { toast("That preset no longer exists."); editingPreset = null; populate(); return; }
                    if (savePreset(saved, n)) { status.setText("Preset \"" + n + "\" updated."); toast("Preset updated."); }
                });
                stop.setOnClickListener(v -> { editingPreset = null; populate(); status.setText("Stopped editing the preset; the filters stay as they are."); });
            }
        }

        /** Validates the working filters and stores them under {@code n}; false (with a toast) when it cannot. */
        private boolean savePreset(JSONObject presets, String n) {
            collectNumbers();
            try {
                JSONObject validated = PogoFeedFilters.fromJson(working).toViewportJson();
                JSONObject next = new JSONObject(presets.toString()); next.put(n, validated);
                if (next.toString().length() > 150000) { toast("Saved presets are too large."); return false; }
                presets.put(n, validated);
                writePresets(presets);
                return true;
            } catch (JSONException e) { toast("Couldn't save that preset."); return false; }
        }

        // ------------------------------------------------------------------ species mode state

        private void refreshSpeciesMode() {
            if (speciesModeButtons == null) return;
            boolean allowlist = "allowlist".equals(working.optString("species_filter_mode", "off"));
            highlight(speciesModeButtons[0], !allowlist);
            highlight(speciesModeButtons[1], allowlist);
            if (speciesButton != null) {
                speciesButton.setEnabled(allowlist);
                speciesButton.setAlpha(allowlist ? 1f : 0.5f);
            }
            refreshSpeciesButton();
        }

        private void refreshSpeciesButton() {
            if (speciesButton == null) return;
            JSONArray list = working.optJSONArray("species_filter_list");
            int n = list == null ? 0 : list.length();
            speciesButton.setText(n == 0 ? "Choose species" : "Choose species (" + n + ")");
        }

        // ------------------------------------------------------------------ apply / reset

        private void onApplyClicked() {
            collectNumbers();
            if (onApply != null && onApply.apply(working)) { dialog.dismiss(); return; }
            status.setText("Some values look off (e.g. min above max). Please review and try again.");
        }

        private void onResetClicked() {
            JSONObject defaults;
            try {
                defaults = new PogoFeedFilters().toViewportJson();
            } catch (JSONException e) { toast("Couldn't reset the filters."); return; }
            if (myShinies) {
                // Only reset the keys the shinies endpoint honours; leave Nearby-only keys in place.
                copyKeys(defaults, working, SHINIES_KEYS);
            } else {
                copyAll(defaults, working); // same 21-key set; overwrite in place
            }
            populate();
            status.setText("Filters reset to defaults. Apply to use them.");
        }

        /** Reads every numeric field into the working copy, clamped to its allowed range. */
        private void collectNumbers() {
            for (NumField f : numbers) {
                int current = working.optInt(f.key, f.min);
                int value = current;
                String text = f.view.getText().toString().trim();
                if (!text.isEmpty()) {
                    try { value = Integer.parseInt(text); } catch (NumberFormatException ignored) { value = current; }
                }
                value = Math.max(f.min, Math.min(f.max, value));
                put(f.key, value);
                f.view.setText(Integer.toString(value));
            }
        }

        // ------------------------------------------------------------------ species picker

        private void openSpeciesPicker() {
            final TreeSet<Integer> selected = new TreeSet<>();
            JSONArray list = working.optJSONArray("species_filter_list");
            if (list != null) for (int i = 0; i < list.length(); i++) {
                int id = list.optInt(i, -1);
                if (id >= MIN_SPECIES_ID && id <= MAX_SPECIES_ID) selected.add(id);
            }
            final List<Integer> all = new ArrayList<>();
            for (int id = MIN_SPECIES_ID; id <= MAX_SPECIES_ID; id++) all.add(id);
            final List<Integer> shown = new ArrayList<>(all);

            LinearLayout body = new LinearLayout(ui);
            body.setOrientation(LinearLayout.VERTICAL);
            int pad = dp(14);
            body.setPadding(pad, dp(8), pad, 0);
            final EditText search = new EditText(ui);
            search.setHint("Search name or #id");
            search.setSingleLine(true);
            search.setInputType(InputType.TYPE_CLASS_TEXT);
            body.addView(search, new LinearLayout.LayoutParams(-1, dp(48)));
            final TextView count = label("", 12);
            body.addView(count);
            ListView listView = new ListView(ui);
            final SpeciesAdapter adapter = new SpeciesAdapter(shown, selected);
            listView.setAdapter(adapter);
            listView.setOnItemClickListener((p, v, pos, idv) -> {
                int id = shown.get(pos);
                if (selected.contains(id)) selected.remove(id); else selected.add(id);
                adapter.notifyDataSetChanged();
                count.setText(selected.size() + " selected");
            });
            body.addView(listView, new LinearLayout.LayoutParams(-1, 0, 1));
            count.setText(selected.size() + " selected");

            search.addTextChangedListener(new TextWatcher() {
                public void beforeTextChanged(CharSequence s, int a, int b, int c) {}
                public void onTextChanged(CharSequence s, int a, int b, int c) {}
                public void afterTextChanged(Editable e) {
                    String q = e.toString().trim().toLowerCase(Locale.US);
                    shown.clear();
                    for (int id : all) {
                        if (q.isEmpty()) { shown.add(id); continue; }
                        String name = speciesName(id).toLowerCase(Locale.US);
                        if (name.contains(q) || Integer.toString(id).contains(q)) shown.add(id);
                    }
                    adapter.notifyDataSetChanged();
                }
            });

            AlertDialog picker = new AlertDialog.Builder(ui)
                .setTitle("Species allowlist")
                .setView(body)
                .setPositiveButton("Done", (d, w) -> {
                    JSONArray out = new JSONArray();
                    for (int id : selected) out.put(id);
                    put("species_filter_list", out);
                    if (!selected.isEmpty()) put("species_filter_mode", "allowlist");
                    refreshSpeciesMode();
                })
                .setNeutralButton("Clear", (d, w) -> {
                    put("species_filter_list", new JSONArray());
                    refreshSpeciesMode();
                })
                .setNegativeButton("Cancel", null)
                .create();
            if (sink != null) sink.track(picker);
            picker.setOnShowListener(d -> { FeedTheme.style(body); FeedTheme.styleGameButtons(body); FeedTheme.styleButtons(picker); FeedTheme.gameFooter(picker, null); });
            picker.show();
        }

        private final class SpeciesAdapter extends BaseAdapter {
            private final List<Integer> ids; private final TreeSet<Integer> selected;
            SpeciesAdapter(List<Integer> ids, TreeSet<Integer> selected) { this.ids = ids; this.selected = selected; }
            public int getCount() { return ids.size(); }
            public Object getItem(int position) { return ids.get(position); }
            public long getItemId(int position) { return ids.get(position); }
            public View getView(int position, View convertView, ViewGroup parent) {
                CheckBox cb = convertView instanceof CheckBox ? (CheckBox) convertView : new CheckBox(ui);
                if (convertView == null) {
                    cb.setTextColor(FeedTheme.ink());
                    cb.setButtonTintList(android.content.res.ColorStateList.valueOf(FeedTheme.accent()));
                    cb.setClickable(false); cb.setFocusable(false); // row click owns the toggle
                    cb.setPadding(dp(8), dp(8), dp(8), dp(8));
                }
                int id = ids.get(position);
                cb.setText("#" + id + "  " + speciesName(id));
                cb.setChecked(selected.contains(id));
                return cb;
            }
        }

        private String speciesName(int id) {
            if (speciesNames == null) return "#" + id;
            String name = speciesNames.optString(Integer.toString(id), "");
            return name.isEmpty() ? "#" + id : name;
        }

        // ------------------------------------------------------------------ presets (Nearby only, local)

        private void openPresets() {
            final JSONObject presets = readPresets();
            final List<String> names = new ArrayList<>();
            for (java.util.Iterator<String> it = presets.keys(); it.hasNext(); ) names.add(it.next());
            java.util.Collections.sort(names);

            LinearLayout body = new LinearLayout(ui);
            body.setOrientation(LinearLayout.VERTICAL);
            int pad = dp(16);
            body.setPadding(pad, dp(8), pad, 0);

            final EditText name = new EditText(ui);
            name.setHint("Name this preset");
            name.setSingleLine(true);
            body.addView(name, new LinearLayout.LayoutParams(-1, dp(48)));
            Button save = button("Save current filters");
            body.addView(save, new LinearLayout.LayoutParams(-1, dp(44)));

            final LinearLayout listCol = new LinearLayout(ui);
            listCol.setOrientation(LinearLayout.VERTICAL);
            listCol.setPadding(0, dp(10), 0, 0);
            ScrollView sv = new ScrollView(ui);
            sv.addView(listCol);
            body.addView(sv, new LinearLayout.LayoutParams(-1, 0, 1));

            AlertDialog manager = new AlertDialog.Builder(ui)
                .setTitle("Filter presets")
                .setView(body)
                .setNegativeButton("Close", null)
                .create();
            if (sink != null) sink.track(manager);

            final Runnable[] rebuild = new Runnable[1];
            rebuild[0] = () -> {
                listCol.removeAllViews();
                if (names.isEmpty()) { listCol.addView(caption("No saved presets yet.")); }
                for (String n : new ArrayList<>(names)) {
                    // PGRazorX: name on its own line, then Apply / Edit / Rename / Delete.
                    LinearLayout row = new LinearLayout(ui);
                    row.setOrientation(LinearLayout.VERTICAL);
                    row.setPadding(0, dp(4), 0, dp(6));
                    TextView nameView = label(n, 14);
                    row.addView(nameView, new LinearLayout.LayoutParams(-1, -2));
                    LinearLayout actions = new LinearLayout(ui);
                    actions.setOrientation(LinearLayout.HORIZONTAL);
                    Button apply = button("Apply"), edit = button("Edit"), rename = button("Rename"), del = button("Delete");
                    for (Button b : new Button[]{apply, edit, rename, del}) actions.addView(b, new LinearLayout.LayoutParams(0, dp(40), 1));
                    row.addView(actions, new LinearLayout.LayoutParams(-1, -2));
                    listCol.addView(row, new LinearLayout.LayoutParams(-1, -2));
                    apply.setOnClickListener(v -> {
                        JSONObject p = presets.optJSONObject(n);
                        if (p != null) {
                            editingPreset = null;
                            copyAll(p, working); populate();
                            status.setText("Preset \"" + n + "\" loaded. Apply to use it.");
                        }
                        manager.dismiss();
                    });
                    edit.setOnClickListener(v -> {
                        JSONObject p = presets.optJSONObject(n);
                        if (p != null) {
                            editingPreset = n;
                            copyAll(p, working); populate();
                            status.setText("Editing preset \"" + n + "\". Change the filters, then tap Save to \"" + n + "\".");
                        }
                        manager.dismiss();
                    });
                    rename.setOnClickListener(v -> {
                        final EditText field = new EditText(ui);
                        field.setSingleLine(true); field.setText(n); field.setSelection(n.length());
                        AlertDialog ask = new AlertDialog.Builder(ui)
                            .setTitle("Rename preset")
                            .setView(field)
                            .setPositiveButton("Rename", null)
                            .setNegativeButton("Cancel", null)
                            .create();
                        if (sink != null) sink.track(ask);
                        ask.setOnShowListener(d2 -> {
                            FeedTheme.styleButtons(ask); FeedTheme.gameFooter(ask, null);
                            ask.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v2 -> {
                                String next = field.getText().toString().trim();
                                if (next.isEmpty() || next.length() > 40) { toast("Use a preset name of 1 to 40 characters."); return; }
                                if (next.equals(n)) { ask.dismiss(); return; }
                                if (presets.has(next)) { toast("A preset with that name already exists."); return; }
                                try {
                                    JSONObject value = presets.optJSONObject(n);
                                    if (value == null) { ask.dismiss(); return; }
                                    presets.remove(n); presets.put(next, value); writePresets(presets);
                                    names.remove(n); names.add(next); java.util.Collections.sort(names);
                                    if (n.equals(editingPreset)) { editingPreset = next; populate(); }
                                    rebuild[0].run(); ask.dismiss(); toast("Preset renamed.");
                                } catch (JSONException e) { toast("Couldn't rename that preset."); }
                            });
                        });
                        ask.show();
                    });
                    del.setOnClickListener(v -> {
                        presets.remove(n); names.remove(n); writePresets(presets);
                        if (n.equals(editingPreset)) { editingPreset = null; populate(); }
                        rebuild[0].run();
                    });
                    FeedTheme.style(row); FeedTheme.styleGameButtons(row);
                }
            };
            save.setOnClickListener(v -> {
                String n = name.getText().toString().trim();
                if (n.isEmpty() || n.length() > 40) { toast("Use a preset name of 1 to 40 characters."); return; }
                if (!presets.has(n) && presets.length() >= 25) { toast("You can save up to 25 presets."); return; }
                if (!savePreset(presets, n)) return;
                if (!names.contains(n)) names.add(n);
                java.util.Collections.sort(names);
                name.setText("");
                rebuild[0].run();
                toast("Preset saved.");
            });

            manager.setOnShowListener(d -> { FeedTheme.style(body); FeedTheme.styleGameButtons(body); FeedTheme.styleButtons(manager); FeedTheme.gameFooter(manager, null); rebuild[0].run(); });
            manager.show();
        }

        private JSONObject readPresets() {
            try {
                String s = prefs.getString("filter_presets_v1", null);
                return s != null ? new JSONObject(s) : new JSONObject();
            } catch (JSONException e) { return new JSONObject(); }
        }

        private void writePresets(JSONObject presets) {
            prefs.edit().putString("filter_presets_v1", presets.toString()).apply();
        }

        // ------------------------------------------------------------------ view helpers

        private View numberRow(String labelText, String key, int min, int max) {
            LinearLayout row = new LinearLayout(ui);
            row.setOrientation(LinearLayout.HORIZONTAL);
            row.setGravity(Gravity.CENTER_VERTICAL);
            row.addView(label(labelText, 14), new LinearLayout.LayoutParams(0, -2, 1));
            EditText field = new EditText(ui);
            field.setInputType(InputType.TYPE_CLASS_NUMBER);
            field.setSingleLine(true);
            field.setGravity(Gravity.CENTER);
            field.setText(Integer.toString(clamp(working.optInt(key, min), min, max)));
            row.addView(field, new LinearLayout.LayoutParams(dp(84), dp(48)));
            numbers.add(new NumField(key, field, min, max));
            return row;
        }

        private CheckBox checkbox(String labelText, String key) {
            CheckBox cb = new CheckBox(ui);
            cb.setText(labelText);
            cb.setTextSize(14);
            cb.setPadding(dp(4), dp(6), dp(4), dp(6));
            cb.setChecked(working.optBoolean(key, false));
            cb.setOnCheckedChangeListener((v, checked) -> put(key, checked));
            return cb;
        }

        private void header(String text) {
            TextView t = label(text, 13);
            t.setAllCaps(true);
            t.setTypeface(null, android.graphics.Typeface.BOLD);
            t.setTextColor(FeedTheme.accent());
            t.setPadding(0, dp(14), 0, dp(4));
            content.addView(t);
        }

        private TextView caption(String text) {
            TextView t = label(text, 12);
            t.setTextColor(FeedTheme.muted());
            t.setPadding(0, 0, 0, dp(4));
            return t;
        }

        private void highlight(Button b, boolean on) {
            FeedTheme.gameSelected(b, on);
        }

        private int dp(float v) { return Math.round(v * activity.getResources().getDisplayMetrics().density); }
        private TextView label(String text, int size) { TextView v = new TextView(ui); v.setText(text); v.setTextSize(size); return v; }
        private Button button(String text) { Button b = new Button(ui); b.setText(text); b.setAllCaps(false); b.setTextSize(13.5f); return b; }
        private Button segButton(String text) {
            Button b = new Button(ui); b.setText(text); b.setAllCaps(false); b.setTextSize(13);
            b.setPadding(dp(10), 0, dp(10), 0); return b;
        }
        private void toast(String message) { Toast.makeText(activity, message, Toast.LENGTH_SHORT).show(); }

        private void put(String key, Object value) {
            try { working.put(key, value); } catch (JSONException ignored) { }
        }
    }

    private static final class NumField {
        final String key; final EditText view; final int min, max;
        NumField(String key, EditText view, int min, int max) { this.key = key; this.view = view; this.min = min; this.max = max; }
    }

    /** The keys the My Shinies endpoint honours; Reset in shinies mode only touches these. */
    private static final String[] SHINIES_KEYS = {
        "species_filter_mode", "species_filter_list", "min_iv", "max_iv", "min_level", "max_level",
        "filter_xxs", "filter_xxl"
    };

    private static int clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

    private static void copyAll(JSONObject from, JSONObject to) {
        for (java.util.Iterator<String> it = from.keys(); it.hasNext(); ) {
            String k = it.next();
            try { to.put(k, from.get(k)); } catch (JSONException ignored) { }
        }
    }

    private static void copyKeys(JSONObject from, JSONObject to, String[] keys) {
        for (String k : keys) if (from.has(k)) {
            try { to.put(k, from.get(k)); } catch (JSONException ignored) { }
        }
    }
}
