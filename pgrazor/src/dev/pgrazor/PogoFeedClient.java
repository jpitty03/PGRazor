package dev.pgrazor;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.SocketTimeoutException;
import java.net.URL;
import java.net.URLEncoder;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

/**
 * Minimal HTTPS client for PoGoCoordinates built on {@link HttpURLConnection} (no third-party deps).
 *
 * <p>The pogocoordinates.com/search page is a Next.js front end; the data comes from a separate API:
 * <ul>
 *   <li>{@code POST https://coordinates-api.pokemongopro.com/search} — JSON filter body, returns
 *       {@code {results:[...], tier, cap, total_matches}}. Results carry NO coordinates, only a
 *       {@code reveal_code}.</li>
 *   <li>{@code GET  https://coordinates-api.pokemongopro.com/reveal/{reveal_code}} — returns
 *       {@code {coords:"lat,lng", lat, lng}} (what the site's "Get coords" button calls).</li>
 * </ul>
 * Auth is optional: the site keeps a JWT in {@code localStorage["jwt"]} and sends it as
 * {@code Authorization: Bearer <jwt>}. Anonymous calls work with a lower result cap.
 * All methods are synchronous and must be called off the main thread.
 */
public final class PogoFeedClient {
    private static final String SITE = "https://pogocoordinates.com";
    private static final String API = "https://coordinates-api.pokemongopro.com";
    private static final int CONNECT_TIMEOUT_MS = 10000;
    private static final int READ_TIMEOUT_MS = 15000;
    private static final int MAX_RESPONSE_BYTES = 8 * 1024 * 1024;
    private static final String USER_AGENT = "Mozilla/5.0 (Linux; Android 13; Mobile) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/122.0.0.0 Mobile Safari/537.36";
    private static final long DEFAULT_RETRY_AFTER_MS = 10000;
    /** The site's own radius limit for a coordinate search. Larger radii are sent as a global search. */
    public static final double MAX_SEARCH_RADIUS_KM = 1000;
    /** PvP leagues the site's search supports (its PvP tab only offers Great and Ultra). */
    public static final String LEAGUE_GREAT = "great", LEAGUE_ULTRA = "ultra";
    private static final int REVEAL_CACHE_SIZE = 3000;

    /** Discord sign-in page that continues straight to the PoGoCoordinates authorize step. */
    private static final String DISCORD_LOGIN =
        "https://discord.com/login?redirect_to=%2Foauth2%2Fauthorize%3Fclient_id%3D1499296458753445998"
        + "%26redirect_uri%3Dhttps%253A%252F%252Fpogocoordinates.com%252Fcallback%26response_type%3Dcode"
        + "%26scope%3Didentify%2Bapplications.commands%26integration_type%3D1%26state%3Dlogin%253Adiscord";

    public String getDiscordLoginUrl() {
        return DISCORD_LOGIN;
    }

    /** Raised for every network/auth failure with a user-safe message and no raw server content. */
    public static class FeedException extends Exception {
        public final int status;       // HTTP status when known, else 0
        public final long retryAfterMs; // suggested wait for a 429, else 0

        public FeedException(String message) { this(message, 0, 0); }

        public FeedException(String message, int status, long retryAfterMs) {
            super(message);
            this.status = status;
            this.retryAfterMs = retryAfterMs;
        }
    }

    private final LiveFeedSession session;
    private volatile HttpURLConnection connection;
    private final Object requestLock = new Object();
    private long epoch;

    /** encounter_id -> {lat, lng}. Coordinates of a spawn never change, so each is revealed once. */
    private final Map<String, double[]> revealCache = new LinkedHashMap<String, double[]>(256, 0.75f, true) {
        @Override protected boolean removeEldestEntry(Map.Entry<String, double[]> eldest) {
            return size() > REVEAL_CACHE_SIZE;
        }
    };

    public PogoFeedClient(LiveFeedSession session) { this.session = session; }

