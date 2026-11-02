package dev.pgrazor;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.text.Editable;
import android.text.TextWatcher;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Switch;
import android.widget.TextView;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

/**
 * PGRazorX: the Filters dialog of the non-Pokémon search modes (Raids, Quests, Weather, Rocket, Power
 * Spots). It shows the same choices as the site's search page for that mode and edits a copy of the
 * mode's spec; Apply hands the copy back. Option lists that come from the site (raid bosses, quest
 * rewards and tasks, Rocket types, Power Spot bosses) are fetched by the owner and cached here.
 */
final class LiveFeedModeFilterDialog {
    private LiveFeedModeFilterDialog() {}

    interface Sink { void track(AlertDialog dialog); }
    interface Apply { void apply(JSONObject spec); }

    private static final Map<String, JSONObject> CATALOGS = new HashMap<>();

    /** The site endpoint with this mode's option catalog, or null when the options are fixed. */
    static String catalogPath(LiveFeedModes.Mode m) {
        switch (m) {
            case RAIDS: return "/raids/pokemons";
            case QUESTS: return "/quests/catalog-pairs";
            case ROCKET: return "/rocket/types";
            case POWER_SPOTS: return "/powerspots/pokemons";
            default: return null;
        }
    }
    static JSONObject cached(String path) { synchronized (CATALOGS) { return CATALOGS.get(path); } }
    static void cache(String path, JSONObject data) { synchronized (CATALOGS) { CATALOGS.put(path, data); } }

    static void show(Activity activity, Context ui, LiveFeedModes.Mode mode, JSONObject working, Sink sink, Apply onApply) {
        if (activity == null || activity.isFinishing() || activity.isDestroyed() || working == null) return;
        Builder b = new Builder(ui, working);
        ScrollView scroll = new ScrollView(ui);
        scroll.setFillViewport(false);
        scroll.addView(b.body);
        String path = catalogPath(mode);
        JSONObject catalog = path == null ? null : cached(path);
        switch (mode) {
            case RAIDS: b.raids(catalog); break;
            case QUESTS: b.quests(catalog); break;
            case WEATHER: b.weather(); break;
            case ROCKET: b.rocket(catalog); break;
            case POWER_SPOTS: b.spots(catalog); break;
            default: return;
        }
        AlertDialog dialog = new AlertDialog.Builder(ui)
            .setTitle(mode.filtersTitle)
            .setView(scroll)
            .setPositiveButton("Apply", (d, w) -> onApply.apply(working))
            .setNegativeButton("Cancel", null)
            .create();
        if (sink != null) sink.track(dialog);
        dialog.setOnShowListener(d -> {
            MapTheme.style(scroll); MapTheme.styleGameButtons(scroll);
            b.refresh();
            MapTheme.styleButtons(dialog); MapTheme.gameFooter(dialog, null);
        });
        dialog.show();
    }

    /** Builds the controls and keeps every selectable button's look in sync with the spec. */
    private static final class Builder {
        final Context ui;
        final JSONObject spec;
        final LinearLayout body;
        final List<Runnable> refreshers = new ArrayList<>();

        Builder(Context ui, JSONObject spec) {
            this.ui = ui; this.spec = spec;
            body = new LinearLayout(ui);
            body.setOrientation(LinearLayout.VERTICAL);
            body.setPadding(dp(4), dp(2), dp(4), dp(8));
        }

        int dp(float v) { return Math.round(v * ui.getResources().getDisplayMetrics().density); }

        void refresh() { for (Runnable r : new ArrayList<>(refreshers)) r.run(); }

        // ------------------------------------------------------------ modes

