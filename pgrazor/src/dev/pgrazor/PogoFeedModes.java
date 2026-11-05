package dev.pgrazor;

import android.content.SharedPreferences;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Iterator;
import java.util.List;
import java.util.Locale;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

/**
 * PGRazorX: the search modes of pogocoordinates.com/search besides Pokémon (Raids, Quests, Weather,
 * Rocket, Power Spots). Each mode has its own endpoint on the coordinates API and its own filter spec,
 * captured from the site's search page:
 * <ul>
 *   <li>{@code POST /raids/search} {bosses:[{pokemon_id,form_id,variant}], tiers:[1..16],
 *       exclusive_mode:all|ex_only|normal_only, status:active|egg, boosted_only}</li>
 *   <li>{@code POST /quests/search} {reward_type, reward_pokemon_ids, reward_item_ids,
 *       reward_mega_pokemon_ids, reward_stardust_amounts, reward_pokecoin_amounts, quest_titles}</li>
 *   <li>{@code POST /weather/search} {weather_id:1..7}</li>
 *   <li>{@code POST /rocket/search} {type} (ids from {@code GET /rocket/types})</li>
 *   <li>{@code POST /powerspots/search} {pokemon_ids, battle_levels:[1..6], active_only}</li>
 * </ul>
 * Every body also carries {@code center_lat/center_lng/radius_km} (all null = worldwide). Results hold
 * no coordinates, only a {@code reveal_code} for the shared {@code GET /reveal/{code}}.
 */
final class PogoFeedModes {
    private PogoFeedModes() {}

    enum Mode {
        POKEMON("Pokémon", null, "Pokémon filters"),
        RAIDS("Raids", "/raids/search", "Raid filters"),
        QUESTS("Quests", "/quests/search", "Quest filters"),
        WEATHER("Weather", "/weather/search", "Weather filters"),
        ROCKET("Rocket", "/rocket/search", "Rocket filters"),
        POWER_SPOTS("Power Spots", "/powerspots/search", "Power Spot filters");

        final String label, path, filtersTitle;
        Mode(String label, String path, String filtersTitle) { this.label = label; this.path = path; this.filtersTitle = filtersTitle; }

        static Mode byName(String name) {
            for (Mode m : values()) if (m.name().equals(name)) return m;
            return POKEMON;
        }
    }

    /** Raid tiers as the site lists them; the API value is index + 1. */
    static final String[] RAID_TIERS = {"Tier 1", "Tier 2", "Tier 3", "Tier 4", "Legendary", "Mega",
        "Legendary Mega", "Ultra Beast", "Elite", "Primal", "Shadow Tier 1", "Shadow Tier 2", "Shadow Tier 3",
        "Shadow Tier 4", "Shadow Legendary", "Super Mega"};
    static final String[] EX_MODES = {"all", "ex_only", "normal_only"};
    static final String[] EX_LABELS = {"All", "EX only", "Normal only"};
    static final String[] RAID_STATUS = {"active", "egg"};
    static final String[] RAID_STATUS_LABELS = {"Active", "Egg"};
    /** Weather ids 1..7 (index + 1). */
    static final String[] WEATHERS = {"Sunny", "Rainy", "Partly Cloudy", "Cloudy", "Windy", "Snow", "Fog"};
    static final String[] REWARD_TYPES = {"Pokemon", "Item", "Stardust", "Mega Energy", "PokéCoin"};
    /** Power spot battle levels 1..6 (index + 1). */
    static final String[] SPOT_LEVELS = {"1★", "2★", "3★", "4★", "5★", "6★ (G-Max)"};
    /** Fallback when {@code GET /rocket/types} cannot be loaded: {id, label, group}. */
    static final String[][] ROCKET_TYPES = {
        {"kecleon", "Kecleon", "special"}, {"male", "Male Grunt", "special"}, {"female", "Female Grunt", "special"},
        {"decoy", "Decoy Grunt", "special"}, {"bug", "Bug Type", "type"}, {"dark", "Dark Type", "type"},
        {"dragon", "Dragon Type", "type"}, {"fairy", "Fairy Type", "type"}, {"fighting", "Fighting Type", "type"},
        {"fire", "Fire Type", "type"}, {"flying", "Flying Type", "type"}, {"grass", "Grass Type", "type"},
        {"ground", "Ground Type", "type"}, {"ice", "Ice Type", "type"}, {"steel", "Steel Type", "type"},
        {"normal", "Normal Type", "type"}, {"poison", "Poison Type", "type"}, {"psychic", "Psychic Type", "type"},
        {"rock", "Rock Type", "type"}, {"water", "Water Type", "type"}, {"ghost", "Ghost Type", "type"},
        {"electric", "Electric Type", "type"}, {"cliff", "Cliff", "leader"}, {"arlo", "Arlo", "leader"},
        {"sierra", "Sierra", "leader"}, {"giovanni", "Giovanni", "leader"}};

