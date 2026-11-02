package dev.pgrazor;

import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashSet;
import java.util.List;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

/**
 * Immutable snapshot of the PoGoCoordinates display-filter set shared by the viewport and My Shinies
 * requests. Exactly the 21 filter keys observed on the wire are modelled; every default is broad/off
 * so a freshly constructed instance hides nothing it has not been told to hide.
 *
 * <p>{@link #fromJson(JSONObject)} is the only mutable-input entry point: it validates types and
 * ranges, rejecting malformed payloads with a {@link JSONException}, silently ignoring unknown keys,
 * and normalising the species allowlist (deduplicated, bounded to the current Pokédex range). The two
 * serialisers emit the exact shapes captured from the service: {@link #toViewportJson()} carries all
 * 21 keys and {@link #toShiniesJson()} carries only the 8-key subset the My Shinies endpoint accepts.
 *
 * <p>No value here is a service guarantee. Ranges such as the PvP rank cap are validated purely as an
 * app-side guard against nonsensical input; the server decides what it actually returns.
 */
public final class LiveFeedFilters {
    /** The only two species-filter modes observed; any other value is rejected. */
    public static final String MODE_OFF = "off";
    public static final String MODE_ALLOWLIST = "allowlist";
    /** Upper bound of the bundled Pokédex sprite range; species ids must fall in {@code 1..MAX}. */
    public static final int MAX_SPECIES_ID = 1025;
    /** App-side guard rails for the PvP rank cap (not a claim about the service's own limits). */
    public static final int MIN_PVP_RANK = 1;
    public static final int MAX_PVP_RANK = 100000;
    private static final int DEFAULT_PVP_MAX_RANK = 10;
    private static final int IV_MIN = 0, IV_MAX = 100;
    private static final int LEVEL_MIN = 0, LEVEL_MAX = 50;

    private final boolean showAll;
    private final int minIv, maxIv, minLevel, maxLevel;
    private final boolean alwaysShowPerfect, alwaysShowZero, alwaysShowRare, alwaysShowSize;
    private final boolean filterXxs, filterXxl;
    private final String speciesMode;
    private final List<Integer> speciesList; // unmodifiable, deduplicated, each id in 1..MAX_SPECIES_ID
    private final boolean showPvpGreat, showPvpUltra, showPvpLittle;
    private final int pvpMaxRank;
    private final boolean showMyShinies, shinyOnly, showMyBackgrounds, backgroundOnly;

    /** Broad/off defaults: nothing filtered out, every "always show" and feature toggle disabled. */
    public LiveFeedFilters() {
        this(false, IV_MIN, IV_MAX, LEVEL_MIN, LEVEL_MAX, false, false, false, false, false, false,
                MODE_OFF, Collections.<Integer>emptyList(), false, false, false, DEFAULT_PVP_MAX_RANK,
                false, false, false, false);
    }

    private LiveFeedFilters(boolean showAll, int minIv, int maxIv, int minLevel, int maxLevel,
                            boolean alwaysShowPerfect, boolean alwaysShowZero, boolean alwaysShowRare,
                            boolean alwaysShowSize, boolean filterXxs, boolean filterXxl, String speciesMode,
                            List<Integer> speciesList, boolean showPvpGreat, boolean showPvpUltra,
                            boolean showPvpLittle, int pvpMaxRank, boolean showMyShinies, boolean shinyOnly,
                            boolean showMyBackgrounds, boolean backgroundOnly) {
        this.showAll = showAll;
        this.minIv = minIv;
        this.maxIv = maxIv;
        this.minLevel = minLevel;
        this.maxLevel = maxLevel;
        this.alwaysShowPerfect = alwaysShowPerfect;
        this.alwaysShowZero = alwaysShowZero;
        this.alwaysShowRare = alwaysShowRare;
        this.alwaysShowSize = alwaysShowSize;
        this.filterXxs = filterXxs;
        this.filterXxl = filterXxl;
        this.speciesMode = speciesMode;
        this.speciesList = Collections.unmodifiableList(new ArrayList<>(speciesList));
        this.showPvpGreat = showPvpGreat;
        this.showPvpUltra = showPvpUltra;
        this.showPvpLittle = showPvpLittle;
        this.pvpMaxRank = pvpMaxRank;
        this.showMyShinies = showMyShinies;
        this.shinyOnly = shinyOnly;
        this.showMyBackgrounds = showMyBackgrounds;
        this.backgroundOnly = backgroundOnly;
    }

