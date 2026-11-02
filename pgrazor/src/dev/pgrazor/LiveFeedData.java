package dev.pgrazor;

import java.util.ArrayList;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

/**
 * Immutable, UI-agnostic model for PoGoCoordinates search results plus the parser that turns raw
 * response data into bounded, sanitised entries.
 *
 * <p>Parsing is defensive: invalid/non-finite coordinates are dropped, expired pokemon/raids/stations
 * are discarded, unknown fields stay unknown rather than being invented, and the total entry count is
 * capped so a hostile or malformed payload cannot exhaust memory. Names/details are plain-text safe.
 */
public final class LiveFeedData {
    /** Hard cap on produced entries regardless of how many the payload advertises. */
    public static final int MAX_ENTRIES = 10000;
    /** Default client refresh cadence; the UI may override from its own config. */
    public static final int DEFAULT_REFRESH_SECONDS = 15;
    /** Bounds applied to a server-advertised refresh cadence so it can neither hammer nor stall polling. */
    private static final int MIN_REFRESH_SECONDS = 5;
    private static final int MAX_REFRESH_SECONDS = 600;

    public enum Kind { POKEMON, STOP, GYM, RAID, STATION, QUEST, WEATHER, ROCKET }

    /**
     * Optional, observed detail attributes for a Pokémon/shiny row. Every field uses {@code -1} for
     * "not present / unknown" so the UI never renders a fabricated 0. Only fields that actually appear
     * in the viewport and My Shinies schemas are modelled; backgrounds are deliberately absent because
     * no positive background schema was observed. Immutable.
     */
    public static final class Attrs {
        /** Shared singleton for entries with no detail attributes (every field unknown). */
        public static final Attrs UNKNOWN = new Attrs(-1, -1, -1, -1, -1, -1, -1, -1, -1, -1);
        /** Raw PoGo ids; {@code -1} when the field was absent. {@code costume}/{@code gender} may be 0. */
        public final int form, costume, gender, size;
        /** Individual IV components 0..15; {@code -1} when absent or out of range. */
        public final int atkIv, defIv, staIv;
        /** Great/Ultra/Little league ranks when present and positive, else {@code -1}. */
        public final int pvpGreatRank, pvpUltraRank, pvpLittleRank;

        public Attrs(int form, int costume, int gender, int size, int atkIv, int defIv, int staIv,
                     int pvpGreatRank, int pvpUltraRank, int pvpLittleRank) {
            this.form = form;
            this.costume = costume;
            this.gender = gender;
            this.size = size;
            this.atkIv = atkIv;
            this.defIv = defIv;
            this.staIv = staIv;
            this.pvpGreatRank = pvpGreatRank;
            this.pvpUltraRank = pvpUltraRank;
            this.pvpLittleRank = pvpLittleRank;
        }

        /** True only when all three individual IVs are known, so a "a/d/s" readout is never partial. */
        public boolean hasIvComponents() { return atkIv >= 0 && defIv >= 0 && staIv >= 0; }
    }

    /**
     * PGRazorX: flag emoji of the country that ends a row's place ("Roanoke, Texas, United States" → 🇺🇸).
     * The place is the last " · " part of {@code details}; its last comma-separated part must name a
     * country (English, as the feed sends it), otherwise "" (no flag rather than a wrong one).
     */
    public static String countryFlag(String details) {
        if (details == null || details.isEmpty()) return "";
        int dot = details.lastIndexOf(" · ");
        String place = dot >= 0 ? details.substring(dot + 3) : details;
        int comma = place.lastIndexOf(',');
        String code = countryCode(comma >= 0 ? place.substring(comma + 1) : place);
        if (code == null) return "";
        return new String(Character.toChars(0x1F1E6 + code.charAt(0) - 'A')) + new String(Character.toChars(0x1F1E6 + code.charAt(1) - 'A'));
    }