    // ------------------------------------------------------------------ specs

    /** The site's default spec for a mode (without the location keys). */
    static JSONObject defaults(Mode m) {
        JSONObject o = new JSONObject();
        try {
            switch (m) {
                case RAIDS:
                    o.put("filter_by", "tier"); // UI only: "pokemon" uses bosses, "tier" uses tiers
                    o.put("bosses", new JSONArray()); o.put("tiers", new JSONArray());
                    o.put("exclusive_mode", "all"); o.put("status", "active"); o.put("boosted_only", false);
                    break;
                case QUESTS:
                    o.put("reward_type", "Pokemon");
                    for (String k : QUEST_LISTS) o.put(k, new JSONArray());
                    break;
                case WEATHER: o.put("weather_id", JSONObject.NULL); break;
                case ROCKET: o.put("type", JSONObject.NULL); break;
                case POWER_SPOTS:
                    o.put("pokemon_ids", new JSONArray()); o.put("battle_levels", new JSONArray()); o.put("active_only", false);
                    break;
                default: break;
            }
        } catch (JSONException ignored) { }
        return o;
    }
    static final String[] QUEST_LISTS = {"reward_pokemon_ids", "reward_item_ids", "reward_mega_pokemon_ids",
        "reward_stardust_amounts", "reward_pokecoin_amounts", "quest_titles"};

    /** The saved spec of a mode, layered over the defaults so a missing key never breaks a search. */
    static JSONObject load(SharedPreferences prefs, Mode m) {
        JSONObject spec = defaults(m);
        try {
            String raw = prefs.getString("spec_" + m.name(), null);
            if (raw != null) {
                JSONObject saved = new JSONObject(raw);
                for (Iterator<String> it = saved.keys(); it.hasNext(); ) { String k = it.next(); if (spec.has(k)) spec.put(k, saved.get(k)); }
            }
        } catch (Exception ignored) { }
        return spec;
    }

    static void save(SharedPreferences prefs, Mode m, JSONObject spec) {
        prefs.edit().putString("spec_" + m.name(), spec.toString()).apply();
    }

    /** A required choice the site also insists on, or null when the spec can be searched. */
    static String missing(Mode m, JSONObject spec) {
        if (m == Mode.WEATHER && spec.isNull("weather_id")) return "Choose a weather in Filters.";
        if (m == Mode.ROCKET && (spec.isNull("type") || spec.optString("type", "").isEmpty())) return "Choose a Rocket type in Filters.";
        if (m == Mode.QUESTS && spec.optString("reward_type", "").isEmpty()) return "Choose a reward type in Filters.";
        return null;
    }

