import dev.pgrazor.LiveFeedFilters;
import org.json.JSONArray;
import org.json.JSONObject;

/**
 * Validation and serialisation checks for the shared display-filter model. Uses only synthesised
 * values (no captured account data). Standalone {@code main}; compile with LiveFeedFilters.java and an
 * {@code org.json} implementation on the classpath.
 */
public final class LiveFeedFiltersTest {
    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    private static void expectReject(JSONObject o, String why) {
        boolean threw = false;
        try { LiveFeedFilters.fromJson(o); } catch (org.json.JSONException e) { threw = true; }
        check(threw, "Expected rejection: " + why);
    }

    public static void main(String[] args) throws Exception {
        // --- defaults: 21-key viewport, every default broad/off ---
        JSONObject def = new LiveFeedFilters().toViewportJson();
        check(def.length() == 21, "viewport carries all 21 keys");
        check(!def.getBoolean("show_all"), "default show_all false");
        check(def.getInt("min_iv") == 0 && def.getInt("max_iv") == 100, "default IV range broad");
        check(def.getInt("min_level") == 0 && def.getInt("max_level") == 50, "default level range broad");
        check("off".equals(def.getString("species_filter_mode")), "default species mode off");
        check(def.getJSONArray("species_filter_list").length() == 0, "default species list empty");
        check(def.getInt("pvp_max_rank") == 10, "default pvp_max_rank 10");
        check(!def.getBoolean("always_show_perfect") && !def.getBoolean("always_show_zero")
                && !def.getBoolean("always_show_rare") && !def.getBoolean("always_show_size"), "always-show off");
        check(!def.getBoolean("filter_xxs") && !def.getBoolean("filter_xxl"), "size filters off");
        check(!def.getBoolean("show_pvp_great") && !def.getBoolean("show_pvp_ultra")
                && !def.getBoolean("show_pvp_little"), "pvp toggles off");
        check(!def.getBoolean("show_my_shinies") && !def.getBoolean("shiny_only")
                && !def.getBoolean("show_my_backgrounds") && !def.getBoolean("background_only"), "feature toggles off");

        // --- captured-shape round trip, species de-duplication, order preservation ---
        JSONObject captured = new JSONObject("{\"show_all\":false,\"min_iv\":41,\"max_iv\":100,"
                + "\"min_level\":0,\"max_level\":50,\"always_show_perfect\":true,\"always_show_zero\":false,"
                + "\"always_show_rare\":true,\"always_show_size\":false,\"filter_xxs\":false,\"filter_xxl\":false,"
                + "\"species_filter_mode\":\"allowlist\",\"species_filter_list\":[415,242,113,774,415],"
                + "\"show_pvp_great\":false,\"show_pvp_ultra\":false,\"show_pvp_little\":false,\"pvp_max_rank\":10,"
                + "\"show_my_shinies\":true,\"shiny_only\":false,\"show_my_backgrounds\":true,\"background_only\":false}");
        LiveFeedFilters f = LiveFeedFilters.fromJson(captured);
        JSONObject vp = f.toViewportJson();
        check(vp.getInt("min_iv") == 41 && vp.getBoolean("always_show_perfect") && vp.getBoolean("always_show_rare"),
                "captured viewport values survive the round trip");
        check("allowlist".equals(vp.getString("species_filter_mode")), "allowlist mode survives");
        JSONArray list = vp.getJSONArray("species_filter_list");
        check(list.length() == 4 && list.getInt(0) == 415 && list.getInt(3) == 774,
                "species list de-duplicated with order preserved");
        check(!vp.getBoolean("show_my_shinies") && vp.getBoolean("show_my_backgrounds"),
                "a saved show_my_shinies is dropped (My Shinies was removed); backgrounds survive");

        // --- rejection of malformed input and out-of-range values ---
        expectReject(new JSONObject().put("show_all", "yes"), "boolean given a string");
        expectReject(new JSONObject().put("min_iv", "41"), "number given a string");
        expectReject(new JSONObject().put("min_iv", 150), "min_iv above 100");
        expectReject(new JSONObject().put("max_level", 51), "max_level above 50");
        expectReject(new JSONObject().put("min_iv", 41.5), "fractional IV");
        expectReject(new JSONObject().put("min_iv", 80).put("max_iv", 50), "inverted IV range");
        expectReject(new JSONObject().put("min_level", 40).put("max_level", 30), "inverted level range");
        expectReject(new JSONObject().put("species_filter_mode", "blocklist"), "unknown species mode");
        expectReject(new JSONObject().put("species_filter_list", new JSONArray().put(0)), "species id below 1");
        expectReject(new JSONObject().put("species_filter_list", new JSONArray().put(1026)), "species id above max");
        expectReject(new JSONObject().put("pvp_max_rank", 0), "pvp rank below guard");
        expectReject(new JSONObject().put("pvp_max_rank", 100001), "pvp rank above guard");
        JSONArray oversized = new JSONArray();
        for (int i = 0; i < LiveFeedFilters.MAX_SPECIES_ID + 1; i++) oversized.put(1);
        expectReject(new JSONObject().put("species_filter_list", oversized), "oversized species array");

        // --- unknown keys ignored; null yields defaults ---
        LiveFeedFilters tolerant = LiveFeedFilters.fromJson(new JSONObject().put("totally_unknown", 5).put("min_iv", 50));
        check(tolerant.toViewportJson().getInt("min_iv") == 50, "unknown keys ignored, known key applied");
        check(LiveFeedFilters.fromJson(null).toViewportJson().length() == 21, "null argument yields defaults");

        // --- client-side match mirrors the website (level range is not applied by the server) ---
        LiveFeedFilters lvl = LiveFeedFilters.fromJson(new JSONObject().put("min_iv", 100).put("max_iv", 100)
            .put("min_level", 0).put("max_level", 5));
        check(lvl.matches(1, 100, 4, -1, -1, -1, -1), "hundo inside the level range is kept");
        check(!lvl.matches(1, 100, 23, -1, -1, -1, -1), "hundo above the max level is dropped");
        check(!lvl.matches(1, 91, 4, -1, -1, -1, -1), "IV below the minimum is dropped");
        check(lvl.matches(1, 100, -1, -1, -1, -1, -1), "unknown level skips the level check");
        check(!lvl.matches(1, -1, 4, -1, -1, -1, -1), "no stats are dropped unless showing everything");
        LiveFeedFilters perfect = LiveFeedFilters.fromJson(new JSONObject().put("min_iv", 90).put("max_level", 5)
            .put("always_show_perfect", true));
        check(perfect.matches(1, 100, 30, -1, -1, -1, -1), "always-show perfect bypasses the level range");
        check(new LiveFeedFilters().matches(1, 50, 30, -1, -1, -1, -1), "defaults keep everything with stats");

        // --- PvP only: just the ranked ones, IV/level ignored, never sent to the service ---
        LiveFeedFilters pvp = LiveFeedFilters.fromJson(new JSONObject().put("min_iv", 0).put("max_iv", 19)
            .put("show_pvp_great", true).put("pvp_max_rank", 50).put("pvp_only", true));
        check(pvp.pvpOnlyActive(), "PvP only is active with a league ticked");
        check(pvp.matches(1, 60, 30, -1, 12, -1, -1), "Great League #12 is listed whatever its IV");
        check(!pvp.matches(1, 17.8, 16, -1, 1285, -1, -1), "in-range IV alone no longer lists it");
        check(!pvp.matches(1, 60, 30, -1, -1, 3, -1), "an unticked league's rank doesn't count");
        check(!pvp.matches(1, 60, 30, -1, 51, -1, -1), "rank past the max is dropped");
        check(pvp.toViewportJson().length() == 21 && !pvp.toRequestJson().has("pvp_only"), "pvp_only is never sent");
        check(pvp.toRequestJson().getInt("max_iv") == 100, "request opens the IV range for PvP only");
        check(pvp.toStoredJson().getBoolean("pvp_only") && pvp.toStoredJson().getInt("max_iv") == 19,
            "stored copy keeps pvp_only and the user's IV range");
        check(LiveFeedFilters.fromJson(pvp.toStoredJson()).pvpOnlyActive(), "stored copy round-trips");
        check(!LiveFeedFilters.fromJson(new JSONObject().put("pvp_only", true)).pvpOnlyActive(),
            "PvP only needs a ticked league");

        System.out.println("PASS: live feed filters defaults, validation, de-duplication and 21/8-key serialisation");
    }
}