    public boolean signedIn() { return session.signedIn(); }

    public void logout() { cancel(); synchronized(requestLock) { session.clear(); revealCache.clear(); } }

    /** Aborts the connection currently in flight, if any; the blocked call fails with a safe message. */
    public void cancel() {
        HttpURLConnection current;
        synchronized(requestLock) { epoch++; current = connection; }
        if (current != null) { try { current.disconnect(); } catch (Exception ignored) { } }
    }

    /** The page Discord sends the sign-in code back to; it is never loaded (see {@link #exchangeDiscordCode}). */
    public static final String CALLBACK = SITE + "/callback";

    /**
     * Finishes a Discord sign-in without loading pogocoordinates.com: the site's callback page only
     * posts the code to the API ({@code POST /auth/discord/callback {code}}) and keeps the returned
     * session token. Doing that here also works where the site itself is unreachable (some networks
     * block it by name) while the API host is not. The token is saved and also returned.
     */
    public String exchangeDiscordCode(String code) throws Exception {
        if (code == null || code.isEmpty() || code.length() > 512) throw new FeedException("Invalid sign-in code.");
        long requestEpoch; synchronized(requestLock) { requestEpoch = epoch; }
        JSONObject body = new JSONObject().put("code", code);
        Object raw = send("POST", API + "/auth/discord/callback", body, null, requestEpoch);
        String token = findToken(raw, 0);
        if (token == null) throw new FeedException("Sign-in finished but no session came back. Try again.");
        saveSession(token);
        return token;
    }

    /** The first JWT-shaped string value (three base64url parts, "eyJ…") in the response. */
    private static String findToken(Object value, int depth) {
        if (value instanceof String) {
            String s = ((String) value).trim();
            return s.startsWith("eyJ") && s.split("\\.").length == 3 && s.length() < 8192 ? s : null;
        }
        if (depth > 3) return null;
        if (value instanceof JSONObject) {
            JSONObject o = (JSONObject) value;
            for (String preferred : new String[]{"access_token", "token", "jwt"}) {
                String t = findToken(o.opt(preferred), depth + 1);
                if (t != null) return t;
            }
            for (java.util.Iterator<String> keys = o.keys(); keys.hasNext(); ) {
                String t = findToken(o.opt(keys.next()), depth + 1);
                if (t != null) return t;
            }
        }
        return null;
    }

    /** Stores the site JWT (value of {@code localStorage["jwt"]} after Discord sign-in). */
    public void saveSession(String token) throws Exception {
        if (token == null || token.trim().isEmpty())
            throw new FeedException("Invalid session token.");
        synchronized(requestLock) { session.save(token.trim()); }
    }

    /** Legacy overload kept for existing callers: same as the filter overload with only an IV floor. */
    public PogoFeedData.Snapshot fetch(double lat, double lon, double radiusKm, int minIv) throws Exception {
        JSONObject f = new PogoFeedFilters().toViewportJson();
        f.put("min_iv", (int) clamp(minIv, 0, 100));
        return fetch(lat, lon, radiusKm, PogoFeedFilters.fromJson(f));
    }

    /**
     * Runs one Pokesearch around the given centre. Coordinates are NOT looked up here: every row comes
     * back "locked" with its reveal code (unless it was revealed earlier and is cached); the UI calls
     * {@link #revealOne} when the user taps "Get coords". {@code radiusKm > 1000} is a global search.
     */
    public PogoFeedData.Snapshot fetch(double lat, double lon, double radiusKm, PogoFeedFilters filters) throws Exception {
        return fetchLeague(lat, lon, radiusKm, filters, null);
    }