    private static java.util.Map<String, String> countries;
    /** ISO 3166 alpha-2 code for an English country name; null when unknown. */
    static synchronized String countryCode(String name) {
        if (name == null) return null;
        String key = name.trim().toLowerCase(java.util.Locale.ROOT);
        if (key.startsWith("the ")) key = key.substring(4);
        if (key.isEmpty()) return null;
        if (countries == null) {
            java.util.Map<String, String> map = new java.util.HashMap<>();
            for (String code : java.util.Locale.getISOCountries()) {
                String english = new java.util.Locale("", code).getDisplayCountry(java.util.Locale.ENGLISH).toLowerCase(java.util.Locale.ROOT);
                if (!english.isEmpty()) map.put(english, code);
            }
            String[][] aliases = {{"usa", "US"}, {"united states of america", "US"}, {"united states", "US"}, {"uk", "GB"},
                {"united kingdom", "GB"}, {"great britain", "GB"}, {"england", "GB"}, {"scotland", "GB"}, {"wales", "GB"},
                {"northern ireland", "GB"}, {"south korea", "KR"}, {"korea", "KR"}, {"republic of korea", "KR"},
                {"north korea", "KP"}, {"russia", "RU"}, {"russian federation", "RU"}, {"taiwan", "TW"},
                {"hong kong", "HK"}, {"hong kong sar china", "HK"}, {"macau", "MO"}, {"macao", "MO"}, {"vietnam", "VN"},
                {"viet nam", "VN"}, {"czechia", "CZ"}, {"czech republic", "CZ"}, {"turkey", "TR"}, {"türkiye", "TR"},
                {"turkiye", "TR"}, {"ivory coast", "CI"}, {"côte d'ivoire", "CI"}, {"cote d'ivoire", "CI"},
                {"netherlands", "NL"}, {"holland", "NL"}, {"bahamas", "BS"}, {"myanmar", "MM"}, {"burma", "MM"},
                {"laos", "LA"}, {"brunei", "BN"}, {"north macedonia", "MK"}, {"macedonia", "MK"}, {"eswatini", "SZ"},
                {"swaziland", "SZ"}, {"cape verde", "CV"}, {"cabo verde", "CV"}, {"palestine", "PS"},
                {"palestinian territories", "PS"}, {"vatican city", "VA"}, {"bolivia", "BO"}, {"venezuela", "VE"},
                {"iran", "IR"}, {"syria", "SY"}, {"tanzania", "TZ"}, {"moldova", "MD"}, {"micronesia", "FM"},
                {"dr congo", "CD"}, {"democratic republic of the congo", "CD"}, {"republic of the congo", "CG"},
                {"congo", "CG"}, {"puerto rico", "PR"}, {"curacao", "CW"}, {"curaçao", "CW"}, {"saint martin", "MF"},
                {"sint maarten", "SX"}, {"east timor", "TL"}, {"timor-leste", "TL"}, {"micronesia (federated states of)", "FM"}};
            for (String[] alias : aliases) map.put(alias[0], alias[1]);
            countries = map;
        }
        String code = countries.get(key);
        return code != null && code.length() == 2 ? code : null;
    }

    /** One map object. Numeric fields use -1 and {@code iv}/{@code hasStats} to flag "unknown". */
    public static final class Entry {
        public final String id, name, details;
        public final Kind kind;
        public final double lat, lon;
        public final int dexId, cp;
        public final double level;
        public final double iv;
        public final long expiresAt;
        public final boolean hasStats;
        /** Raw PoGo team id for gyms/raids: 0 uncontested/unknown, 1 Mystic, 2 Valor, 3 Instinct. */
        public final int team;
        /** Raid egg hatch time (ms); 0 when not a raid or unknown. An egg is {@code raidHatchAt > now}. */
        public final long raidHatchAt;
        /** True only when a hatched boss was confirmed at fetch time; eggs never set this. Gates the live
         *  boss sprite so a stale egg row cannot flip to a predicted boss once its hatch time passes. */
        public final boolean raidBossLive;
        /** True only for rows that came from the My Shinies endpoint (the signed-in account's own
         *  shinies). Lets the UI exempt them from the viewport IV/level filter; it is not a claim about
         *  any unlinked Pokémon GO account. */
        public final boolean personalShiny;
        /** Optional observed detail attributes; never {@code null} ({@link Attrs#UNKNOWN} when absent). */
        public final Attrs attrs;
        /** Pokesearch reveal code for a row whose coordinates are not known yet; null otherwise. */
        public final String revealCode;

        /** Backwards-compatible overload (no team/raid metadata) kept for existing call sites and tests. */
        public Entry(String id, Kind kind, String name, String details, double lat, double lon,
                     int dexId, int cp, double level, double iv, long expiresAt, boolean hasStats) {
            this(id, kind, name, details, lat, lon, dexId, cp, level, iv, expiresAt, hasStats, 0, 0L, false);
        }

        /** Backwards-compatible overload with team/raid metadata but no shiny/attrs extension. */
        public Entry(String id, Kind kind, String name, String details, double lat, double lon,
                     int dexId, int cp, double level, double iv, long expiresAt, boolean hasStats,
                     int team, long raidHatchAt, boolean raidBossLive) {
            this(id, kind, name, details, lat, lon, dexId, cp, level, iv, expiresAt, hasStats,
                    team, raidHatchAt, raidBossLive, false, Attrs.UNKNOWN);
        }

        public Entry(String id, Kind kind, String name, String details, double lat, double lon,
                     int dexId, int cp, double level, double iv, long expiresAt, boolean hasStats,
                     int team, long raidHatchAt, boolean raidBossLive, boolean personalShiny, Attrs attrs) {
            this(id, kind, name, details, lat, lon, dexId, cp, level, iv, expiresAt, hasStats,
                    team, raidHatchAt, raidBossLive, personalShiny, attrs, null);
        }