    /** Request body: the spec's API keys plus the search area ({@code radiusKm} outside 0..1000 = worldwide). */
    static JSONObject body(Mode m, JSONObject spec, double lat, double lon, double radiusKm) throws JSONException {
        JSONObject b = new JSONObject();
        switch (m) {
            case RAIDS: {
                boolean byTier = "tier".equals(spec.optString("filter_by", "tier"));
                b.put("pokemon_ids", new JSONArray());
                b.put("bosses", byTier ? new JSONArray() : arr(spec, "bosses"));
                b.put("tiers", byTier ? arr(spec, "tiers") : new JSONArray());
                b.put("exclusive_mode", spec.optString("exclusive_mode", "all"));
                b.put("status", spec.optString("status", "active"));
                b.put("boosted_only", spec.optBoolean("boosted_only", false));
                break;
            }
            case QUESTS:
                b.put("reward_type", spec.isNull("reward_type") ? JSONObject.NULL : spec.optString("reward_type"));
                for (String k : QUEST_LISTS) b.put(k, arr(spec, k));
                break;
            case WEATHER: b.put("weather_id", spec.isNull("weather_id") ? JSONObject.NULL : (Object) spec.optInt("weather_id")); break;
            case ROCKET: b.put("type", spec.isNull("type") ? JSONObject.NULL : spec.optString("type")); break;
            case POWER_SPOTS:
                b.put("pokemon_ids", arr(spec, "pokemon_ids"));
                b.put("battle_levels", arr(spec, "battle_levels"));
                b.put("active_only", spec.optBoolean("active_only", false));
                break;
            default: break;
        }
        boolean local = radiusKm > 0 && radiusKm <= PogoFeedClient.MAX_SEARCH_RADIUS_KM
                && Double.isFinite(lat) && Double.isFinite(lon) && Math.abs(lat) <= 90 && Math.abs(lon) <= 180;
        b.put("center_lat", local ? (Object) lat : JSONObject.NULL);
        b.put("center_lng", local ? (Object) lon : JSONObject.NULL);
        b.put("radius_km", local ? (Object) Math.max(0.1, radiusKm) : JSONObject.NULL);
        return b;
    }

    static JSONArray arr(JSONObject o, String key) {
        JSONArray a = o.optJSONArray(key);
        return a == null ? new JSONArray() : a;
    }

    /** Short description of the active filters for the status line. */
    static String summary(Mode m, JSONObject spec) {
        List<String> parts = new ArrayList<>();
        switch (m) {
            case RAIDS: {
                if ("tier".equals(spec.optString("filter_by", "tier"))) {
                    JSONArray t = arr(spec, "tiers");
                    List<String> names = new ArrayList<>();
                    for (int i = 0; i < t.length(); i++) { int v = t.optInt(i); if (v >= 1 && v <= RAID_TIERS.length) names.add(RAID_TIERS[v - 1]); }
                    parts.add(names.isEmpty() ? "Any tier" : join(names, ", "));
                } else {
                    int n = arr(spec, "bosses").length();
                    parts.add(n == 0 ? "Any boss" : n + (n == 1 ? " boss" : " bosses"));
                }
                int ex = indexOf(EX_MODES, spec.optString("exclusive_mode", "all"));
                if (ex > 0) parts.add(EX_LABELS[ex]);
                parts.add("egg".equals(spec.optString("status")) ? "Eggs" : "Active");
                break;
            }
            case QUESTS: {
                String type = spec.optString("reward_type", "");
                parts.add(type.isEmpty() ? "Any reward" : type);
                int picked = 0; for (String k : QUEST_LISTS) if (!k.equals("quest_titles")) picked += arr(spec, k).length();
                if (picked > 0) parts.add(picked + " selected");
                int tasks = arr(spec, "quest_titles").length();
                if (tasks > 0) parts.add(tasks + (tasks == 1 ? " task" : " tasks"));
                break;
            }
            case WEATHER: {
                int id = spec.optInt("weather_id", 0);
                parts.add(id >= 1 && id <= WEATHERS.length ? WEATHERS[id - 1] : "No weather chosen");
                break;
            }
            case ROCKET: {
                String type = spec.optString("type", "");
                String label = type;
                for (String[] t : ROCKET_TYPES) if (t[0].equals(type)) label = t[1];
                parts.add(spec.isNull("type") || type.isEmpty() ? "No type chosen" : label);
                break;
            }
            case POWER_SPOTS: {
                int n = arr(spec, "pokemon_ids").length();
                parts.add(n == 0 ? "Any boss" : n + (n == 1 ? " boss" : " bosses"));
                JSONArray lv = arr(spec, "battle_levels");
                if (lv.length() > 0) { List<String> l = new ArrayList<>(); for (int i = 0; i < lv.length(); i++) l.add(lv.optInt(i) + "★"); parts.add(join(l, ", ")); }
                if (spec.optBoolean("active_only", false)) parts.add("Active only");
                break;
            }
            default: break;
        }
        return join(parts, " · ");
    }

    // ------------------------------------------------------------------ results