    /**
     * Same search in PvP mode for one league ({@link #LEAGUE_GREAT} / {@link #LEAGUE_ULTRA}) with the
     * filters' max rank as {@code pvp_top_rank}. {@code league == null} runs the normal (stats) search.
     */
    public PogoFeedData.Snapshot fetchLeague(double lat, double lon, double radiusKm, PogoFeedFilters filters,
                                             String league) throws Exception {
        PogoFeedFilters f = filters == null ? new PogoFeedFilters() : filters;
        boolean global = !(radiusKm > 0 && radiusKm <= MAX_SEARCH_RADIUS_KM);
        if (!global) requireLocation(lat, lon);
        JSONObject body = searchBody(f, global ? null : new double[]{lat, lon}, radiusKm, f.isShinyOnly());
        if (league != null) {
            body.put("pvp_mode", true);
            body.put("pvp_league", league);
            body.put("pvp_top_rank", f.getPvpMaxRank());
            // Like the site's PvP tab: rank is the criterion, the Stats (IV/level) filters don't apply.
            body.put("iv_percent_min", JSONObject.NULL);
            body.put("iv_percent_max", JSONObject.NULL);
            body.put("level_min", JSONObject.NULL);
            body.put("level_max", JSONObject.NULL);
        }
        return search(body);
    }

    /** True when the Stats filters (IV or level range) narrow the search. */
    public static boolean hasStatsCriteria(PogoFeedFilters f) {
        return f != null && (f.getMinIv() > 0 || f.getMaxIv() < 100 || f.getMinLevel() > 0 || f.getMaxLevel() < 50);
    }

    /**
     * The searches of one refresh, in order; {@code null} is the Stats (IV) search. Without a league
     * it is just the IV search. With leagues ticked, the IV search goes FIRST (priority) when an
     * IV/level filter is set, then Great League, then Ultra League.
     */
    public static List<String> searchPlan(PogoFeedFilters f) {
        List<String> leagues = selectedLeagues(f);
        List<String> plan = new ArrayList<>();
        plan.add(null); // the IV (Stats) search always runs first
        plan.addAll(leagues);
        return plan;
    }

    /** Leagues to search one by one, in order; empty when no PvP league is selected. */
    public static List<String> selectedLeagues(PogoFeedFilters f) {
        List<String> out = new ArrayList<>();
        if (f == null) return out;
        if (f.isShowPvpGreat()) out.add(LEAGUE_GREAT);
        if (f.isShowPvpUltra()) out.add(LEAGUE_ULTRA);
        return out;
    }

    /**
     * PGRazorX: one search in another mode of the site (Raids, Quests, Weather, Rocket, Power Spots).
     * Same auth and error handling as the Pokémon search; the rows come back locked (reveal codes).
     */
    public PogoFeedData.Snapshot fetchMode(PogoFeedModes.Mode mode, org.json.JSONObject spec, double lat, double lon,
                                           double radiusKm) throws Exception {
        long now = System.currentTimeMillis();
        JSONObject resp = postJson(mode.path, PogoFeedModes.body(mode, spec, lat, lon, radiusKm));
        return new PogoFeedData.Snapshot(PogoFeedModes.parse(mode, resp), now, PogoFeedData.DEFAULT_REFRESH_SECONDS, tierNote(resp));
    }

    /** {@code POST} a JSON body to an API path and return the JSON object reply. */
    public JSONObject postJson(String path, JSONObject body) throws Exception {
        long requestEpoch; synchronized(requestLock) { requestEpoch = epoch; }
        Object raw = send("POST", API + path, body, session.token(), requestEpoch);
        if (!(raw instanceof JSONObject)) throw new FeedException("The search service returned an unexpected response.");
        return (JSONObject) raw;
    }

    /** {@code GET} an API path (filter catalogs such as /rocket/types) and return the JSON object reply. */
    public JSONObject getJson(String path) throws Exception {
        long requestEpoch; synchronized(requestLock) { requestEpoch = epoch; }
        Object raw = send("GET", API + path, null, session.token(), requestEpoch);
        if (!(raw instanceof JSONObject)) throw new FeedException("The service returned an unexpected response.");
        return (JSONObject) raw;
    }