        public Entry(String id, Kind kind, String name, String details, double lat, double lon,
                     int dexId, int cp, double level, double iv, long expiresAt, boolean hasStats,
                     int team, long raidHatchAt, boolean raidBossLive, boolean personalShiny, Attrs attrs,
                     String revealCode) {
            this.revealCode = revealCode;
            this.id = id;
            this.kind = kind;
            this.name = name;
            this.details = details;
            this.lat = lat;
            this.lon = lon;
            this.dexId = dexId;
            this.cp = cp;
            this.level = level;
            this.iv = iv;
            this.expiresAt = expiresAt;
            this.hasStats = hasStats;
            this.team = team;
            this.raidHatchAt = raidHatchAt;
            this.raidBossLive = raidBossLive;
            this.personalShiny = personalShiny;
            this.attrs = attrs == null ? Attrs.UNKNOWN : attrs;
        }

        /** Serialises this row so the feed list can survive closing the feed / app restarts. */
        public JSONObject toJson() throws JSONException {
            JSONObject o = new JSONObject();
            o.put("id", id); o.put("kind", kind.name()); o.put("name", name); o.put("details", details);
            if (hasCoords()) { o.put("lat", lat); o.put("lon", lon); }
            o.put("dex", dexId); o.put("cp", cp); o.put("level", level); o.put("iv", iv);
            o.put("exp", expiresAt); o.put("stats", hasStats); o.put("team", team);
            o.put("hatch", raidHatchAt); o.put("bossLive", raidBossLive); o.put("shiny", personalShiny);
            o.put("a", new JSONArray().put(attrs.form).put(attrs.costume).put(attrs.gender).put(attrs.size)
                    .put(attrs.atkIv).put(attrs.defIv).put(attrs.staIv)
                    .put(attrs.pvpGreatRank).put(attrs.pvpUltraRank).put(attrs.pvpLittleRank));
            if (revealCode != null) o.put("code", revealCode);
            return o;
        }

        /** Inverse of {@link #toJson()}; null for a malformed row. */
        public static Entry fromJson(JSONObject o) {
            try {
                Kind kind = Kind.valueOf(o.getString("kind"));
                JSONArray a = o.optJSONArray("a");
                Attrs attrs = a == null || a.length() < 10 ? Attrs.UNKNOWN : new Attrs(a.getInt(0), a.getInt(1),
                        a.getInt(2), a.getInt(3), a.getInt(4), a.getInt(5), a.getInt(6), a.getInt(7), a.getInt(8), a.getInt(9));
                String code = o.optString("code", "");
                return new Entry(o.getString("id"), kind, o.optString("name", ""), o.optString("details", ""),
                        o.optDouble("lat", Double.NaN), o.optDouble("lon", Double.NaN), o.optInt("dex", -1),
                        o.optInt("cp", -1), o.optDouble("level", -1), o.optDouble("iv", -1), o.optLong("exp", 0),
                        o.optBoolean("stats", false), o.optInt("team", 0), o.optLong("hatch", 0),
                        o.optBoolean("bossLive", false), o.optBoolean("shiny", false), attrs,
                        code.isEmpty() ? null : code);
            } catch (Exception e) { return null; }
        }

        /** True when the coordinates are known (not a locked Pokesearch row). */
        public boolean hasCoords() { return Double.isFinite(lat) && Double.isFinite(lon); }

        /** Copy of this row with revealed coordinates (and no pending reveal code). */
        public Entry withCoords(double newLat, double newLon) {
            return new Entry(id, kind, name, details, newLat, newLon, dexId, cp, level, iv, expiresAt, hasStats,
                    team, raidHatchAt, raidBossLive, personalShiny, attrs, null);
        }

        /**
         * Copy of this (newer) row that also carries the PvP league rank(s) {@code older} had and this
         * one lacks, e.g. a Pokémon found in the Great League search and again in the Ultra League one.
         * Ranks go to the attrs and the "GL #… · UL #…" segments are shown together, GL first.
         */
        public Entry withPvpFrom(Entry older) {
            if (older == null || older == this) return this;
            int gl = attrs.pvpGreatRank > 0 ? attrs.pvpGreatRank : older.attrs.pvpGreatRank;
            int ul = attrs.pvpUltraRank > 0 ? attrs.pvpUltraRank : older.attrs.pvpUltraRank;
            if (gl == attrs.pvpGreatRank && ul == attrs.pvpUltraRank) return this;
            String glSeg = null, ulSeg = null;
            List<String> rest = new ArrayList<>();
            for (String seg : splitDetails(details)) {
                if (seg.startsWith("GL #")) glSeg = seg; else if (seg.startsWith("UL #")) ulSeg = seg; else rest.add(seg);
            }
            for (String seg : splitDetails(older.details)) {
                if (glSeg == null && seg.startsWith("GL #")) glSeg = seg;
                if (ulSeg == null && seg.startsWith("UL #")) ulSeg = seg;
            }
            int at = 0; // right after the "x% IV" segment when there is one
            for (int i = 0; i < rest.size(); i++) if (rest.get(i).endsWith("% IV")) { at = i + 1; break; }
            if (ulSeg != null) rest.add(at, ulSeg);
            if (glSeg != null) rest.add(at, glSeg);
            Attrs a = new Attrs(attrs.form, attrs.costume, attrs.gender, attrs.size, attrs.atkIv, attrs.defIv,
                    attrs.staIv, gl, ul, attrs.pvpLittleRank);
            return new Entry(id, kind, name, join(rest), lat, lon, dexId, cp, level, iv, expiresAt, hasStats,
                    team, raidHatchAt, raidBossLive, personalShiny, a, revealCode);
        }

