package dev.pgrazor;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.graphics.drawable.ColorDrawable;
import android.text.Editable;
import android.text.InputType;
import android.text.TextWatcher;
import android.view.View;
import android.view.ViewGroup;
import android.widget.BaseAdapter;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.TextView;
import java.util.ArrayList;
import java.util.List;
import java.util.TreeSet;
import java.util.function.Consumer;
import java.util.function.Supplier;
import org.json.JSONObject;

/** Shared offline species picker. Filters only change the view; Save commits a detached selection. */
final class PokemonSpeciesDialog {
    private PokemonSpeciesDialog() {}

    /** Display range mirrors the known National Dex; selected ids beyond it are still shown and kept. */
    private static final int DISPLAY_MIN = 1, DISPLAY_MAX = 1025;
    private static final String NAMES_ASSET = "pgrazor/map-icons/pokemon-names.json";
    private static final int NAMES_LIMIT = 2 * 1024 * 1024;
    private static volatile JSONObject cachedNames;

    /** Worker-thread read, shared with settings summaries. Root uses the module's bundled assets. */
    static JSONObject readNames(Context context) {
        if (cachedNames != null) return cachedNames;
        try (java.io.InputStream in = PgAssets.open(context, NAMES_ASSET)) {
            java.io.ByteArrayOutputStream bytes = new java.io.ByteArrayOutputStream();
            byte[] buffer = new byte[4096]; int read;
            while ((read = in.read(buffer)) != -1) {
                if (bytes.size() + read > NAMES_LIMIT) return null;
                bytes.write(buffer, 0, read);
            }
            return cachedNames = new JSONObject(bytes.toString("UTF-8"));
        } catch (Exception ignored) { return null; }
    }

    static void show(Activity activity, String title, String noun, int max, Supplier<String> load,
                     Consumer<String> save, Runnable onSaved, String emptyLabel, Consumer<AlertDialog> track) {
        if (activity == null || activity.isFinishing() || activity.isDestroyed()) return;
        new Picker(activity, title, noun, max, load, save, onSaved, emptyLabel, false, track).open();
    }

    static void showRaid(Activity activity, String title, String noun, int max, Supplier<String> load,
                         Consumer<String> save, Runnable onSaved, String emptyLabel) {
        if (activity == null || activity.isFinishing() || activity.isDestroyed()) return;
        new Picker(activity, title, noun, max, load, save, onSaved, emptyLabel, true, null).open();
    }

    private static String categoryHelp(int category, boolean raid) {
        if (category == PokemonCategories.STAGE_2 || category == PokemonCategories.STAGE_3)
            return "Stages count from the first species, including babies. Stage 2: evolved once; Stage 3: evolved twice.";
        return raid ? "Groups can include unreleased forms, not today's raid lineup. Use Raid tiers to require Mega/Primal raids."
            : "Choose a group, then select Pokemon individually or use Select shown. Groups can include unreleased forms.";
    }

    private static final class Picker {
        private final Activity activity;
        private final Context ui;
        private final Runnable onSaved;
        private final String title, noun, emptyLabel;
        private final boolean raid;
        private final java.util.function.Consumer<AlertDialog> track;
        private final int max;
        private final java.util.function.Supplier<String> load;
        private final java.util.function.Consumer<String> save;
        private final TreeSet<Integer> selected = new TreeSet<>();
        private final List<Integer> all = new ArrayList<>();
        private final List<Integer> shown = new ArrayList<>();
        private volatile JSONObject names;   // dex-id string -> name; filled off the main thread
        private Adapter adapter;
        private TextView count;
        private android.widget.Button categoryButton;
        private TextView categoryHelp;
        private int category = PokemonCategories.ALL;   // view-only filter; never persisted
        private EditText search;
        private AlertDialog dialog, categoryDialog;
        private boolean closed;

        Picker(Activity activity, String title, String noun, int max, java.util.function.Supplier<String> load,
               java.util.function.Consumer<String> save, Runnable onSaved, String emptyLabel, boolean raid, java.util.function.Consumer<AlertDialog> track) {
            this.activity = activity; this.ui = MapTheme.wrap(activity); this.onSaved = onSaved;
            this.title = title; this.noun = noun; this.max = max; this.load = load; this.save = save;
            this.emptyLabel = emptyLabel; this.raid = raid; this.track = track;
        }

        private int dp(float v) { return MapTheme.dp(ui, v); }