    private String tierNote(JSONObject resp) {
        StringBuilder note = new StringBuilder();
        String tier = resp.optString("tier", "");
        int cap = resp.optInt("cap", 0);
        if (!tier.isEmpty()) {
            note.append("Tier: ").append("vip".equals(tier) ? "VIP" : "logged_in".equals(tier) ? "Logged in" : "Guest");
            if (cap > 0) note.append(" · cap ").append(cap).append(" per search");
        }
        long wait = revealWaitMs();
        if (wait > 0) {
            if (note.length() > 0) note.append('\n');
            note.append("Coordinate lookup limit reached · available again in ~").append((wait + 59999) / 60000).append(" min");
        }
        return note.toString();
    }

    // ------------------------------------------------------------------ search + reveal

    private PogoFeedData.Snapshot search(JSONObject body) throws Exception {
        long requestEpoch; synchronized(requestLock) { requestEpoch = epoch; }
        String token = session.token(); // may be null: anonymous search is allowed (lower cap)
        long now = System.currentTimeMillis();

        Object raw = send("POST", API + "/search", body, token, requestEpoch);
        if (!(raw instanceof JSONObject)) throw new FeedException("The search service returned an unexpected response.");
        JSONObject resp = (JSONObject) raw;
        JSONArray results = resp.optJSONArray("results");
        if (results == null) results = new JSONArray();

        JSONArray normalized = new JSONArray();
        for (int i = 0; i < results.length(); i++) {
            JSONObject r = results.optJSONObject(i);
            if (r == null) continue;
            String encounter = r.optString("encounter_id", "");
            String code = r.optString("reveal_code", "");
            if (encounter.isEmpty() || code.isEmpty()) continue;
            double[] ll;
            synchronized(requestLock) { ll = revealCache.get(encounter); } // only ones the user revealed
            normalized.put(normalize(r, ll, false));
        }

        JSONObject data = new JSONObject();
        data.put("pokemon", normalized);
        PogoFeedData.Snapshot parsed = PogoFeedData.parse(new JSONObject().put("data", data), now);

        StringBuilder note = new StringBuilder();
        String tier = resp.optString("tier", "");
        int cap = resp.optInt("cap", 0);
        if (!tier.isEmpty()) {
            note.append("Tier: ").append("vip".equals(tier) ? "VIP" : "logged_in".equals(tier) ? "Logged in" : "Guest");
            if (cap > 0) note.append(" · cap ").append(cap).append(" per search");
        }
        long wait = revealWaitMs();
        if (wait > 0) {
            if (note.length() > 0) note.append('\n');
            note.append("Coordinate lookup limit reached · available again in ~").append((wait + 59999) / 60000).append(" min");
        }
        return new PogoFeedData.Snapshot(parsed.entries, now, PogoFeedData.DEFAULT_REFRESH_SECONDS, note.toString());
    }

    /** Epoch millis until which the server said coordinate lookups are rate-limited (0 = free). */
    private volatile long revealBlockedUntil;

    /** Millis left before coordinate lookups are allowed again (0 when allowed now). */
    public long revealWaitMs() { return Math.max(0, revealBlockedUntil - System.currentTimeMillis()); }

    /**
     * On-demand lookup for one locked row (the site's "Get coords" button). Returns {lat, lng}.
     * Throws {@link FeedException} 429 with {@code retryAfterMs} while the lookup quota is used up.
     */
    public double[] revealOne(String encounterId, String code) throws Exception {
        if (code == null || code.isEmpty()) throw new FeedException("This result has no lookup code.");
        double[] cached;
        synchronized(requestLock) { cached = encounterId == null ? null : revealCache.get(encounterId); }
        if (cached != null) return cached;
        long wait = revealWaitMs();
        if (wait > 0) throw new FeedException("Lookup limit reached. Try again in ~" + ((wait + 59999) / 60000) + " min.", 429, wait);
        long requestEpoch; synchronized(requestLock) { requestEpoch = epoch; }
        double[] ll = reveal(encounterId, code, session.token(), requestEpoch);
        if (ll == null) throw new FeedException("The service returned no coordinates for this result.");
        return ll;
    }