        private static List<String> splitDetails(String d) {
            List<String> out = new ArrayList<>();
            if (d == null || d.isEmpty()) return out;
            for (String p : d.split(" · ")) if (!p.isEmpty()) out.add(p);
            return out;
        }

        /** True only when a real future expiry was known and has now passed. */
        public boolean expired(long now) { return expiresAt > 0 && expiresAt <= now; }

        /** Human-readable one-line detail, folding in the live countdown when an expiry is known. */
        public String subtitle(long now) {
            StringBuilder sb = new StringBuilder();
            if (details != null && !details.isEmpty()) sb.append(details);
            if (expiresAt > 0) {
                long remaining = expiresAt - now;
                if (sb.length() > 0) sb.append(" · ");
                if (remaining <= 0) sb.append(kind == Kind.POKEMON ? "despawned" : "ended");
                else sb.append(kind == Kind.POKEMON ? "despawns in " : "ends in ").append(duration(remaining));
            }
            return sb.toString();
        }
    }

    public static final class Snapshot {
        public final List<Entry> entries;
        public final long fetchedAt;
        public final int refreshSeconds;
        public final String note;

        public Snapshot(List<Entry> entries, long fetchedAt, int refreshSeconds, String note) {
            this.entries = Collections.unmodifiableList(new ArrayList<>(entries));
            this.fetchedAt = fetchedAt;
            this.refreshSeconds = refreshSeconds > 0 ? refreshSeconds : DEFAULT_REFRESH_SECONDS;
            this.note = note == null ? "" : note;
        }
    }

    private LiveFeedData() { }

    public static Snapshot parse(Object response, long now) throws JSONException {
        if (response instanceof JSONArray) {
            List<Entry> entries = new ArrayList<>();
            Set<String> ids = new HashSet<>();
            parsePokemon((JSONArray) response, now, entries, ids);
            return new Snapshot(entries, now, DEFAULT_REFRESH_SECONDS, "");
        }
        if (response instanceof JSONObject) {
            return parse((JSONObject) response, now);
        }
        return new Snapshot(Collections.emptyList(), now, DEFAULT_REFRESH_SECONDS, "");
    }

    /**
     * Parses a viewport response. Accepts either the {@code {success,data:{...}}} envelope or a bare
     * {@code {forts,pokemon,stations}} object (user-supplied map data). Exposed for unit tests.
     */
    public static Snapshot parse(JSONObject response, long now) throws JSONException {
        JSONObject root = response;
        JSONObject data = response.optJSONObject("data");
        if (data != null) root = data;

        List<Entry> entries = new ArrayList<>();
        Set<String> ids = new HashSet<>();
        boolean truncated = parsePokemon(root.optJSONArray("pokemon"), now, entries, ids);
        truncated |= parseForts(root.optJSONArray("forts"), now, entries, ids);
        truncated |= parseStations(root.optJSONArray("stations"), now, entries, ids);

        int refresh = root.optInt("refresh_seconds", root.optInt("viewport_refresh_seconds", DEFAULT_REFRESH_SECONDS));
        String note = "";
        if (truncated) note = "Showing the first " + MAX_ENTRIES + " results.";
        else if (root.optBoolean("limit_reached", false)) note = "The service limited how many objects it returned.";
        // Surface the viewport's own shiny-access signal so the My Shinies tab can show a truthful
        // "not enabled / still loading" state; only act on it when the field is explicitly present.
        String shiny = shinyAccessNote(root);
        if (!shiny.isEmpty()) note = note.isEmpty() ? shiny : note + "\n" + shiny;
        return new Snapshot(entries, now, refresh, note);
    }

    /**
     * Parses a My Shinies response ({@code POST /api/game/my_shinies}). The {@code shinies} array uses
     * a different schema from the viewport: {@code despawn_ts} is epoch <em>seconds</em> (not millis),
     * {@code iv} is already the percentage, and each row carries individual IVs/size/form. Entries keep
     * {@link Kind#POKEMON} so the species sprite resolves, flag {@link Entry#personalShiny}, and preserve
     * the 64-bit encounter id as a string. {@code cooldown_sec} is a service estimate and is ignored on
     * purpose so it can never masquerade as the local action cooldown. Exposed for unit tests.
     */
    public static Snapshot parseShinies(JSONObject response, long now) throws JSONException {
        JSONObject root = response;
        JSONObject data = response.optJSONObject("data");
        if (data != null) root = data;

        List<Entry> entries = new ArrayList<>();
        Set<String> ids = new HashSet<>();
        boolean truncated = parseShinyArray(root.optJSONArray("shinies"), now, entries, ids);

        int refresh = boundRefresh(root.optInt("refresh_sec", 30));
        String note = truncated ? "Showing the first " + MAX_ENTRIES + " results." : "";
        String access = shinyAccessNote(root);
        if (!access.isEmpty()) note = note.isEmpty() ? access : note + "\n" + access;
        return new Snapshot(entries, now, refresh, note);
    }

