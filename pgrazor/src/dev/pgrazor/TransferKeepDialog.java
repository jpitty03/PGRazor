package dev.pgrazor;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
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
import java.util.Locale;
import java.util.TreeSet;
import java.util.function.Consumer;
import java.util.function.Supplier;
import org.json.JSONObject;

/**
 * Searchable species picker reused for any canonical comma-separated dex-id list. The custom
 * transfer keep list is one such list: kept species are never auto-transferred, and the picker edits
 * a local selection set and commits the canonical string via {@link Control#setTransferKeepSpecies}
 * on Save, leaving saved settings untouched on Cancel. The generalized {@link #show(Activity, String,
 * String, Supplier, Consumer, Runnable)} entry point drives any other list (for example the
 * evolve-before-transfer selected species) through its own seed/commit pair without changing or
 * sharing the keep list's saved value.
 *
 * <p>Rows are drawn by a recycling {@link BaseAdapter} over a bounded id list, so typing in the
 * search box only rebuilds a small list of integers and never instantiates a view per species. The
 * selection is kept separate from the filtered view, so species stay selected while the search
 * narrows the list, and any saved id without a bundled name (or beyond the displayed dex range) is
 * still shown checked and preserved on Save. The dialog never touches the network.
 */
public final class TransferKeepDialog {
    private TransferKeepDialog() {}

    /** Display range mirrors the known National Dex; selected ids beyond it are still shown and kept. */
    private static final int DISPLAY_MIN = 1, DISPLAY_MAX = 1025;
    private static final String NAMES_ASSET = "pgrazor/map-icons/pokemon-names.json";
    private static final int NAMES_LIMIT = 2 * 1024 * 1024;

    /** Opens the custom transfer keep list (unchanged behavior and saved value). */
    public static void show(Activity activity, Runnable onSaved) {
        show(activity, "Custom keep list", "species kept",
            Control::transferKeepSpecies, Control::setTransferKeepSpecies, onSaved);
    }

    /**
     * Opens the picker for an arbitrary list. {@code seed} supplies the saved canonical string to
     * pre-select; {@code commit} receives the new canonical comma-separated string on Save. The keep
     * list is only touched when its own entry point is used.
     */
    public static void show(Activity activity, String title, String countNoun,
            Supplier<String> seed, Consumer<String> commit, Runnable onSaved) {
        if (activity == null || activity.isFinishing() || activity.isDestroyed()) return;
        new Picker(activity, title, countNoun, seed, commit, onSaved).open();
    }

    private static final class Picker {
        private final Activity activity;
        private final Context ui;
        private final String title;
        private final String countNoun;
        private final Supplier<String> seed;
        private final Consumer<String> commit;
        private final Runnable onSaved;
        private final TreeSet<Integer> selected = new TreeSet<>();
        private final List<Integer> all = new ArrayList<>();
        private final List<Integer> shown = new ArrayList<>();
        private volatile JSONObject names;   // dex-id string -> name; filled off the main thread
        private Adapter adapter;
        private TextView count;
        private EditText search;
        private AlertDialog dialog;
        private boolean closed;

        Picker(Activity activity, String title, String countNoun,
                Supplier<String> seed, Consumer<String> commit, Runnable onSaved) {
            this.activity = activity; this.ui = MapTheme.wrap(activity);
            this.title = title; this.countNoun = countNoun;
            this.seed = seed; this.commit = commit; this.onSaved = onSaved;
        }

        private int dp(float v) { return MapTheme.dp(ui, v); }