    /** {@code GET /reveal/{code}} → {lat, lng} (cached by encounter), or null for an unusable payload. */
    private double[] reveal(String encounterId, String code, String token, long requestEpoch) throws Exception {
        String path = API + "/reveal/" + URLEncoder.encode(code, "UTF-8");
        Object raw;
        try {
            raw = send("GET", path, null, token, requestEpoch);
        } catch (FeedException fe) {
            if (fe.status == 429) revealBlockedUntil = System.currentTimeMillis() + Math.max(fe.retryAfterMs, 60_000L);
            throw fe;
        }
        if (!(raw instanceof JSONObject)) return null;
        JSONObject o = (JSONObject) raw;
        double lat = o.optDouble("lat", Double.NaN), lng = o.optDouble("lng", Double.NaN);
        if (!Double.isFinite(lat) || !Double.isFinite(lng)) {
            String[] parts = o.optString("coords", "").split(",");
            if (parts.length == 2) {
                try { lat = Double.parseDouble(parts[0].trim()); lng = Double.parseDouble(parts[1].trim()); }
                catch (NumberFormatException ignored) { return null; }
            }
        }
        if (!Double.isFinite(lat) || !Double.isFinite(lng) || lat < -90 || lat > 90 || lng < -180 || lng > 180) return null;
        double[] ll = new double[]{lat, lng};
        if (encounterId != null) synchronized(requestLock) { revealCache.put(encounterId, ll); }
        return ll;
    }

    /** Exact body shape the site sends (captured from the /search page), filled from our filters. */
    private static JSONObject searchBody(PogoFeedFilters f, double[] center, double radiusKm, boolean shinyOnly) throws JSONException {
        JSONObject b = new JSONObject();
        b.put("shiny_only", shinyOnly);
        b.put("shiny_account", JSONObject.NULL);
        b.put("background_only", f.isBackgroundOnly());
        b.put("pvp_mode", false);
        b.put("pvp_league", "great");
        b.put("pvp_top_rank", JSONObject.NULL);
        b.put("pvp_include_pre_evolutions", true);
        b.put("pvp_min_cp", JSONObject.NULL);
        JSONArray ids = new JSONArray();
        if (PogoFeedFilters.MODE_ALLOWLIST.equals(f.getSpeciesMode()))
            for (Integer id : f.getSpeciesList()) ids.put((int) id);
        b.put("pokemon_ids", ids);
        b.put("species_forms", new JSONArray());
        b.put("location", JSONObject.NULL);
        b.put("cp_min", JSONObject.NULL);
        b.put("cp_max", JSONObject.NULL);
        b.put("level_min", f.getMinLevel() > 0 ? (Object) f.getMinLevel() : JSONObject.NULL);
        b.put("level_max", f.getMaxLevel() < 50 ? (Object) f.getMaxLevel() : JSONObject.NULL);
        b.put("iv_mode", "percent");
        b.put("iv_percent_min", f.getMinIv() > 0 ? (Object) f.getMinIv() : JSONObject.NULL);
        b.put("iv_percent_max", f.getMaxIv() < 100 ? (Object) f.getMaxIv() : JSONObject.NULL);
        b.put("raw_iv_atk", JSONObject.NULL);
        b.put("raw_iv_def", JSONObject.NULL);
        b.put("raw_iv_sta", JSONObject.NULL);
        JSONArray sizes = new JSONArray();
        if (f.isFilterXxs()) sizes.put("XXS");
        if (f.isFilterXxl()) sizes.put("XXL");
        b.put("sizes", sizes);
        b.put("forms", new JSONArray());
        b.put("gender", JSONObject.NULL);
        b.put("boosted", JSONObject.NULL);
        b.put("weather_id", JSONObject.NULL);
        b.put("min_ttl", JSONObject.NULL);
        if (center != null) {
            b.put("sort", "DISTANCE");
            b.put("center_lat", center[0]);
            b.put("center_lng", center[1]);
            b.put("radius_km", clamp(radiusKm, 0.1, MAX_SEARCH_RADIUS_KM));
        } else {
            b.put("sort", "DSP");
            b.put("center_lat", JSONObject.NULL);
            b.put("center_lng", JSONObject.NULL);
            b.put("radius_km", JSONObject.NULL);
        }
        b.put("blacklist_pokemon_ids", new JSONArray());
        b.put("blacklist_species_forms", new JSONArray());
        b.put("blacklist_forms", new JSONArray());
        return b;
    }