    /**
     * Builds a validated, immutable filter set from a raw object. Missing keys fall back to the broad
     * defaults; unknown keys are ignored. Throws {@link JSONException} for a wrong-typed value, an
     * out-of-range number, an inverted min/max, an unrecognised species mode, or a species id outside
     * {@code 1..MAX_SPECIES_ID}. A {@code null} argument yields the defaults.
     */
    public static LiveFeedFilters fromJson(JSONObject o) throws JSONException {
        if (o == null) return new LiveFeedFilters();

        boolean showAll = boolField(o, "show_all", false);
        int minIv = intField(o, "min_iv", IV_MIN, IV_MIN, IV_MAX);
        int maxIv = intField(o, "max_iv", IV_MAX, IV_MIN, IV_MAX);
        int minLevel = intField(o, "min_level", LEVEL_MIN, LEVEL_MIN, LEVEL_MAX);
        int maxLevel = intField(o, "max_level", LEVEL_MAX, LEVEL_MIN, LEVEL_MAX);
        if (minIv > maxIv) throw new JSONException("min_iv cannot exceed max_iv.");
        if (minLevel > maxLevel) throw new JSONException("min_level cannot exceed max_level.");

        boolean alwaysShowPerfect = boolField(o, "always_show_perfect", false);
        boolean alwaysShowZero = boolField(o, "always_show_zero", false);
        boolean alwaysShowRare = boolField(o, "always_show_rare", false);
        boolean alwaysShowSize = boolField(o, "always_show_size", false);
        boolean filterXxs = boolField(o, "filter_xxs", false);
        boolean filterXxl = boolField(o, "filter_xxl", false);
        String speciesMode = modeField(o);
        List<Integer> speciesList = speciesField(o);
        boolean showPvpGreat = boolField(o, "show_pvp_great", false);
        boolean showPvpUltra = boolField(o, "show_pvp_ultra", false);
        boolean showPvpLittle = boolField(o, "show_pvp_little", false);
        int pvpMaxRank = intField(o, "pvp_max_rank", DEFAULT_PVP_MAX_RANK, MIN_PVP_RANK, MAX_PVP_RANK);
        boolean showMyShinies = boolField(o, "show_my_shinies", false);
        boolean shinyOnly = boolField(o, "shiny_only", false);
        boolean showMyBackgrounds = boolField(o, "show_my_backgrounds", false);
        boolean backgroundOnly = boolField(o, "background_only", false);

        return new LiveFeedFilters(showAll, minIv, maxIv, minLevel, maxLevel, alwaysShowPerfect,
                alwaysShowZero, alwaysShowRare, alwaysShowSize, filterXxs, filterXxl, speciesMode,
                speciesList, showPvpGreat, showPvpUltra, showPvpLittle, pvpMaxRank, showMyShinies,
                shinyOnly, showMyBackgrounds, backgroundOnly);
    }

    /** Full 21-key display-filter object for {@code POST /api/map/viewport}. */
    public JSONObject toViewportJson() throws JSONException {
        JSONObject f = new JSONObject();
        f.put("show_all", showAll);
        f.put("min_iv", minIv);
        f.put("max_iv", maxIv);
        f.put("min_level", minLevel);
        f.put("max_level", maxLevel);
        f.put("always_show_perfect", alwaysShowPerfect);
        f.put("always_show_zero", alwaysShowZero);
        f.put("always_show_rare", alwaysShowRare);
        f.put("always_show_size", alwaysShowSize);
        f.put("filter_xxs", filterXxs);
        f.put("filter_xxl", filterXxl);
        f.put("species_filter_mode", speciesMode);
        f.put("species_filter_list", speciesJson());
        f.put("show_pvp_great", showPvpGreat);
        f.put("show_pvp_ultra", showPvpUltra);
        f.put("show_pvp_little", showPvpLittle);
        f.put("pvp_max_rank", pvpMaxRank);
        f.put("show_my_shinies", showMyShinies);
        f.put("shiny_only", shinyOnly);
        f.put("show_my_backgrounds", showMyBackgrounds);
        f.put("background_only", backgroundOnly);
        return f;
    }