    /** Maps one search response onto feed rows (all locked: coordinates come from the reveal code). */
    static List<PogoFeedData.Entry> parse(Mode m, JSONObject resp) {
        List<PogoFeedData.Entry> out = new ArrayList<>();
        JSONArray results = resp == null ? null : resp.optJSONArray("results");
        if (results == null) return out;
        for (int i = 0; i < results.length() && out.size() < PogoFeedData.MAX_ENTRIES; i++) {
            JSONObject r = results.optJSONObject(i);
            if (r == null) continue;
            String code = str(r, "reveal_code");
            if (code.isEmpty()) continue;
            PogoFeedData.Entry e = null;
            switch (m) {
                case RAIDS: e = raid(r, code); break;
                case QUESTS: e = quest(r, code); break;
                case WEATHER: e = weather(r, code); break;
                case ROCKET: e = rocket(r, code); break;
                case POWER_SPOTS: e = spot(r, code); break;
                default: break;
            }
            if (e != null) out.add(e);
        }
        return out;
    }

    private static PogoFeedData.Entry raid(JSONObject r, String code) {
        boolean egg = r.optBoolean("is_egg", false) || r.optInt("pokemon_id", 0) <= 0;
        String tier = str(r, "tier_name");
        String name = egg ? "Egg" + (tier.isEmpty() ? "" : " · " + tier) : withForm(str(r, "pokemon_name"), str(r, "form"));
        if (name.isEmpty()) name = "Raid boss";
        long end = parseIso(str(r, "raid_end")), battle = parseIso(str(r, "raid_battle"));
        List<String> d = new ArrayList<>();
        if (!egg && !tier.isEmpty()) d.add(tier);
        if (egg && battle > 0) d.add("hatches " + clock(battle));
        if (r.has("cp") && !r.isNull("cp")) d.add("CP " + r.optInt("cp"));
        String moves = moves(r);
        if (!moves.isEmpty()) d.add(moves);
        if (r.optBoolean("ex_eligible", false)) d.add("EX");
        if (r.optBoolean("weather_boosted", false)) d.add("Boosted");
        String gym = str(r, "gym_name");
        if (!gym.isEmpty()) d.add(gym);
        int level = r.optInt("raid_level", 0);
        return new PogoFeedData.Entry("raid:" + str(r, "raid_id"), PogoFeedData.Kind.RAID, name, join(d, " · "),
                Double.NaN, Double.NaN, egg ? -1 : r.optInt("pokemon_id", -1), r.has("cp") && !r.isNull("cp") ? r.optInt("cp") : -1,
                level, -1, end, false, r.optInt("team_id", 0), egg ? battle : 0, !egg, false, PogoFeedData.Attrs.UNKNOWN, code);
    }

    private static PogoFeedData.Entry quest(JSONObject r, String code) {
        JSONArray rewards = r.optJSONArray("rewards");
        List<String> labels = new ArrayList<>();
        int dex = -1;
        if (rewards != null) for (int i = 0; i < rewards.length(); i++) {
            JSONObject w = rewards.optJSONObject(i);
            if (w == null) continue;
            String label = str(w, "label");
            if (label.isEmpty()) label = str(w, "kind");
            if (!label.isEmpty()) labels.add(label);
            if (dex < 0 && w.has("pokemon_id") && !w.isNull("pokemon_id")) dex = w.optInt("pokemon_id", -1);
        }
        String name = labels.isEmpty() ? "Quest" : join(labels, ", ");
        List<String> d = new ArrayList<>();
        String task = str(r, "task");
        int target = r.optInt("target", 0);
        if (!task.isEmpty()) d.add(target > 1 ? task + " (" + target + ")" : task);
        String stop = str(r, "pokestop_name");
        if (!stop.isEmpty()) d.add(stop);
        return new PogoFeedData.Entry("quest:" + str(r, "quest_id"), PogoFeedData.Kind.QUEST, name, join(d, " · "),
                Double.NaN, Double.NaN, dex, -1, -1, -1, parseIso(str(r, "expires")), false, 0, 0, false, false,
                PogoFeedData.Attrs.UNKNOWN, code);
    }