    /** Maps one /search result (+ revealed coords) onto the keys {@link PogoFeedData#parse} understands. */
    private static JSONObject normalize(JSONObject r, double[] ll, boolean personal) throws JSONException {
        JSONObject o = new JSONObject();
        o.put("encounter_id", r.optString("encounter_id"));
        o.put("pokemon_id", r.optInt("pokemon_id", 0));
        String name = r.optString("pokemon_name", "");
        String form = r.isNull("form") ? "" : r.optString("form", "");
        String costume = r.isNull("costume") ? "" : r.optString("costume", "");
        if (!form.isEmpty()) name = name + " (" + form + ")";
        if (!costume.isEmpty()) name = name + " [" + costume + "]";
        o.put("pokemon_name", name);
        if (ll != null) {
            o.put("lat", ll[0]);
            o.put("lon", ll[1]);
        } else {
            o.put("reveal_code", r.optString("reveal_code", "")); // locked row: coords on demand
        }

        boolean hasIv = r.has("iv_percent") && !r.isNull("iv_percent");
        o.put("has_full_stats", hasIv);
        if (hasIv) o.put("iv_percent", r.optDouble("iv_percent"));
        if (r.has("cp") && !r.isNull("cp")) o.put("cp", r.optInt("cp"));
        if (r.has("level") && !r.isNull("level")) o.put("level", r.optDouble("level"));
        String[] raw = r.optString("raw_iv", "").split("/");
        if (raw.length == 3) {
            try {
                o.put("iv_attack", Integer.parseInt(raw[0].trim()));
                o.put("iv_defense", Integer.parseInt(raw[1].trim()));
                o.put("iv_stamina", Integer.parseInt(raw[2].trim()));
            } catch (NumberFormatException ignored) { }
        }
        long despawn = parseIso(r.optString("despawn_at", ""));
        if (despawn > 0) o.put("despawn_ms", despawn);
        o.put("is_shiny", personal || r.optBoolean("shiny", false));

        int gender = genderId(r.optString("gender", ""));
        if (gender > 0) o.put("gender", gender);
        int size = sizeId(r.optString("size", ""));
        if (size > 0) o.put("size", size);

        // Extra one-line text the row/detail can show: size · gender · moves · place · boosted
        List<String> extra = new ArrayList<>();
        if (!r.optString("size", "").isEmpty()) extra.add(r.optString("size"));
        if (!r.optString("gender", "").isEmpty()) extra.add(r.optString("gender"));
        String fast = r.isNull("fast_move") ? "" : r.optString("fast_move", "");
        String charge = r.isNull("charge_move") ? "" : r.optString("charge_move", "");
        if (!fast.isEmpty() || !charge.isEmpty()) extra.add(fast + " / " + charge);
        if (r.optBoolean("weather_boosted", false)) extra.add("Boosted");
        if (r.optBoolean("background", false)) extra.add("Background");
        JSONObject pvp = r.optJSONObject("pvp");
        if (pvp != null) {
            String lg = pvp.optString("league", "");
            int rank = pvp.optInt("rank", -1);
            if (rank > 0) {
                if (LEAGUE_GREAT.equals(lg)) o.put("pvp_great_rank", rank);
                if (LEAGUE_ULTRA.equals(lg)) o.put("pvp_ultra_rank", rank);
                StringBuilder pv = new StringBuilder(LEAGUE_ULTRA.equals(lg) ? "UL" : "GL").append(" #").append(rank);
                if (pvp.has("percent")) pv.append(String.format(Locale.US, " (%.1f%%)", pvp.optDouble("percent")));
                String evo = pvp.optString("pokemon_name", "");
                if (!evo.isEmpty() && !evo.equals(r.optString("pokemon_name", ""))) pv.append(" as ").append(evo);
                if (pvp.has("cp")) pv.append(" CP ").append(pvp.optInt("cp"));
                extra.add(0, pv.toString());
            }
        }
        String place = r.isNull("location") ? "" : r.optString("location", "");
        if (!place.isEmpty()) extra.add(place);
        if (!extra.isEmpty()) {
            StringBuilder sb = new StringBuilder();
            for (String s : extra) { if (sb.length() > 0) sb.append(" · "); sb.append(s); }
            o.put("extra_details", sb.toString());
        }
        return o;
    }

