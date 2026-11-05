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
 * Immutable, UI-agnostic model for LivePokeMap viewport results plus the parser that turns a raw
 * {@code /api/map/viewport} (or user-supplied map data) response into bounded, sanitised entries.
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

    public enum Kind { POKEMON, STOP, GYM, RAID, STATION }

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
        /** Always false: it marked rows from LivePokeMap's former My Shinies endpoint (constructor
         *  compatibility for existing callers and tests). */
        public final boolean personalShiny;
        /** Optional observed detail attributes; never {@code null} ({@link Attrs#UNKNOWN} when absent). */
        public final Attrs attrs;

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
        // The service picks a detail tier from the search span; at the widest ("perfect") it only sends
        // 100% IV Pokémon, whatever the display filters ask for.
        if ("perfect".equals(root.optString("span_tier", "")))
            note = (note.isEmpty() ? "" : note + " ") + "At this range LivePokeMap only sends 100% IV Pokémon.";
        return new Snapshot(entries, now, refresh, note);
    }

    // --- pokemon ---------------------------------------------------------------------------------

    private static boolean parsePokemon(JSONArray array, long now, List<Entry> out, Set<String> ids) {
        if (array == null) return false;
        int limit = Math.min(array.length(), MAX_ENTRIES);
        for (int i = 0; i < limit; i++) {
            if (out.size() >= MAX_ENTRIES) return true;
            JSONObject o = array.optJSONObject(i);
            if (o == null) continue;
            double lat = o.optDouble("lat", Double.NaN), lon = o.optDouble("lon", Double.NaN);
            if (!validCoords(lat, lon)) continue;
            // Preserve the 64-bit encounter id as a string; never narrow it through a numeric type.
            String encounter = o.optString("encounter_id", o.optString("encounterId", ""));
            if (encounter.isEmpty()) continue; // cannot de-duplicate an anonymous spawn
            long despawn = o.optLong("despawn_ms", o.optLong("despawnMs", 0));
            if (despawn < 0 || (despawn > 0 && despawn <= now)) continue; // invalid or already despawned

            String id = "pokemon:" + encounter;
            if (!ids.add(id)) continue;

            int dex = o.optInt("dex_id", o.optInt("dexId", 0));
            String source = o.optString("source", "wild");
            boolean hasStats = o.optBoolean("has_full_stats", false);
            int cp = hasStats ? o.optInt("cp", -1) : -1;
            double level = hasStats ? o.optDouble("level", -1) : -1;
            if (!Double.isFinite(level) || level <= 0 || level > 55) level = -1;
            double iv = hasStats && !o.isNull("iv_percent") ? o.optDouble("iv_percent", -1) : -1;
            if (!Double.isFinite(iv) || iv < 0 || iv > 100) iv = -1;

            String name = clean(o.optString("pokemon_name", o.optString("pokemonName", "")));
            if (name.isEmpty()) name = dex > 0 ? "#" + dex : "Unknown Pokémon";

            String details;
            if (hasStats && iv >= 0) {
                StringBuilder d = new StringBuilder();
                if (cp >= 0) d.append("CP ").append(cp);
                if (level >= 0) { if (d.length() > 0) d.append(" · "); d.append("Lv ").append(level == Math.rint(level) ? Long.toString((long)level) : Double.toString(level)); }
                if (d.length() > 0) d.append(" · ");
                d.append(String.format(java.util.Locale.US, "%.1f%% IV", iv));
                appendRank(d, "GL", optRank(o, "pvp_great_rank"));
                appendRank(d, "UL", optRank(o, "pvp_ultra_rank"));
                appendRank(d, "LC", optRank(o, "pvp_little_rank"));
                details = d.toString();
            } else {
                details = "Wild · stats hidden";
            }
            // Observed optional attributes; individual IVs only trusted when full stats were reported.
            Attrs attrs = new Attrs(optRawId(o, "form"), optRawId(o, "costume"), optRawId(o, "gender"),
                    optRawId(o, "size"),
                    hasStats ? optIvComponent(o, "iv_attack") : -1,
                    hasStats ? optIvComponent(o, "iv_defense") : -1,
                    hasStats ? optIvComponent(o, "iv_stamina") : -1,
                    optRank(o, "pvp_great_rank"), optRank(o, "pvp_ultra_rank"), optRank(o, "pvp_little_rank"));
            out.add(new Entry(id, Kind.POKEMON, name, details, lat, lon, dex, cp, level, iv, despawn,
                    hasStats && iv >= 0, 0, 0L, false, false, attrs));
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
    /** PvP ranks worth showing in a row: the top 100 of a league. */
    static final int SHOWN_RANK_LIMIT = 100;

    private static void appendRank(StringBuilder d, String league, int rank) {
        if (rank >= 1 && rank <= SHOWN_RANK_LIMIT) d.append(" · ").append(league).append(" #").append(rank);
    }

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