    /** The 8-key subset accepted by {@code POST /api/game/my_shinies}, in captured order. */
    public JSONObject toShiniesJson() throws JSONException {
        JSONObject f = new JSONObject();
        f.put("species_filter_mode", speciesMode);
        f.put("species_filter_list", speciesJson());
        f.put("min_iv", minIv);
        f.put("max_iv", maxIv);
        f.put("min_level", minLevel);
        f.put("max_level", maxLevel);
        f.put("filter_xxs", filterXxs);
        f.put("filter_xxl", filterXxl);
        return f;
    }

    public boolean isShinyOnly() { return shinyOnly; }
    public boolean isShowMyShinies() { return showMyShinies; }
    public boolean isFilterXxs() { return filterXxs; }
    public boolean isBackgroundOnly() { return backgroundOnly; }
    public boolean isShowPvpGreat() { return showPvpGreat; }
    public boolean isShowPvpUltra() { return showPvpUltra; }
    public int getPvpMaxRank() { return pvpMaxRank; }
    public boolean isFilterXxl() { return filterXxl; }
    public int getMinIv() { return minIv; }
    public int getMaxIv() { return maxIv; }
    public int getMinLevel() { return minLevel; }
    public int getMaxLevel() { return maxLevel; }
    public String getSpeciesMode() { return speciesMode; }
    public List<Integer> getSpeciesList() { return speciesList; }

    private JSONArray speciesJson() {
        JSONArray array = new JSONArray();
        for (Integer id : speciesList) array.put((int) id);
        return array;
    }

    // --- validation helpers ----------------------------------------------------------------------

    private static boolean boolField(JSONObject o, String key, boolean def) throws JSONException {
        if (!o.has(key) || o.isNull(key)) return def;
        Object v = o.get(key);
        if (v instanceof Boolean) return (Boolean) v;
        throw new JSONException("Filter '" + key + "' must be a boolean.");
    }

    private static int intField(JSONObject o, String key, int def, int lo, int hi) throws JSONException {
        if (!o.has(key) || o.isNull(key)) return def;
        Object v = o.get(key);
        if (!(v instanceof Number)) throw new JSONException("Filter '" + key + "' must be a number.");
        double d = ((Number) v).doubleValue();
        if (Double.isNaN(d) || Double.isInfinite(d) || Math.floor(d) != d)
            throw new JSONException("Filter '" + key + "' must be a whole number.");
        if (d < lo || d > hi)
            throw new JSONException("Filter '" + key + "' is out of range (" + lo + ".." + hi + ").");
        return (int) d;
    }

    private static String modeField(JSONObject o) throws JSONException {
        String key = "species_filter_mode";
        if (!o.has(key) || o.isNull(key)) return MODE_OFF;
        Object v = o.get(key);
        if (!(v instanceof String)) throw new JSONException("species_filter_mode must be a string.");
        String mode = (String) v;
        if (MODE_OFF.equals(mode) || MODE_ALLOWLIST.equals(mode)) return mode;
        throw new JSONException("species_filter_mode must be '" + MODE_OFF + "' or '" + MODE_ALLOWLIST + "'.");
    }

    private static List<Integer> speciesField(JSONObject o) throws JSONException {
        String key = "species_filter_list";
        if (!o.has(key) || o.isNull(key)) return Collections.emptyList();
        Object v = o.get(key);
        if (!(v instanceof JSONArray)) throw new JSONException("species_filter_list must be an array.");
        JSONArray array = (JSONArray) v;
        // Reject an oversized array up front so a hostile payload cannot force a huge allocation; after
        // de-duplication the list can never exceed the id range anyway.
        if (array.length() > MAX_SPECIES_ID)
            throw new JSONException("species_filter_list has too many entries (max " + MAX_SPECIES_ID + ").");
        LinkedHashSet<Integer> ids = new LinkedHashSet<>();
        for (int i = 0; i < array.length(); i++) {
            Object item = array.get(i);
            if (!(item instanceof Number)) throw new JSONException("species_filter_list must contain numbers.");
            double d = ((Number) item).doubleValue();
            if (Double.isNaN(d) || Double.isInfinite(d) || Math.floor(d) != d)
                throw new JSONException("species ids must be whole numbers.");
            int id = (int) d;
            if (id < 1 || id > MAX_SPECIES_ID)
                throw new JSONException("species ids must be in 1.." + MAX_SPECIES_ID + ".");
            ids.add(id);
        }
        return new ArrayList<>(ids);
    }
}