    /** ISO-8601 with 'Z' or ±hh:mm offset → epoch millis; 0 when unparseable. */
    private static long parseIso(String s) {
        if (s == null || s.isEmpty()) return 0;
        String[] patterns = {"yyyy-MM-dd'T'HH:mm:ssXXX", "yyyy-MM-dd'T'HH:mm:ss.SSSXXX"};
        for (String p : patterns) {
            try {
                SimpleDateFormat fmt = new SimpleDateFormat(p, Locale.US);
                fmt.setLenient(false);
                java.util.Date d = fmt.parse(s);
                if (d != null) return d.getTime();
            } catch (Exception ignored) { }
        }
        return 0;
    }

    private static int genderId(String g) {
        switch (g) { case "Male": return 1; case "Female": return 2; case "Genderless": return 3; default: return 0; }
    }

    private static int sizeId(String s) {
        switch (s) { case "XXS": return 1; case "XS": return 2; case "M": return 3; case "XL": return 4; case "XXL": return 5; default: return 0; }
    }

    private static void requireLocation(double lat, double lon) throws FeedException {
        if (!Double.isFinite(lat) || !Double.isFinite(lon) || lat < -90 || lat > 90 || lon < -180 || lon > 180)
            throw new FeedException("That location is outside the valid range.");
    }

    // ------------------------------------------------------------------ transport

    // On an expired-session 401 for this still-current request, clears the token so the UI falls back
    // to the login form. A 403 (no access) deliberately leaves the session intact.
    private Object send(String method, String url, JSONObject body, String token, long requestEpoch) throws Exception {
        try {
            return request(method, url, body, token, requestEpoch);
        } catch (FeedException fe) {
            if (fe.status == 401 && token != null) synchronized(requestLock) { if (epoch == requestEpoch) session.clear(); }
            throw fe;
        }
    }