        void raids(JSONObject catalog) {
            final LinearLayout bosses = new LinearLayout(ui), tiers = new LinearLayout(ui);
            bosses.setOrientation(LinearLayout.VERTICAL); tiers.setOrientation(LinearLayout.VERTICAL);
            section("Filter by");
            body.addView(pills(new String[]{"Pokémon", "Tier"}, new String[]{"pokemon", "tier"}, "filter_by", () -> {
                boolean byTier = "tier".equals(spec.optString("filter_by", "tier"));
                bosses.setVisibility(byTier ? View.GONE : View.VISIBLE);
                tiers.setVisibility(byTier ? View.VISIBLE : View.GONE);
            }));
            body.addView(bosses); body.addView(tiers);
            // Bosses: the site's current raid bosses, each with its form and variant (normal/mega/shadow).
            sectionIn(bosses, "Raid bosses");
            List<String[]> options = new ArrayList<>();
            JSONArray list = catalog == null ? null : catalog.optJSONArray("pokemon");
            if (list != null) for (int i = 0; i < list.length(); i++) {
                JSONObject p = list.optJSONObject(i);
                if (p == null) continue;
                String form = p.isNull("form_id") ? "" : String.valueOf(p.optInt("form_id"));
                options.add(new String[]{p.optString("name", "#" + p.optInt("id")), p.optInt("id") + "|" + form + "|" + p.optString("variant", "any")});
            }
            if (options.isEmpty()) bosses.addView(note("Couldn't load the raid bosses. Use Tier, or try again later."));
            else bosses.addView(searchableChips(options, new Selector() {
                public boolean on(String key) { return bossIndex(key) >= 0; }
                public void toggle(String key) {
                    JSONArray a = LiveFeedModes.arr(spec, "bosses"), out = new JSONArray();
                    int at = bossIndex(key);
                    for (int i = 0; i < a.length(); i++) if (i != at) out.put(a.opt(i));
                    if (at < 0) {
                        String[] k = key.split("\\|", -1);
                        try {
                            JSONObject boss = new JSONObject();
                            boss.put("pokemon_id", Integer.parseInt(k[0]));
                            boss.put("form_id", k[1].isEmpty() ? JSONObject.NULL : (Object) Integer.parseInt(k[1]));
                            boss.put("variant", k[2]);
                            out.put(boss);
                        } catch (Exception ignored) { }
                    }
                    put("bosses", out);
                }
            }, "Search bosses"));
            sectionIn(tiers, "Tier");
            List<String[]> tierOptions = new ArrayList<>();
            for (int i = 0; i < LiveFeedModes.RAID_TIERS.length; i++) tierOptions.add(new String[]{LiveFeedModes.RAID_TIERS[i], String.valueOf(i + 1)});
            tiers.addView(chips(tierOptions, multi("tiers", true)));
            section("Type");
            body.addView(pills(LiveFeedModes.EX_LABELS, LiveFeedModes.EX_MODES, "exclusive_mode", null));
            section("Status");
            body.addView(pills(LiveFeedModes.RAID_STATUS_LABELS, LiveFeedModes.RAID_STATUS, "status", null));
        }

        /** Index of the selected boss matching "id|form|variant", or -1. */
        int bossIndex(String key) {
            JSONArray a = LiveFeedModes.arr(spec, "bosses");
            for (int i = 0; i < a.length(); i++) {
                JSONObject o = a.optJSONObject(i);
                if (o == null) continue;
                String form = o.isNull("form_id") ? "" : String.valueOf(o.optInt("form_id"));
                if ((o.optInt("pokemon_id") + "|" + form + "|" + o.optString("variant", "any")).equals(key)) return i;
            }
            return -1;
        }

        void quests(JSONObject catalog) {
            final LinearLayout dependent = new LinearLayout(ui);
            dependent.setOrientation(LinearLayout.VERTICAL);
            section("Reward type");
            List<String[]> types = new ArrayList<>();
            for (String t : LiveFeedModes.REWARD_TYPES) types.add(new String[]{t, t});
            body.addView(chips(types, new Selector() {
                public boolean on(String key) { return key.equals(spec.optString("reward_type", "")); }
                public void toggle(String key) {
                    if (on(key)) return;
                    put("reward_type", key);
                    for (String k : LiveFeedModes.QUEST_LISTS) put(k, new JSONArray()); // the site clears them too
                    rebuildQuestOptions(dependent, catalog);
                }
            }));
            body.addView(dependent);
            rebuildQuestOptions(dependent, catalog);
        }