    private static PogoFeedData.Entry weather(JSONObject r, String code) {
        int id = r.optInt("weather_id", 0);
        String name = str(r, "weather_name");
        if (name.isEmpty() && id >= 1 && id <= WEATHERS.length) name = WEATHERS[id - 1];
        String place = str(r, "location");
        return new PogoFeedData.Entry("weather:" + id + ":" + place, PogoFeedData.Kind.WEATHER, name.isEmpty() ? "Weather" : name, place,
                Double.NaN, Double.NaN, -1, -1, -1, -1, parseIso(str(r, "expires_at")), false, 0, 0, false, false,
                PogoFeedData.Attrs.UNKNOWN, code);
    }

    private static PogoFeedData.Entry rocket(JSONObject r, String code) {
        String label = str(r, "type_label");
        if (label.isEmpty()) label = str(r, "type");
        List<String> d = new ArrayList<>();
        String stop = str(r, "name"), place = str(r, "location");
        if (!stop.isEmpty()) d.add(stop);
        if (!place.isEmpty()) d.add(place);
        long exp = parseIso(str(r, "expires_at"));
        return new PogoFeedData.Entry("rocket:" + str(r, "type") + ":" + stop + ":" + place + ":" + exp, PogoFeedData.Kind.ROCKET,
                label.isEmpty() ? "Team GO Rocket" : label, join(d, " · "), Double.NaN, Double.NaN, -1, -1, -1, -1, exp, false,
                0, 0, false, false, PogoFeedData.Attrs.UNKNOWN, code);
    }

    private static PogoFeedData.Entry spot(JSONObject r, String code) {
        String name = withForm(str(r, "pokemon_name"), str(r, "form"));
        List<String> d = new ArrayList<>();
        int level = r.optInt("battle_level", 0);
        if (level > 0) d.add(level >= 6 ? "6★ G-Max" : level + "★");
        long start = parseIso(str(r, "battle_start"));
        if (!r.optBoolean("battle_active", true) && start > 0) d.add("starts " + clock(start));
        String gender = str(r, "gender");
        if (!gender.isEmpty()) d.add(gender);
        String moves = moves(r);
        if (!moves.isEmpty()) d.add(moves);
        String spot = str(r, "name"), place = str(r, "location");
        if (!spot.isEmpty()) d.add(spot);
        if (!place.isEmpty()) d.add(place);
        return new PogoFeedData.Entry("spot:" + str(r, "spot_id"), PogoFeedData.Kind.STATION, name.isEmpty() ? "Power Spot" : name,
                join(d, " · "), Double.NaN, Double.NaN, r.optInt("pokemon_id", -1), -1, level, -1,
                parseIso(str(r, "battle_end")), false, 0, 0, false, false, PogoFeedData.Attrs.UNKNOWN, code);
    }

    // ------------------------------------------------------------------ helpers

    static String str(JSONObject o, String key) {
        return o == null || !o.has(key) || o.isNull(key) ? "" : o.optString(key, "").trim();
    }

    private static String withForm(String name, String form) {
        return form.isEmpty() || name.isEmpty() ? name : name + " (" + form + ")";
    }

    private static String moves(JSONObject r) {
        String fast = str(r, "fast_move"), charge = str(r, "charge_move");
        if (fast.isEmpty() && charge.isEmpty()) return "";
        return fast + " / " + charge;
    }

    static String join(List<String> parts, String sep) {
        StringBuilder sb = new StringBuilder();
        for (String s : parts) { if (s == null || s.isEmpty()) continue; if (sb.length() > 0) sb.append(sep); sb.append(s); }
        return sb.toString();
    }

    static int indexOf(String[] values, String v) {
        for (int i = 0; i < values.length; i++) if (values[i].equals(v)) return i;
        return -1;
    }

    private static String clock(long millis) {
        return new SimpleDateFormat("HH:mm", Locale.US).format(new java.util.Date(millis));
    }

    /** ISO-8601 with 'Z' or ±hh:mm offset (optionally with millis) → epoch millis; 0 when unparseable. */
    static long parseIso(String s) {
        if (s == null || s.isEmpty()) return 0;
        for (String p : new String[]{"yyyy-MM-dd'T'HH:mm:ssXXX", "yyyy-MM-dd'T'HH:mm:ss.SSSXXX"}) {
            try {
                SimpleDateFormat fmt = new SimpleDateFormat(p, Locale.US);
                fmt.setLenient(false);
                java.util.Date d = fmt.parse(s);
                if (d != null) return d.getTime();
            } catch (Exception ignored) { }
        }
        return 0;
    }
}