    private Object request(String method, String target, JSONObject body, String bearer, long requestEpoch) throws Exception {
        URL url = new URL(target);
        if (!"https".equalsIgnoreCase(url.getProtocol())) throw new FeedException("A secure connection is required.");
        HttpURLConnection http = (HttpURLConnection) url.openConnection();
        synchronized(requestLock) { checkActive(requestEpoch); connection = http; }
        try {
            http.setConnectTimeout(CONNECT_TIMEOUT_MS);
            http.setReadTimeout(READ_TIMEOUT_MS);
            http.setInstanceFollowRedirects(false); // refuse redirects so the bearer token cannot leak
            http.setRequestMethod(method);
            http.setRequestProperty("Accept", "application/json");
            http.setRequestProperty("User-Agent", USER_AGENT);
            http.setRequestProperty("Referer", SITE + "/search");
            http.setRequestProperty("Origin", SITE);
            if (bearer != null && !bearer.isEmpty()) http.setRequestProperty("Authorization", "Bearer " + bearer);
            if (body != null) {
                http.setDoOutput(true);
                http.setRequestProperty("Content-Type", "application/json");
                byte[] payload = body.toString().getBytes(StandardCharsets.UTF_8);
                try (OutputStream out = http.getOutputStream()) { out.write(payload); }
            }

            int code = http.getResponseCode();
            if (code >= 300 && code < 400)
                throw new FeedException("The service redirected the request; refusing to continue.", code, 0);
            if (code == 401)
                throw new FeedException("Your session has expired. Please sign in again.", 401, 0);
            if (code == 403)
                throw new FeedException("Your account doesn't have access to this feature.", 403, 0);
            if (code == 429)
                throw new FeedException("Too many requests. Please wait before refreshing.", 429, retryAfterMs(http, readError(http)));
            if (code < 200 || code >= 300)
                throw new FeedException("The search service is unavailable (HTTP " + code + ").", code, 0);

            byte[] bytes;
            try (InputStream in = http.getInputStream()) { bytes = readBounded(in); }
            String responseStr = new String(bytes, StandardCharsets.UTF_8).trim();
            Object json;
            try {
                json = responseStr.startsWith("[") ? new JSONArray(responseStr) : new JSONObject(responseStr);
            } catch (JSONException e) {
                throw new FeedException("The search service returned an unexpected response.", code, 0);
            }
            synchronized(requestLock) { checkActive(requestEpoch); }
            return json;
        } catch (SocketTimeoutException e) {
            throw new FeedException("The search service timed out. Please retry.");
        } catch (IOException e) {
            synchronized(requestLock) { checkActive(requestEpoch); }
            throw new FeedException("Could not reach the search service.");
        } finally {
            try { http.disconnect(); } catch (Exception ignored) { }
            if (connection == http) connection = null;
        }
    }

    private void checkActive(long requestEpoch) throws FeedException {
        if (epoch != requestEpoch || Thread.currentThread().isInterrupted()) throw new FeedException("Request cancelled.");
    }

    /** Retry-After header, else {@code detail.retry_after} (seconds) from the JSON error body. */
    private static long retryAfterMs(HttpURLConnection http, String errorBody) {
        String header = http.getHeaderField("Retry-After");
        if (header != null) {
            try {
                long seconds = Long.parseLong(header.trim());
                if (seconds >= 0) return Math.min(seconds, 3600) * 1000L;
            } catch (NumberFormatException ignored) { }
        }
        try {
            JSONObject o = new JSONObject(errorBody);
            JSONObject d = o.optJSONObject("detail");
            long seconds = d != null ? d.optLong("retry_after", -1) : o.optLong("retry_after", -1);
            if (seconds >= 0) return Math.min(seconds, 3600) * 1000L;
        } catch (Exception ignored) { }
        return DEFAULT_RETRY_AFTER_MS;
    }

    private static String readError(HttpURLConnection http) {
        try (InputStream in = http.getErrorStream()) {
            if (in == null) return "";
            byte[] b = readBounded(in);
            return new String(b, 0, Math.min(b.length, 4096), StandardCharsets.UTF_8);
        } catch (Exception e) { return ""; }
    }

    private static byte[] readBounded(InputStream stream) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buffer = new byte[8192];
        int count;
        while ((count = stream.read(buffer)) != -1) {
            if (Thread.currentThread().isInterrupted()) throw new IOException("Interrupted");
            if (out.size() + count > MAX_RESPONSE_BYTES) throw new IOException("Response too large");
            out.write(buffer, 0, count);
        }
        return out.toByteArray();
    }

    private static double clamp(double value, double min, double max) {
        return value < min ? min : (value > max ? max : value);
    }
}