    private static String shinyAccessNote(JSONObject root) {
        if (root.has("shiny_access") && !root.isNull("shiny_access") && !root.optBoolean("shiny_access", true))
            return "Shiny access is not enabled for this account.";
        if (root.has("shiny_ready") && !root.isNull("shiny_ready") && !root.optBoolean("shiny_ready", true))
            return "Shinies are still loading — try again shortly.";
        return "";
    }

    // --- pokemon ---------------------------------------------------------------------------------

    private static boolean parsePokemon(JSONArray array, long now, List<Entry> out, Set<String> ids) {
        if (array == null) return false;
        int limit = Math.min(array.length(), MAX_ENTRIES);
        for (int i = 0; i < limit; i++) {
            if (out.size() >= MAX_ENTRIES) return true;
            JSONObject o = array.optJSONObject(i);
            if (o == null) continue;
            double lat = o.optDouble("lat", o.optDouble("latitude", Double.NaN));
            double lon = o.optDouble("lon", o.optDouble("longitude", o.optDouble("lng", Double.NaN)));
            String revealCode = o.optString("reveal_code", "");
            if (!validCoords(lat, lon)) {
                if (revealCode.isEmpty()) continue;
                lat = Double.NaN; lon = Double.NaN; // locked: coordinates fetched on demand
            } else revealCode = "";
            int dex = o.optInt("dex_id", o.optInt("dexId", o.optInt("pokemon_id", o.optInt("pokemonId", 0))));
            // Preserve the 64-bit encounter id as a string; fallback to dex:lat:lon if anonymous.
            String encounter = o.optString("encounter_id", o.optString("encounterId", o.optString("id", o.optString("spawn_id", ""))));
            if (encounter.isEmpty()) {
                if (dex <= 0) continue;
                encounter = dex + ":" + lat + ":" + lon;
            }
            long despawn = o.optLong("despawn_ms", o.optLong("despawnMs", 0));
            if (despawn <= 0 && o.has("despawn_time")) despawn = toMillisFromEpochSeconds(o.optLong("despawn_time", 0));
            if (despawn <= 0 && o.has("despawn_ts")) despawn = toMillisFromEpochSeconds(o.optLong("despawn_ts", 0));
            if (despawn <= 0 && o.has("despawn")) despawn = toMillisFromEpochSeconds(o.optLong("despawn", 0));
            if (despawn <= 0) despawn = now + 15 * 60 * 1000L; // default 15m active window if unlisted
            if (despawn < 0 || despawn <= now) continue; // invalid or already despawned

            boolean isShiny = o.optBoolean("is_shiny", o.optBoolean("shiny", false));
            String id = (isShiny ? "shiny:" : "pokemon:") + encounter;
            if (!ids.add(id)) continue;

            boolean hasStats = o.has("has_full_stats") ? o.optBoolean("has_full_stats", false) : (o.has("iv") || o.has("iv_percent"));
            int cp = hasStats ? (o.has("cp") && !o.isNull("cp") ? o.optInt("cp", -1) : -1) : -1;
            double level = hasStats ? (o.has("level") && !o.isNull("level") ? o.optDouble("level", -1) : -1) : -1;
            if (!Double.isFinite(level) || level <= 0 || level > 55) level = -1;
            double iv = -1;
            if (hasStats) {
                iv = !o.isNull("iv_percent") ? o.optDouble("iv_percent", -1) : (!o.isNull("iv") ? o.optDouble("iv", -1) : -1);
                if (!Double.isFinite(iv) || iv < 0 || iv > 100) iv = -1;
            }

            String name = clean(o.optString("pokemon_name", o.optString("pokemonName", o.optString("name", ""))));
            if (name.isEmpty()) name = dex > 0 ? "#" + dex : "Unknown Pokémon";
            if (isShiny && !name.startsWith("✨")) name = "✨ " + name;

            String details;
            if (hasStats && iv >= 0) {
                StringBuilder d = new StringBuilder();
                if (cp >= 0) d.append("CP ").append(cp);
                if (level >= 0) { if (d.length() > 0) d.append(" · "); d.append("Lv ").append(level == Math.rint(level) ? Long.toString((long)level) : Double.toString(level)); }
                if (d.length() > 0) d.append(" · ");
                d.append(String.format(java.util.Locale.US, "%.1f%% IV", iv));
                details = d.toString();
            } else {
                details = isShiny ? "Shiny Wild" : "Wild · stats hidden";
            }
            // Optional extra line from the Pokesearch API (size · gender · moves · place).
            String extra = clean(o.optString("extra_details", ""));
            if (!extra.isEmpty()) details = details + " · " + extra;
            int atk = optIvComponent(o, "iv_attack"); if (atk < 0) atk = optIvComponent(o, "atk_iv");
            int def = optIvComponent(o, "iv_defense"); if (def < 0) def = optIvComponent(o, "def_iv");
            int sta = optIvComponent(o, "iv_stamina"); if (sta < 0) sta = optIvComponent(o, "sta_iv");
            Attrs attrs = new Attrs(optRawId(o, "form"), optRawId(o, "costume"), optRawId(o, "gender"),
                    optRawId(o, "size"),
                    hasStats ? atk : -1,
                    hasStats ? def : -1,
                    hasStats ? sta : -1,
                    optRank(o, "pvp_great_rank"), optRank(o, "pvp_ultra_rank"), optRank(o, "pvp_little_rank"));
            out.add(new Entry(id, Kind.POKEMON, name, details, lat, lon, dex, cp, level, iv, despawn,
                    hasStats && iv >= 0, 0, 0L, false, isShiny, attrs, revealCode.isEmpty() ? null : revealCode));
        }
        return false;
    }