        void rebuildQuestOptions(LinearLayout box, JSONObject catalog) {
            // Drop the refreshers of the views being replaced; keep the others.
            refreshers.removeIf(r -> r instanceof BoxRefresher && ((BoxRefresher) r).box == box);
            box.removeAllViews();
            final int start = refreshers.size();
            String type = spec.optString("reward_type", "");
            String listKey = null, specKey = null, label = null, idKey = "value", nameKey = "label", search = null;
            switch (type) {
                case "Pokemon": listKey = "reward_pokemon"; specKey = "reward_pokemon_ids"; label = "Reward Pokémon"; idKey = "id"; nameKey = "name"; search = "Search Pokémon"; break;
                case "Mega Energy": listKey = "mega_pokemon"; specKey = "reward_mega_pokemon_ids"; label = "Mega Energy Pokémon"; idKey = "id"; nameKey = "name"; search = "Search Pokémon"; break;
                case "Item": listKey = "items"; specKey = "reward_item_ids"; label = "Items"; search = "Search items"; break;
                case "Stardust": listKey = "stardust_amounts"; specKey = "reward_stardust_amounts"; label = "Stardust amount"; break;
                case "PokéCoin": listKey = "pokecoin_amounts"; specKey = "reward_pokecoin_amounts"; label = "PokéCoin amount"; break;
                default: break;
            }
            if (listKey != null) {
                sectionIn(box, label + " (none = any)");
                List<String[]> options = options(catalog == null ? null : catalog.optJSONArray(listKey), idKey, nameKey);
                if (options.isEmpty()) box.addView(note(catalog == null ? "Couldn't load the options. Any reward is searched." : "No options listed right now."));
                else box.addView(search != null ? searchableChips(options, multi(specKey, true), search) : chips(options, multi(specKey, true)));
            }
            JSONObject tasks = catalog == null ? null : catalog.optJSONObject("tasks");
            List<String[]> taskOptions = options(tasks == null ? null : tasks.optJSONArray(type), "value", "label");
            if (!taskOptions.isEmpty()) {
                sectionIn(box, "Task (none = any)");
                box.addView(searchableChips(taskOptions, multi("quest_titles", false), "Search tasks"));
            }
            // Refreshers added while building this box are tagged so a later rebuild can drop them.
            for (int i = start; i < refreshers.size(); i++) refreshers.set(i, new BoxRefresher(box, refreshers.get(i)));
            MapTheme.style(box); MapTheme.styleGameButtons(box);
            refresh();
        }

        void weather() {
            section("Weather");
            List<String[]> options = new ArrayList<>();
            for (int i = 0; i < LiveFeedModes.WEATHERS.length; i++) options.add(new String[]{LiveFeedModes.WEATHERS[i], String.valueOf(i + 1)});
            body.addView(chips(options, new Selector() {
                public boolean on(String key) { return !spec.isNull("weather_id") && String.valueOf(spec.optInt("weather_id")).equals(key); }
                public void toggle(String key) { put("weather_id", Integer.parseInt(key)); }
            }));
            body.addView(note("Results are areas with that weather right now."));
        }

        void rocket(JSONObject catalog) {
            List<String[]> types = new ArrayList<>();
            JSONArray list = catalog == null ? null : catalog.optJSONArray("types");
            if (list != null) for (int i = 0; i < list.length(); i++) {
                JSONObject t = list.optJSONObject(i);
                if (t != null && !t.optString("id", "").isEmpty()) types.add(new String[]{t.optString("id"), t.optString("label", t.optString("id")), t.optString("group", "type")});
            }
            if (types.isEmpty()) for (String[] t : LiveFeedModes.ROCKET_TYPES) types.add(t);
            Selector single = new Selector() {
                public boolean on(String key) { return key.equals(spec.optString("type", "")) && !spec.isNull("type"); }
                public void toggle(String key) { put("type", key); }
            };
            String[][] groups = {{"special", "Special"}, {"type", "Pokémon types"}, {"leader", "Leaders"}};
            for (String[] g : groups) {
                List<String[]> options = new ArrayList<>();
                for (String[] t : types) if (g[0].equals(t[2])) options.add(new String[]{t[1], t[0]});
                if (options.isEmpty()) continue;
                section(g[1]);
                body.addView(chips(options, single));
            }
        }

        void spots(JSONObject catalog) {
            section("Pokémon (none = any)");
            List<String[]> options = options(catalog == null ? null : catalog.optJSONArray("pokemon"), "id", "name");
            if (options.isEmpty()) body.addView(note("Couldn't load the Power Spot bosses. Any boss is searched."));
            else body.addView(searchableChips(options, multi("pokemon_ids", true), "Search Pokémon"));
            section("Level (none = any)");
            List<String[]> levels = new ArrayList<>();
            for (int i = 0; i < LiveFeedModes.SPOT_LEVELS.length; i++) levels.add(new String[]{LiveFeedModes.SPOT_LEVELS[i], String.valueOf(i + 1)});
            body.addView(chips(levels, multi("battle_levels", true)));
            Switch active = new Switch(ui);
            active.setText("Active battles only");
            active.setChecked(spec.optBoolean("active_only", false));
            active.setOnCheckedChangeListener((v, on) -> put("active_only", on));
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, -2);
            lp.topMargin = dp(10);
            body.addView(active, lp);
        }

        // ------------------------------------------------------------ building blocks

        interface Selector { boolean on(String key); void toggle(String key); }