        void open() {
            // Seed from the saved canonical list (already validated and bounded by Control).
            String saved = seed.get();
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
            search = new EditText(ui); search.setHint("Search name or #dex"); search.setSingleLine(true);
            search.setInputType(InputType.TYPE_CLASS_TEXT);
            body.addView(search, new LinearLayout.LayoutParams(-1, dp(48)));
            count = new TextView(ui); count.setTextSize(12); count.setPadding(0, dp(4), 0, dp(4)); body.addView(count);
            ListView list = new ListView(ui);
            adapter = new Adapter();
            list.setAdapter(adapter);
            list.setOnItemClickListener((parent, view, position, rowId) -> {
                int id = shown.get(position);
                if (!selected.remove(id)) selected.add(id);
                adapter.notifyDataSetChanged(); updateCount();
            });
            int listHeight = Math.max(dp(120), Math.min(dp(420),
                activity.getResources().getDisplayMetrics().heightPixels - dp(260)));
            body.addView(list, new LinearLayout.LayoutParams(-1, listHeight));
            updateCount();

            search.addTextChangedListener(new TextWatcher() {
                public void beforeTextChanged(CharSequence s, int a, int b, int c) {}
                public void onTextChanged(CharSequence s, int a, int b, int c) {}
                public void afterTextChanged(Editable e) { applyFilter(e.toString()); }
            });

            dialog = new AlertDialog.Builder(ui)
                .setTitle(title)
                .setView(body)
                .setPositiveButton("Save", (d, w) -> commit())
                .setNeutralButton("Clear all", null)   // click overridden below so it does not dismiss
                .setNegativeButton("Cancel", null)
                .create();
            dialog.setOnDismissListener(d -> closed = true);
            dialog.setOnShowListener(d -> {
                MapTheme.style(body); MapTheme.styleButtons(dialog);
                // Clear only empties the selection; the user can still Save or Cancel afterwards.
                dialog.getButton(AlertDialog.BUTTON_NEUTRAL).setOnClickListener(v -> {
                    selected.clear(); adapter.notifyDataSetChanged(); updateCount();
                });
            });
            dialog.show();
            loadNames();
        }

        private void applyFilter(String query) {
            String q = query.trim().toLowerCase(Locale.US);
            if (q.startsWith("#")) q = q.substring(1).trim();
            shown.clear();
            for (int id : all) {
                if (q.isEmpty()) { shown.add(id); continue; }
                if (Integer.toString(id).contains(q)) { shown.add(id); continue; }
                String name = name(id);
                if (!name.isEmpty() && name.toLowerCase(Locale.US).contains(q)) shown.add(id);
            }
            adapter.notifyDataSetChanged();
        }

        private void updateCount() { count.setText(selected.size() + " " + countNoun); }

        private void commit() {
            StringBuilder sb = new StringBuilder();
            for (int id : selected) { if (sb.length() > 0) sb.append(','); sb.append(id); }
            commit.accept(sb.toString());
            if (onSaved != null) onSaved.run();
        }

        /** Reads the bundled name table off the main thread; labels and name search refresh on arrival. */
        private void loadNames() {
            new Thread(() -> {
                JSONObject parsed = null;
                try (java.io.InputStream in = activity.getAssets().open(NAMES_ASSET)) {
                    java.io.ByteArrayOutputStream bytes = new java.io.ByteArrayOutputStream();
                    byte[] buffer = new byte[4096]; int read; boolean oversize = false;
                    while ((read = in.read(buffer)) != -1) {
                        if (bytes.size() + read > NAMES_LIMIT) { oversize = true; break; }
                        bytes.write(buffer, 0, read);
                    }
                    if (!oversize) parsed = new JSONObject(new String(bytes.toByteArray(), java.nio.charset.StandardCharsets.UTF_8));
                } catch (Exception ignored) {}
                final JSONObject result = parsed;
                activity.runOnUiThread(() -> {
                    if (closed || activity.isFinishing() || activity.isDestroyed() || result == null) return;
                    names = result;
                    applyFilter(search == null ? "" : search.getText().toString()); // name search now resolves
                });
            }, "pgrazor-keep-names").start();
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
                cb.setText(name.isEmpty() ? ("#" + id) : ("#" + id + "  " + name));
                cb.setChecked(selected.contains(id));
                return cb;
            }
        }
    }
}