    // --- my shinies ------------------------------------------------------------------------------

    private static boolean parseShinyArray(JSONArray array, long now, List<Entry> out, Set<String> ids) {
        if (array == null) return false;
        int limit = Math.min(array.length(), MAX_ENTRIES);
        for (int i = 0; i < limit; i++) {
            if (out.size() >= MAX_ENTRIES) return true;
            JSONObject o = array.optJSONObject(i);
            if (o == null) continue;
            double lat = o.optDouble("lat", Double.NaN), lon = o.optDouble("lon", Double.NaN);
            if (!validCoords(lat, lon)) continue;
            // Preserve the 64-bit encounter id as a string; never narrow it through a numeric type.
            String encounter = o.optString("encounter_id", "");
            if (encounter.isEmpty()) continue;
            // despawn_ts is epoch SECONDS here; convert to millis with an overflow guard.
            long despawn = toMillisFromEpochSeconds(o.optLong("despawn_ts", 0));
            if (despawn < 0 || (despawn > 0 && despawn <= now)) continue; // invalid or already despawned

            String id = "shiny:" + encounter;
            if (!ids.add(id)) continue;

            int dex = o.optInt("dex_id", 0);
            double iv = o.isNull("iv") ? -1 : o.optDouble("iv", -1); // already a percentage in this schema
            if (!Double.isFinite(iv) || iv < 0 || iv > 100) iv = -1;
            int cp = o.has("cp") && !o.isNull("cp") ? o.optInt("cp", -1) : -1;
            if (cp < 0) cp = -1;
            double level = o.optDouble("level", -1);
            if (!Double.isFinite(level) || level <= 0 || level > 55) level = -1;

            int atk = optIvComponent(o, "atk_iv"), def = optIvComponent(o, "def_iv"), sta = optIvComponent(o, "sta_iv");
            // pvp_rank1 is nullable and its league is ambiguous in the capture, so no rank is claimed.
            Attrs attrs = new Attrs(optRawId(o, "form"), optRawId(o, "costume"), optRawId(o, "gender"),
                    optRawId(o, "size"), atk, def, sta, -1, -1, -1);

            String name = clean(o.optString("name", ""));
            if (name.isEmpty()) name = dex > 0 ? "#" + dex : "Shiny";

            StringBuilder d = new StringBuilder();
            if (cp >= 0) d.append("CP ").append(cp);
            if (level >= 0) { if (d.length() > 0) d.append(" · "); d.append("Lv ").append(fmtLevel(level)); }
            if (iv >= 0) { if (d.length() > 0) d.append(" · "); d.append(String.format(java.util.Locale.US, "%.1f%% IV", iv)); }
            if (attrs.hasIvComponents()) { if (d.length() > 0) d.append(" · "); d.append(atk).append('/').append(def).append('/').append(sta); }
            String details = d.length() > 0 ? d.toString() : "Shiny";

            boolean hasStats = iv >= 0;
            out.add(new Entry(id, Kind.POKEMON, name, details, lat, lon, dex, cp, level, iv, despawn, hasStats,
                    0, 0L, false, true, attrs));
        }
        return false;
    }

    // --- forts (stops / gyms / raids) ------------------------------------------------------------