        void open() {
            // Seed from the saved canonical list (already validated and bounded by Control).
            String saved = load.get();
            if (!saved.isEmpty()) for (String token : saved.split(",")) {
                try { int id = Integer.parseInt(token.trim()); if (id >= 1 && id <= 2000) selected.add(id); }
                catch (NumberFormatException ignored) {}
            }
            // Displayed ids: the dex range plus any saved selection beyond it, so nothing is lost.
            TreeSet<Integer> ids = new TreeSet<>();
            for (int id = DISPLAY_MIN; id <= DISPLAY_MAX; id++) ids.add(id);
            ids.addAll(selected);
            all.addAll(ids); shown.addAll(ids);

            LinearLayout body = new LinearLayout(ui); body.setOrientation(LinearLayout.VERTICAL);
            int pad = dp(14); body.setPadding(pad, dp(8), pad, 0);

            categoryButton = new android.widget.Button(ui); categoryButton.setAllCaps(false);
            categoryButton.setText("Category: " + PokemonCategories.LABELS[category]);
            categoryButton.setOnClickListener(v -> openCategoryPicker());
            body.addView(categoryButton);
            TextView help = new TextView(ui); help.setTextSize(12);
            categoryHelp = help;
            help.setText(categoryHelp(category, raid));
            body.addView(help);

            search = new EditText(ui); search.setHint("Search name or #dex"); search.setSingleLine(true);
            search.setInputType(InputType.TYPE_CLASS_TEXT);
            body.addView(search, new LinearLayout.LayoutParams(-1, dp(48)));
            count = new TextView(ui); count.setTextSize(12); count.setPadding(0, dp(4), 0, dp(4)); body.addView(count);

            LinearLayout bulk = new LinearLayout(ui); bulk.setOrientation(LinearLayout.HORIZONTAL);
            android.widget.Button selectBtn = new android.widget.Button(ui); selectBtn.setAllCaps(false);
            selectBtn.setText("Select shown"); selectBtn.setOnClickListener(v -> selectShown());
            android.widget.Button clearBtn = new android.widget.Button(ui); clearBtn.setAllCaps(false);
            clearBtn.setText("Clear shown"); clearBtn.setOnClickListener(v -> clearShown());
            bulk.addView(selectBtn, new LinearLayout.LayoutParams(0, -2, 1f));
            bulk.addView(clearBtn, new LinearLayout.LayoutParams(0, -2, 1f));
            body.addView(bulk);

            ListView list = new ListView(ui);
            adapter = new Adapter();
            list.setAdapter(adapter);
            list.setOnItemClickListener((parent, view, position, rowId) -> {
                int id = shown.get(position);
                if (!selected.remove(id)) {
                    if(selected.size()>=max){android.widget.Toast.makeText(activity,"Choose at most "+max+" "+noun+".",android.widget.Toast.LENGTH_SHORT).show();return;}
                    selected.add(id);
                }
                onSelectionChanged();
            });
            // Leave room for the category button, help and bulk row so the list and keyboard still fit.
            int reserve = dp(400);
            int cap = dp(340);
            int listHeight = Math.max(dp(120), Math.min(cap,
                activity.getResources().getDisplayMetrics().heightPixels - reserve));
            body.addView(list, new LinearLayout.LayoutParams(-1, listHeight));
            updateCount();

            search.addTextChangedListener(new TextWatcher() {
                public void beforeTextChanged(CharSequence s, int a, int b, int c) {}
                public void onTextChanged(CharSequence s, int a, int b, int c) {}
                public void afterTextChanged(Editable e) { refreshShown(); }
            });

            dialog = new AlertDialog.Builder(ui)
                .setTitle(title)
                .setView(body)
                .setPositiveButton("Save", (d, w) -> commit())
                .setNeutralButton("Clear all", null)   // click overridden below so it does not dismiss
                .setNegativeButton("Cancel", null)
                .create();
            DialogLifecycle.bind(activity, dialog, () -> {
                closed = true;
                if (categoryDialog != null) categoryDialog.dismiss();
            });
            if (track != null) track.accept(dialog);
            dialog.setOnShowListener(d -> {
                MapTheme.style(body); MapTheme.styleButtons(dialog);
                dialog.getWindow().setBackgroundDrawable(new ColorDrawable(MapTheme.surface()));
                // Clear only empties the selection; the user can still Save or Cancel afterwards.
                dialog.getButton(AlertDialog.BUTTON_NEUTRAL).setOnClickListener(v -> {
                    selected.clear(); onSelectionChanged();
                });
            });
            dialog.show();
            loadNames();
        }