        /** Multi-select over a JSON array of numbers ({@code numeric}) or strings. */
        Selector multi(String key, boolean numeric) {
            return new Selector() {
                public boolean on(String k) {
                    JSONArray a = LiveFeedModes.arr(spec, key);
                    for (int i = 0; i < a.length(); i++) if (String.valueOf(a.opt(i)).equals(k)) return true;
                    return false;
                }
                public void toggle(String k) {
                    JSONArray a = LiveFeedModes.arr(spec, key), out = new JSONArray();
                    boolean had = false;
                    for (int i = 0; i < a.length(); i++) { if (String.valueOf(a.opt(i)).equals(k)) had = true; else out.put(a.opt(i)); }
                    if (!had) {
                        if (numeric) { try { out.put(Long.parseLong(k)); } catch (NumberFormatException e) { out.put(k); } }
                        else out.put(k);
                    }
                    put(key, out);
                }
            };
        }

        void put(String key, Object value) {
            try { spec.put(key, value); } catch (JSONException ignored) { }
        }

        static List<String[]> options(JSONArray list, String idKey, String nameKey) {
            List<String[]> out = new ArrayList<>();
            if (list == null) return out;
            for (int i = 0; i < list.length(); i++) {
                JSONObject o = list.optJSONObject(i);
                if (o == null || !o.has(idKey) || o.isNull(idKey)) continue;
                out.add(new String[]{o.optString(nameKey, String.valueOf(o.opt(idKey))), String.valueOf(o.opt(idKey))});
            }
            return out;
        }

        void section(String title) { sectionIn(body, title); }

        void sectionIn(LinearLayout parent, String title) {
            TextView t = new TextView(ui);
            t.setText(title); t.setTextSize(13.5f); t.setTypeface(MapTheme.medium());
            t.setPadding(dp(2), dp(12), 0, dp(6));
            parent.addView(t);
        }

        TextView note(String text) {
            TextView t = new TextView(ui);
            t.setText(text); t.setTextSize(12.5f); t.setTextColor(MapTheme.muted());
            t.setPadding(dp(2), dp(4), 0, dp(2));
            return t;
        }

        /** Equal-width single-choice pills bound to a string key (Filter by, Type, Status). */
        LinearLayout pills(String[] labels, String[] values, String key, Runnable onChange) {
            LinearLayout row = new LinearLayout(ui);
            row.setOrientation(LinearLayout.HORIZONTAL);
            final Button[] buttons = new Button[labels.length];
            Runnable sync = () -> {
                String cur = spec.optString(key, values[0]);
                for (int i = 0; i < buttons.length; i++) MapTheme.gameSelected(buttons[i], values[i].equals(cur));
                if (onChange != null) onChange.run();
            };
            for (int i = 0; i < labels.length; i++) {
                final String v = values[i];
                Button b = chip(labels[i]);
                b.setOnClickListener(x -> { put(key, v); sync.run(); });
                row.addView(b, new LinearLayout.LayoutParams(0, dp(40), 1));
                buttons[i] = b;
            }
            register(sync);
            return row;
        }

        /** Wrapping chips; each option is {label, key}. */
        View chips(List<String[]> options, Selector sel) {
            FlowLayout flow = new FlowLayout(ui, dp(6), dp(6));
            final List<Button> buttons = new ArrayList<>();
            final List<String> keys = new ArrayList<>();
            Runnable sync = () -> { for (int i = 0; i < buttons.size(); i++) MapTheme.gameSelected(buttons.get(i), sel.on(keys.get(i))); };
            for (String[] o : options) {
                Button b = chip(o[0]);
                final String key = o[1];
                b.setOnClickListener(x -> { sel.toggle(key); refresh(); });
                flow.addView(b, new ViewGroup.LayoutParams(-2, dp(34)));
                buttons.add(b); keys.add(key);
            }
            register(sync);
            return flow;
        }