    private static boolean parseForts(JSONArray array, long now, List<Entry> out, Set<String> ids) {
        if (array == null) return false;
        int limit = Math.min(array.length(), MAX_ENTRIES);
        for (int i = 0; i < limit; i++) {
            if (out.size() >= MAX_ENTRIES) return true;
            JSONObject o = array.optJSONObject(i);
            if (o == null) continue;
            double lat = o.optDouble("lat", Double.NaN), lon = o.optDouble("lon", Double.NaN);
            if (!validCoords(lat, lon)) continue;
            String fortId = o.optString("fort_id", "");
            if (fortId.isEmpty()) continue;
            String type = o.optString("fort_type", "");

            if ("CHECKPOINT".equals(type) || "POKESTOP".equals(type)) {
                addStop(o, fortId, lat, lon, now, out, ids);
            } else if ("GYM".equals(type)) {
                addGymOrRaid(o, fortId, lat, lon, now, out, ids);
            }
            // Unknown fort types are intentionally skipped rather than guessed at.
        }
        return false;
    }

    private static void addStop(JSONObject o, String fortId, double lat, double lon, long now,
                                List<Entry> out, Set<String> ids) {
        String id = "stop:" + fortId;
        if (!ids.add(id)) return;
        String name = clean(o.optString("name", ""));
        if (name.isEmpty()) name = "Pokéstop";

        // Only surface a feature when it is explicitly flagged true and its expiry is still in the future.
        List<String> feats = new ArrayList<>();
        long lure = o.optLong("lure_expire_ms", 0);
        if (o.optBoolean("has_lure", false) && lure > now) feats.add("Lure " + duration(lure - now));
        long invasion = o.optLong("invasion_expire_ms", 0);
        if (o.optBoolean("has_invasion", false) && invasion > now) feats.add("Invasion " + duration(invasion - now));
        long kecleon = o.optLong("kecleon_expire_ms", 0);
        if (o.optBoolean("has_kecleon", false) && kecleon > now) feats.add("Kecleon " + duration(kecleon - now));

        String details = feats.isEmpty() ? "Pokéstop" : join(feats);
        // The stop itself persists, so it has no expiry; feature countdowns live in the detail text.
        out.add(new Entry(id, Kind.STOP, name, details, lat, lon, -1, -1, -1, -1, 0, false));
    }

    private static void addGymOrRaid(JSONObject o, String fortId, double lat, double lon, long now,
                                     List<Entry> out, Set<String> ids) {
        String name = clean(o.optString("name", ""));
        if (name.isEmpty()) name = "Gym";
        int team = o.optInt("team", 0);
        long raidEnd = o.optLong("raid_end_ms", 0);

        if (raidEnd > now) {
            String id = "raid:" + fortId;
            if (!ids.add(id)) return;
            int rawLevel = o.optInt("raid_level", 0);
            long hatch = o.optLong("raid_battle_ms", 0);
            int boss = o.has("raid_pokemon_id") && !o.isNull("raid_pokemon_id") ? o.optInt("raid_pokemon_id", -1) : -1;
            int cp = o.has("raid_pokemon_cp") && !o.isNull("raid_pokemon_cp") ? o.optInt("raid_pokemon_cp", -1) : -1;

            StringBuilder d = new StringBuilder();
            boolean egg = hatch > now; // not yet hatched
            d.append(teamName(team)).append(" \u00b7 ").append(occupancy(o)).append(" \u00b7 ").append(egg ? "Raid egg" : "Raid");
            if (rawLevel > 0) d.append(" · Raid type ").append(rawLevel); // no invented star rating
            if (boss > 0) d.append(" · #").append(boss);
            else if (!egg) d.append(" · boss unknown");
            if (!egg && cp > 0) d.append(" · CP ").append(cp);

            // Only a hatched raid with a boss id is a confirmed live boss; an egg's boss id is a prediction
            // and must never become a live sprite once this stale row's hatch time quietly passes.
            boolean bossLive = !egg && boss > 0;
            out.add(new Entry(id, Kind.RAID, name, d.toString(), lat, lon, boss, cp, rawLevel, -1, raidEnd, false,
                    team, hatch, bossLive));
            return;
        }

        String id = "gym:" + fortId;
        if (!ids.add(id)) return;
        StringBuilder d = new StringBuilder(teamName(team));
        d.append(" · ").append(occupancy(o));
        if (o.optBoolean("in_battle", false)) d.append(" · in battle");
        out.add(new Entry(id, Kind.GYM, name, d.toString(), lat, lon, -1, -1, -1, -1, 0, false, team, 0L, false));
    }

    private static String occupancy(JSONObject o) {
        if (o.has("slots_available") && !o.isNull("slots_available")) {
            int free = o.optInt("slots_available", -1);
            if (free >= 0 && free <= 6) return (6 - free) + "/6"; // occupied of six, only when validated
        }
        return "--/6"; // null / out-of-range => unknown rather than fabricated
    }

    private static String teamName(int team) {
        switch (team) {
            case 1: return "Mystic";
            case 2: return "Valor";
            case 3: return "Instinct";
            case 0: return "Uncontested";
            default: return "Team unknown";
        }
    }

    // --- stations --------------------------------------------------------------------------------