        /** Recomputes the visible rows as the chosen category intersected with the search text. */
        private void refreshShown() {
            String query = search == null ? "" : search.getText().toString();
            shown.clear();
            for (int id : all) {
                if (!PokemonCategories.matches(category, id, selected.contains(id))) continue;
                if (SmartCatchPreferences.matchesSpecies(id, name(id), query)) shown.add(id);
            }
            adapter.notifyDataSetChanged();
            updateCount();
        }

        /** Repaints after a selection change; the Selected view's membership is recomputed as it changes. */
        private void onSelectionChanged() {
            if (category == PokemonCategories.SELECTED) { refreshShown(); return; }
            adapter.notifyDataSetChanged();
            updateCount();
        }

        /** Adds every visible row all-or-nothing: if the union would pass the cap, nothing is added. */
        private void selectShown() {
            TreeSet<Integer> union = new TreeSet<>(selected); union.addAll(shown);
            if (union.size() > max) {
                android.widget.Toast.makeText(activity, "That would exceed " + max + " " + noun
                    + "; narrow the list first.", android.widget.Toast.LENGTH_SHORT).show();
                return;
            }
            selected.clear(); selected.addAll(union);
            onSelectionChanged();
        }

        /** Removes only the visible rows, leaving any selection the category or search hides intact. */
        private void clearShown() {
            if (selected.removeAll(shown)) onSelectionChanged();
        }

        private void openCategoryPicker() {
            AlertDialog d = new AlertDialog.Builder(ui).setTitle("Browse by category")
                .setNegativeButton("Cancel", null).create();
            ChoiceList.attach(d, PokemonCategories.LABELS, index -> {
                category = index;
                categoryButton.setText("Category: " + PokemonCategories.LABELS[category]);
                categoryHelp.setText(categoryHelp(category, raid));
                refreshShown();
            });
            categoryDialog = d;
            DialogLifecycle.bind(activity, d, () -> { if (categoryDialog == d) categoryDialog = null; });
            if (track != null) track.accept(d);
            d.setOnShowListener(x -> { MapTheme.styleButtons(d);
                d.getWindow().setBackgroundDrawable(new ColorDrawable(MapTheme.surface())); });
            d.show();
        }

        private void updateCount() {
            String results = shown.isEmpty() ? "No matches" : shown.size() + " shown";
            String picked = selected.isEmpty() && emptyLabel != null ? emptyLabel
                : selected.size() + "/" + max + " " + noun + " selected";
            count.setText(results + " \u00b7 " + picked);
        }

        private void commit() {
            StringBuilder sb = new StringBuilder();
            for (int id : selected) { if (sb.length() > 0) sb.append(','); sb.append(id); }
            save.accept(sb.toString());
            if (onSaved != null) onSaved.run();
        }

        /** Reads the bundled name table off the main thread; labels and name search refresh on arrival. */
        private void loadNames() {
            new Thread(() -> {
                final JSONObject result = readNames(activity);
                activity.runOnUiThread(() -> {
                    if (closed || activity.isFinishing() || activity.isDestroyed() || result == null) return;
                    names = result;
                    refreshShown(); // name search and category labels now resolve
                });
            }, "pgrazor-species-names").start();
        }

        private String name(int id) {
            JSONObject table = names;
            return table == null ? "" : table.optString(Integer.toString(id), "");
        }

        private final class Adapter extends BaseAdapter {
            public int getCount() { return shown.size(); }
            public Object getItem(int position) { return shown.get(position); }
            public long getItemId(int position) { return shown.get(position); }
            public View getView(int position, View convertView, ViewGroup parent) {
                CheckBox cb = convertView instanceof CheckBox ? (CheckBox) convertView : new CheckBox(ui);
                if (convertView == null) {
                    cb.setTextColor(MapTheme.ink());
                    cb.setButtonTintList(android.content.res.ColorStateList.valueOf(MapTheme.accent()));
                    cb.setClickable(false); cb.setFocusable(false); // the row click owns the toggle
                    cb.setMinHeight(dp(48)); cb.setPadding(dp(8), dp(8), dp(8), dp(8));
                }
                int id = shown.get(position);
                String name = name(id);
                cb.setText(name.isEmpty() ? ("#" + id) : (name + "  (#" + id + ")"));
                cb.setChecked(selected.contains(id));
                return cb;
            }
        }
    }
}