        /** Chips inside a mint block with a search box; selected ones always stay visible. */
        View searchableChips(List<String[]> options, Selector sel, String hint) {
            LinearLayout box = new LinearLayout(ui);
            box.setOrientation(LinearLayout.VERTICAL);
            box.setBackground(MapTheme.mintBlock(ui));
            box.setPadding(dp(8), dp(8), dp(8), dp(8));
            EditText search = new EditText(ui);
            search.setHint(hint); search.setSingleLine(true); search.setTextSize(13.5f);
            if (android.os.Build.VERSION.SDK_INT >= 26) search.setImportantForAutofill(View.IMPORTANT_FOR_AUTOFILL_NO);
            box.addView(search, new LinearLayout.LayoutParams(-1, dp(40)));
            FlowLayout flow = new FlowLayout(ui, dp(6), dp(6));
            flow.setPadding(0, dp(8), 0, 0);
            box.addView(flow, new LinearLayout.LayoutParams(-1, -2));
            final List<Button> buttons = new ArrayList<>();
            final List<String[]> opts = new ArrayList<>(options);
            Runnable sync = () -> {
                String q = search.getText().toString().trim().toLowerCase(Locale.ROOT);
                for (int i = 0; i < buttons.size(); i++) {
                    boolean on = sel.on(opts.get(i)[1]);
                    MapTheme.gameSelected(buttons.get(i), on);
                    buttons.get(i).setVisibility(on || q.isEmpty() || opts.get(i)[0].toLowerCase(Locale.ROOT).contains(q) ? View.VISIBLE : View.GONE);
                }
            };
            for (String[] o : options) {
                Button b = chip(o[0]);
                final String key = o[1];
                b.setOnClickListener(x -> { sel.toggle(key); refresh(); });
                flow.addView(b, new ViewGroup.LayoutParams(-2, dp(34)));
                buttons.add(b);
            }
            search.addTextChangedListener(new TextWatcher() {
                public void beforeTextChanged(CharSequence s, int a, int b, int c) {}
                public void onTextChanged(CharSequence s, int a, int b, int c) {}
                public void afterTextChanged(Editable s) { sync.run(); }
            });
            register(sync);
            return box;
        }

        void register(Runnable sync) { refreshers.add(sync); }

        Button chip(String label) {
            Button b = new Button(ui);
            b.setText(label); b.setAllCaps(false); b.setTextSize(12.5f); b.setSingleLine(true);
            b.setPadding(dp(12), 0, dp(12), 0); b.setMinWidth(0); b.setMinimumWidth(0);
            b.setGravity(Gravity.CENTER);
            return b;
        }
    }

    /** A refresher belonging to the rebuildable quest option box. */
    private static final class BoxRefresher implements Runnable {
        final LinearLayout box; final Runnable inner;
        BoxRefresher(LinearLayout box, Runnable inner) { this.box = box; this.inner = inner; }
        public void run() { inner.run(); }
    }

    /** Minimal wrapping row layout for chips. */
    private static final class FlowLayout extends ViewGroup {
        private final int hGap, vGap;
        FlowLayout(Context c, int hGap, int vGap) { super(c); this.hGap = hGap; this.vGap = vGap; }

        @Override protected void onMeasure(int widthSpec, int heightSpec) {
            int maxW = MeasureSpec.getSize(widthSpec) - getPaddingLeft() - getPaddingRight();
            if (MeasureSpec.getMode(widthSpec) == MeasureSpec.UNSPECIFIED) maxW = Integer.MAX_VALUE;
            int x = 0, y = 0, lineH = 0, widest = 0;
            for (int i = 0; i < getChildCount(); i++) {
                View c = getChildAt(i);
                if (c.getVisibility() == GONE) continue;
                LayoutParams lp = c.getLayoutParams();
                int cw = lp.width >= 0 ? MeasureSpec.makeMeasureSpec(lp.width, MeasureSpec.EXACTLY) : MeasureSpec.makeMeasureSpec(Math.max(0, maxW), MeasureSpec.AT_MOST);
                int ch = lp.height >= 0 ? MeasureSpec.makeMeasureSpec(lp.height, MeasureSpec.EXACTLY) : MeasureSpec.makeMeasureSpec(0, MeasureSpec.UNSPECIFIED);
                c.measure(cw, ch);
                int w = c.getMeasuredWidth(), h = c.getMeasuredHeight();
                if (x > 0 && x + w > maxW) { x = 0; y += lineH + vGap; lineH = 0; }
                x += w + hGap; lineH = Math.max(lineH, h); widest = Math.max(widest, x - hGap);
            }
            int width = MeasureSpec.getMode(widthSpec) == MeasureSpec.EXACTLY ? MeasureSpec.getSize(widthSpec) : widest + getPaddingLeft() + getPaddingRight();
            setMeasuredDimension(width, y + lineH + getPaddingTop() + getPaddingBottom());
        }

        @Override protected void onLayout(boolean changed, int l, int t, int r, int b) {
            int maxW = r - l - getPaddingLeft() - getPaddingRight();
            int x = 0, y = 0, lineH = 0;
            for (int i = 0; i < getChildCount(); i++) {
                View c = getChildAt(i);
                if (c.getVisibility() == GONE) continue;
                int w = c.getMeasuredWidth(), h = c.getMeasuredHeight();
                if (x > 0 && x + w > maxW) { x = 0; y += lineH + vGap; lineH = 0; }
                c.layout(getPaddingLeft() + x, getPaddingTop() + y, getPaddingLeft() + x + w, getPaddingTop() + y + h);
                x += w + hGap; lineH = Math.max(lineH, h);
            }
        }
    }
}