    private static boolean parseStations(JSONArray array, long now, List<Entry> out, Set<String> ids) {
        if (array == null) return false;
        int limit = Math.min(array.length(), MAX_ENTRIES);
        for (int i = 0; i < limit; i++) {
            if (out.size() >= MAX_ENTRIES) return true;
            JSONObject o = array.optJSONObject(i);
            if (o == null) continue;
            double lat = o.optDouble("lat", Double.NaN), lon = o.optDouble("lon", Double.NaN);
            if (!validCoords(lat, lon)) continue;
            String stationId = o.optString("station_id", "");
            if (stationId.isEmpty()) continue;
            long end = o.optLong("end_ms", 0);
            if (end > 0 && end <= now) continue; // expired station ignored

            String id = "station:" + stationId;
            if (!ids.add(id)) continue;
            String name = clean(o.optString("name", ""));
            if (name.isEmpty()) name = "Power Station";
            int level = o.optInt("battle_level", 0);
            int boss = o.has("boss_dex_id") && !o.isNull("boss_dex_id") ? o.optInt("boss_dex_id", -1) : -1;

            StringBuilder d = new StringBuilder("Max battle");
            if (level > 0) d.append(" Lv").append(level); // battle-kind semantics unverified; show level only
            if (boss > 0) d.append(" · #").append(boss);
            out.add(new Entry(id, Kind.STATION, name, d.toString(), lat, lon, boss, -1, level, -1, end, false));
        }
        return false;
    }

    // --- shared helpers --------------------------------------------------------------------------

    private static boolean validCoords(double lat, double lon) {
        return Double.isFinite(lat) && Double.isFinite(lon)
                && lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180
                && !(Math.abs(lat) < 1e-9 && Math.abs(lon) < 1e-9); // drop null-island placeholders
    }

    /** Clamps a server-advertised refresh cadence (seconds) into a sane client polling window. */
    private static int boundRefresh(int seconds) {
        if (seconds <= 0) return DEFAULT_REFRESH_SECONDS;
        return seconds < MIN_REFRESH_SECONDS ? MIN_REFRESH_SECONDS
                : (seconds > MAX_REFRESH_SECONDS ? MAX_REFRESH_SECONDS : seconds);
    }

    /**
     * Converts an epoch-seconds timestamp to epoch-millis. Returns 0 (unknown) for non-positive input,
     * guards against multiply overflow, and passes a value that is already in the millisecond range
     * through unchanged so a mislabelled field is never double-scaled into the far future.
     */
    private static long toMillisFromEpochSeconds(long ts) {
        if (ts <= 0) return 0;
        if (ts > Long.MAX_VALUE / 1000L) return -1;
        if (ts >= 100_000_000_000L) return ts; // tolerate a mislabeled millisecond timestamp
        return ts * 1000L;
    }

    /** Raw id-like field (form/costume/gender/size): kept when present and non-negative, else -1. */
    private static int optRawId(JSONObject o, String key) {
        if (!o.has(key) || o.isNull(key)) return -1;
        int v = o.optInt(key, -1);
        return v >= 0 ? v : -1;
    }

    /** Individual IV component (0..15); -1 when absent or out of range. */
    private static int optIvComponent(JSONObject o, String key) {
        if (!o.has(key) || o.isNull(key)) return -1;
        int v = o.optInt(key, -1);
        return v >= 0 && v <= 15 ? v : -1;
    }

    /** Optional league rank (>=1); -1 when absent or non-positive. */
    private static int optRank(JSONObject o, String key) {
        if (!o.has(key) || o.isNull(key)) return -1;
        int v = o.optInt(key, -1);
        return v >= 1 ? v : -1;
    }

    private static String fmtLevel(double level) {
        return level == Math.rint(level) ? Long.toString((long) level) : Double.toString(level);
    }

    private static String join(List<String> parts) {
        StringBuilder sb = new StringBuilder();
        for (String p : parts) { if (sb.length() > 0) sb.append(" · "); sb.append(p); }
        return sb.toString();
    }

    /** Formats a positive millisecond span as a compact, human countdown. */
    static String duration(long ms) {
        if (ms < 0) ms = 0;
        long totalSeconds = ms / 1000;
        if (totalSeconds < 60) return totalSeconds + "s";
        long minutes = totalSeconds / 60;
        if (minutes < 60) { long s = totalSeconds % 60; return s > 0 ? minutes + "m " + s + "s" : minutes + "m"; }
        long hours = minutes / 60; long m = minutes % 60;
        return m > 0 ? hours + "h " + m + "m" : hours + "h";
    }

    /** Trims, strips control characters, and bounds length so names/details stay plain-text safe. */
    static String clean(String raw) {
        if (raw == null) return "";
        String trimmed = raw.trim();
        if (trimmed.length() > 120) trimmed = trimmed.substring(0, 120);
        StringBuilder sb = new StringBuilder(trimmed.length());
        for (int i = 0; i < trimmed.length(); i++) {
            char c = trimmed.charAt(i);
            sb.append(Character.isISOControl(c) ? ' ' : c);
        }
        return sb.toString().trim();
    }
}
