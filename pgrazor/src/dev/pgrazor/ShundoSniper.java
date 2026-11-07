package dev.pgrazor;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.graphics.drawable.ColorDrawable;
import android.os.Handler;
import android.os.Looper;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;
import java.util.ArrayDeque;
import java.util.HashSet;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.Locale;
import java.util.Map;
import java.util.Properties;
import java.util.Set;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import org.json.JSONArray;
import org.json.JSONObject;

/**
 * Shundo Sniper: teleports to 100% IV wild Pokémon from the LivePokeMap and PoGoCoordinates feeds, has the
 * native half (shundo.cpp) open each one in a background encounter to see whether it is shiny, and catches
 * only shinies. Opening an encounter is not a cooldown action, so it teleports freely, nearest first. A
 * shiny is held in its encounter where it was found until the cooldown estimate there reaches zero (an
 * open encounter outlives the spawn timer), then caught; after that the search goes on. VGP+ pauses while it is on. It is never saved as on: it is off
 * after every restart.
 */
public final class ShundoSniper {
    private ShundoSniper() {}
    static final long MIN_LEFT_MS = 150_000, RESULT_TIMEOUT_MS = 180_000, LIVE_MIN_MS = 15_000, POGO_MS = 30_000,
        SETTLE_MS = 2_000, RETRY_MS = 60_000;
    static final int MAX_CANDIDATES = 6000, RECENT = 8, MAX_SPECIES = 500;
    // The shortest time between two teleports, chosen by the user (seconds; 0 = right away).
    static final int[] GAPS = {0, 30, 60, 90};
    static final String[] GAP_LABELS = {"Right away", "30 seconds", "60 seconds", "90 seconds"};
    // The PGRazor feed: one server polls LivePokeMap's whole-map 100% IV list and publishes it as a compact
    // file, {"v":1,"fetched_ms":..,"interval_s":..,"hundos":[[encounter_id,dex,form,lat,lon,despawn_ms,cp,level],..]},
    // so no sign-in is needed. Read once a minute (about 145 KB gzipped).
    static final String FEED_URL = "https://feed.faustusfoundry.com/pgrazor/hundos.json";
    static final long FEED_MS = 60_000;

    /** One 100% IV spawn from a feed; coordinates are NaN until PoGoCoordinates reveals them. */
    static final class Target {
        final String key, encounter, name, source, revealCode; final int dex; final long expiresAt; double lat, lon;
        Target(String key, String encounter, int dex, String name, String source, double lat, double lon, long expiresAt, String revealCode) {
            this.key = key; this.encounter = encounter; this.dex = dex; this.name = name; this.source = source;
            this.lat = lat; this.lon = lon; this.expiresAt = expiresAt; this.revealCode = revealCode;
        }
        boolean located() { return CooldownState.valid(lat, lon); }
    }

    private static final Object LOCK = new Object();
    private static volatile boolean enabled;
    private static boolean useFeed = true, useLive = true, usePogo = true;
    // The species to search for (dex numbers); empty means every species.
    private static final java.util.TreeSet<Integer> species = new java.util.TreeSet<>();
    private static int gapSeconds;
    private static long lastTeleport;  // when the last teleport was made, ms
    private static volatile String feedNote = "";
    // The spawn being checked: the request number and encounter id native reads, and when it was sent.
    private static long request, targetSince, nextPick, nextLive, nextPogo, nextFeed;
    private static Target target;
    private static boolean teleporting;
    // A shiny found and held in its encounter; go once a catch here needs no cooldown.
    private static boolean holding, go;
    // The wild Pokemon the last "not found" saw, and where: the same ones seen again after a jump of over
    // 1 km mean the game stopped following teleports (seen after some fresh launches; a restart fixes it).
    private static Set<String> lastMissingSeen = new HashSet<>();
    private static double lastMissingLat = Double.NaN, lastMissingLon = Double.NaN;
    private static final Map<String, Target> candidates = new LinkedHashMap<>();
    // Spawns already picked (encounter id -> its despawn time, ms), so each is checked once. An id is only
    // kept until shortly after its spawn despawns (it cannot appear again) and is saved per account, so a
    // game restart does not check live spawns twice.
    private static final Map<String, Long> attempted = new LinkedHashMap<>();
    static final long TRIED_GRACE_MS = 5 * 60_000, TRIED_UNKNOWN_MS = 60 * 60_000;
    static final String TRIED_FILE = "shundo-tried.txt";
    private static boolean triedLoaded;
    private static final ArrayDeque<String> recent = new ArrayDeque<>();
    private static int checked, shinies, caught, teleports, missed;
    private static double targetKm;
    // The on-map card (ShundoOverlay): shown once the sniper is started; its close button stops the
    // sniper and hides it.
    private static volatile boolean overlayShown;
    private static String status = "";
    private static volatile String liveNote = "", pogoNote = "";
    private static ScheduledExecutorService worker;
    private static LiveFeedClient live;
    private static PogoFeedClient pogo;
    private static final Handler main = new Handler(Looper.getMainLooper());
    private static Context app;
    private static volatile JSONObject speciesNames;  // dex number -> name, loaded once on the worker

    public static void read(Properties p) {
        synchronized (LOCK) {
            useFeed = !"false".equals(p.getProperty("shundoFeed"));
            useLive = !"false".equals(p.getProperty("shundoLive"));
            usePogo = !"false".equals(p.getProperty("shundoPogo"));
            species.clear(); species.addAll(parseSpecies(p.getProperty("shundoSpecies", "")));
            gapSeconds = gap(p.getProperty("shundoGap", "0"));
        }
    }
    public static void write(Properties p) {
        synchronized (LOCK) {
            p.setProperty("shundoFeed", String.valueOf(useFeed));
            p.setProperty("shundoLive", String.valueOf(useLive));
            p.setProperty("shundoPogo", String.valueOf(usePogo));
            p.setProperty("shundoSpecies", speciesList());
            p.setProperty("shundoGap", String.valueOf(gapSeconds));
        }
    }
    /** A saved gap if it is one of GAPS, else 0 (right away). */
    static int gap(String raw) {
        for (int g : GAPS) if (String.valueOf(g).equals(raw)) return g;
        return 0;
    }
    private static String gapLabel() {
        synchronized (LOCK) {
            for (int i = 0; i < GAPS.length; i++) if (GAPS[i] == gapSeconds) return "Time between teleports: " + GAP_LABELS[i];
            return "Time between teleports: " + GAP_LABELS[0];
        }
    }
    /** The dex numbers of a comma list, ignoring anything that isn't one (1..2000), at most MAX_SPECIES. */
    static java.util.TreeSet<Integer> parseSpecies(String raw) {
        java.util.TreeSet<Integer> ids = new java.util.TreeSet<>();
        if (raw == null || raw.length() > 10_000) return ids;
        for (String token : raw.split(",")) {
            if (ids.size() >= MAX_SPECIES) break;
            String t = token.trim();
            if (!t.matches("[1-9][0-9]{0,3}")) continue;
            int id = Integer.parseInt(t);
            if (id <= 2000) ids.add(id);
        }
        return ids;
    }
    static String speciesList() {
        synchronized (LOCK) {
            StringBuilder b = new StringBuilder();
            for (int id : species) { if (b.length() > 0) b.append(','); b.append(id); }
            return b.toString();
        }
    }
    // The species picker's save: drops queued spawns of species no longer wanted and re-reads the feeds
    // now, so spawns of newly added species come in without waiting for the next poll.
    static void setSpecies(String value) {
        int count;
        synchronized (LOCK) {
            species.clear(); species.addAll(parseSpecies(value)); count = species.size();
            if (count > 0) candidates.values().removeIf(t -> !species.contains(t.dex));
            nextFeed = nextLive = nextPogo = 0;
            if (enabled && target == null) status = looking();
        }
        Control.saveSettings();
        Control.event("shundo-sniper", "species " + (count == 0 ? "all" : count + " selected"));
    }
    // Under LOCK: whether a spawn of this species is wanted (an unknown species is not, once a list is set).
    private static boolean wanted(int dex) { return species.isEmpty() || species.contains(dex); }
    // Under LOCK: the idle line, naming the species filter when one is set.
    private static String looking() {
        return species.isEmpty() ? "Looking for 100% IV Pokémon"
            : species.size() == 1 ? "Looking for 100% IV " + cachedName(species.first())
            : "Looking for 100% IV Pokémon (" + species.size() + " species)";
    }
    // A species name only if the table is already loaded (this runs on the main thread too).
    private static String cachedName(int dex) {
        JSONObject names = speciesNames;
        String name = names == null ? "" : names.optString(Integer.toString(dex), "");
        return name.isEmpty() ? "#" + dex : name;
    }
    private static String speciesLabel() {
        synchronized (LOCK) { return "Species: " + (species.isEmpty() ? "All" : species.size() + " selected"); }
    }
    public static boolean active() { return enabled; }

    /**
     * "S1|<request>|<encounter id>|<go>|<running>" while a spawn is being checked, else "S1|0|0|0|<running>"
     * (transport slot 19). running blocks the player's own taps on map Pokémon, PokéStops and gyms.
     */
    public static String config() {
        synchronized (LOCK) { return enabled && target != null ? "S1|" + request + "|" + target.encounter + "|" + (go ? 1 : 0) + "|1" : "S1|0|0|0|" + (enabled ? 1 : 0); }
    }

    static void setEnabled(Context context, boolean on) {
        synchronized (LOCK) {
            if (on == enabled) return;
            enabled = on;
            target = null; teleporting = false; candidates.clear(); nextPick = nextLive = nextPogo = nextFeed = 0;
            status = on ? looking() : "";
            if (on) overlayShown = true;
            if (on) {
                app = context.getApplicationContext();
                if (!triedLoaded) { triedLoaded = true; loadTried(System.currentTimeMillis()); }
                live = new LiveFeedClient(new LiveFeedSession(context.getApplicationContext()));
                pogo = new PogoFeedClient(LiveFeedSession.pogoCoordinates(context.getApplicationContext()));
                worker = Executors.newSingleThreadScheduledExecutor();
                worker.scheduleWithFixedDelay(ShundoSniper::tick, 0, 1, TimeUnit.SECONDS);
            } else stopWorker();
        }
        Control.event("shundo-sniper", on ? "ON" : "OFF");
    }

    // Under LOCK: stops the feed worker and its network clients.
    private static void stopWorker() {
        if (worker != null) worker.shutdownNow();
        if (live != null) live.cancel();
        if (pogo != null) pogo.cancel();
        worker = null; live = null; pogo = null;
        lastMissingSeen = new HashSet<>(); lastMissingLat = lastMissingLon = Double.NaN;
    }

    /** Native "shundo-sniper" events for the spawn being checked. */
    public static void publish(String detail) {
        try {
            JSONObject o = new JSONObject(detail);
            if ("blocked".equals(o.optString("phase"))) { blockedToast(o.optString("what")); return; }
            synchronized (LOCK) {
                if (target == null || o.optLong("request") != request) return;
                String phase = o.optString("phase"), name = target.name, message = o.optString("message");
                switch (phase) {
                    case "throttle": case "refresh-failed": case "blocked": case "block-unavailable": return;  // diagnostics, not a result
                    case "looking": targetSince = System.currentTimeMillis(); status = "Checking " + name + " (" + target.source + ")"; return;
                    case "shiny": shinies++; holding = true; go = false; targetSince = System.currentTimeMillis(); status = "Shiny " + name + "! Waiting for cooldown"; return;
                    case "checked": checked++; lastMissingSeen = new HashSet<>(); lastMissingLat = lastMissingLon = Double.NaN; done(name + " " + ivs(o) + " · not shiny"); return;
                    case "caught": caught++; done(name + " · shiny, caught (" + message + ")"); return;
                    case "fled": done(name + " · shiny, " + message.toLowerCase(Locale.US)); return;
                    case "missing": {
                        Set<String> seen = new HashSet<>();
                        JSONArray ids = o.optJSONArray("ids");
                        for (int i = 0; ids != null && i < ids.length(); i++) seen.add(ids.optString(i));
                        boolean moved = CooldownState.valid(lastMissingLat, lastMissingLon)
                            && CooldownState.distance(lastMissingLat, lastMissingLon, target.lat, target.lon) > 1;
                        boolean stuck = moved && !seen.isEmpty() && !java.util.Collections.disjoint(seen, lastMissingSeen);
                        lastMissingSeen = seen; lastMissingLat = target.lat; lastMissingLon = target.lon;
                        missed++;
                        done(name + " · not found here");
                        if (stuck) {
                            enabled = false; stopWorker();
                            status = "Stopped: the game stopped following teleports. Restart Pokémon GO, then turn Shundo Sniper on again.";
                            Control.event("shundo-sniper", "OFF: game not following teleports");
                        }
                        return;
                    }
                    default: done(name + " · " + (message.isEmpty() ? phase : message));
                }
            }
        } catch (Exception ignored) {}
    }
    private static long lastBlockedToast;
    // A tap the player made on the map while the sniper runs was ignored: say so, at most every 3 s.
    private static void blockedToast(String what) {
        long now = System.currentTimeMillis();
        if (app == null || now - lastBlockedToast < 3000) return;
        lastBlockedToast = now;
        String thing = "pokestop".equals(what) ? "PokéStop" : "gym".equals(what) ? "Gym" : "Pokémon";
        main.post(() -> Toast.makeText(app, "Shundo Sniper is running: " + thing + " taps are blocked. Stop it from its card to play.",
            Toast.LENGTH_SHORT).show());
    }
    private static String ivs(JSONObject o) {
        JSONArray iv = o.optJSONArray("iv");
        return iv == null || iv.length() != 3 ? "" : iv.optInt(0) + "/" + iv.optInt(1) + "/" + iv.optInt(2);
    }
    // Under LOCK: the check is over; the next target waits a moment for the game to settle.
    private static void done(String line) {
        recent.addFirst(line); while (recent.size() > RECENT) recent.removeLast();
        target = null; holding = go = false; status = looking(); nextPick = System.currentTimeMillis() + SETTLE_MS;
    }

    private static void tick() {
        try {
            if (!enabled) return;
            long now = System.currentTimeMillis();
            if (!Control.canEnable() || !Control.locationKnown()) { synchronized (LOCK) { status = "Waiting for the game map"; } return; }
            synchronized (LOCK) {
                if (teleporting) return;
                if (target != null) {
                    if (holding) { hold(now); return; }
                    if (now - targetSince > RESULT_TIMEOUT_MS) done(target.name + " · no answer from the game");
                    else return;
                }
            }
            double lat = Control.currentLatitude(), lon = Control.currentLongitude();
            fetch(now, lat, lon);
            if (now >= nextPick) {
                // The user's minimum time between teleports, counted from the last one.
                long wait;
                synchronized (LOCK) {
                    wait = lastTeleport + gapSeconds * 1000L - now;
                    if (wait > 0) status = "Next teleport in " + (wait + 999) / 1000 + " s";
                }
                if (wait <= 0) pick(now, lat, lon);
            }
        } catch (Throwable error) {
            Control.event("shundo-sniper", "tick failed: " + error);
        }
    }

    // Under LOCK: a held shiny is caught once the cooldown estimate where it is (the player stays put)
    // reaches zero. An open encounter outlives the spawn timer, so it is held however long that takes.
    private static void hold(long now) {
        if (go) return;
        long wait = Cooldown.remaining();
        if (wait <= 0) { go = true; targetSince = now; status = "Shiny " + target.name + "! Catching…"; return; }
        long seconds = (wait + 999) / 1000;
        status = String.format(Locale.US, "Shiny %s! Catching in %d:%02d (cooldown)", target.name, seconds / 60, seconds % 60);
    }

    // Network on the worker thread, outside LOCK. Each feed keeps its own pace and backoff.
    private static void fetch(long now, double lat, double lon) {
        LiveFeedClient liveClient; PogoFeedClient pogoClient; boolean wantLive, wantPogo, wantFeed;
        synchronized (LOCK) { liveClient = live; pogoClient = pogo; wantLive = useLive; wantPogo = usePogo; wantFeed = useFeed; }
        if (wantFeed && now >= nextFeed) {
            nextFeed = now + FEED_MS;
            try { feedNote = "PGRazor feed: " + readFeed(now) + " seen"; }
            catch (Exception e) { feedNote = "PGRazor feed: unavailable"; }
        }
        if (wantLive && liveClient != null && now >= nextLive) {
            try {
                if (!liveClient.signedIn()) { liveNote = "LivePokeMap: sign in under Feeds"; nextLive = now + RETRY_MS; }
                else {
                    // The whole map: LivePokeMap then sends only 100% IV Pokémon.
                    LiveFeedData.Snapshot s = liveClient.fetch(lat, lon, LiveFeedClient.GLOBAL, 100);
                    int added = 0;
                    for (LiveFeedData.Entry e : s.entries)
                        if (e.kind == LiveFeedData.Kind.POKEMON && e.iv >= 100) added += add(e.id, e.dexId, speciesName(e.dexId, e.name), "LivePokeMap", e.lat, e.lon, e.expiresAt, null);
                    liveNote = "LivePokeMap: " + added + " seen";
                    nextLive = now + Math.max(LIVE_MIN_MS, s.refreshSeconds * 1000L);
                }
            } catch (LiveFeedClient.FeedException e) {
                liveNote = "LivePokeMap: " + (e.getMessage() == null ? "unavailable" : e.getMessage());
                nextLive = now + Math.max(30_000, e.retryAfterMs);
            } catch (Exception e) { liveNote = "LivePokeMap: unavailable"; nextLive = now + RETRY_MS; }
        }
        if (wantPogo && pogoClient != null && now >= nextPogo) {
            nextPogo = now + POGO_MS;
            // Nearest first within 1000 km (PoGoCoordinates hides coordinates until each is revealed).
            try {
                PogoFeedData.Snapshot s = pogoClient.fetch(lat, lon, 1000, 100);
                int added = 0;
                for (PogoFeedData.Entry e : s.entries)
                    if (e.kind == PogoFeedData.Kind.POKEMON && e.iv >= 100) added += add(e.id, e.dexId, speciesName(e.dexId, e.name), "PoGoCoordinates", e.lat, e.lon, e.expiresAt, e.revealCode);
                pogoNote = "PoGoCoordinates: " + added + " seen";
            } catch (PogoFeedClient.FeedException e) {
                pogoNote = "PoGoCoordinates: " + (e.getMessage() == null ? "unavailable" : e.getMessage());
                nextPogo = now + Math.max(POGO_MS, e.retryAfterMs);
            } catch (Exception e) { pogoNote = "PoGoCoordinates: unavailable"; nextPogo = now + RETRY_MS; }
        }
    }
    /** The species name from the bundled list (the feeds sometimes send only "#517"), else the feed's. */
    private static String speciesName(int dex, String fallback) {
        JSONObject names = speciesNames;
        if (names == null && app != null) {
            try (java.io.InputStream in = PgAssets.open(app, "pgrazor/map-icons/pokemon-names.json")) {
                java.io.ByteArrayOutputStream bytes = new java.io.ByteArrayOutputStream();
                byte[] buffer = new byte[4096]; int count;
                while ((count = in.read(buffer)) != -1 && bytes.size() <= 2 * 1024 * 1024) bytes.write(buffer, 0, count);
                speciesNames = names = new JSONObject(bytes.toString("UTF-8"));
            } catch (Exception ignored) { speciesNames = names = new JSONObject(); }
        }
        String name = names == null || dex <= 0 ? "" : names.optString(Integer.toString(dex), "");
        return name.isEmpty() ? fallback : name;
    }
    /** Reads the PGRazor feed and adds its rows; returns how many were taken. Worker thread only. */
    private static int readFeed(long now) throws Exception {
        java.net.HttpURLConnection http = (java.net.HttpURLConnection) new java.net.URL(FEED_URL).openConnection();
        http.setConnectTimeout(15_000); http.setReadTimeout(20_000);
        http.setRequestProperty("Accept", "application/json");
        http.setRequestProperty("User-Agent", "PGRazor-ShundoSniper");
        try {
            if (http.getResponseCode() != 200) throw new java.io.IOException("HTTP " + http.getResponseCode());
            java.io.ByteArrayOutputStream bytes = new java.io.ByteArrayOutputStream();
            try (java.io.InputStream in = http.getInputStream()) {
                byte[] buffer = new byte[16384]; int n;
                while ((n = in.read(buffer)) != -1) { if (bytes.size() + n > 8 * 1024 * 1024) throw new java.io.IOException("feed too large"); bytes.write(buffer, 0, n); }
            }
            JSONObject root = new JSONObject(bytes.toString("UTF-8"));
            if (root.optInt("v") != 1) throw new java.io.IOException("unknown feed version");
            JSONArray rows = root.optJSONArray("hundos");
            int added = 0;
            for (int i = 0; rows != null && i < rows.length(); i++) {
                JSONArray r = rows.optJSONArray(i);
                if (r == null || r.length() < 6) continue;
                long despawn = r.optLong(5, 0);
                if (despawn > 0 && despawn <= now) continue;
                int dex = r.optInt(1, 0);
                added += add("pokemon:" + r.optString(0, ""), dex, speciesName(dex, dex > 0 ? "#" + dex : "Pokémon"), "PGRazor feed",
                    r.optDouble(3, Double.NaN), r.optDouble(4, Double.NaN), despawn, null);
            }
            return added;
        } finally { http.disconnect(); }
    }
    // Under LOCK: forgets spawns whose despawn (plus a grace period) has passed.
    private static void pruneTried(long now) {
        attempted.values().removeIf(expires -> expires + TRIED_GRACE_MS < now);
    }
    // Under LOCK: "<encounter id> <despawn ms>" per line, written to a temp file and renamed into place.
    private static void saveTried() {
        if (app == null) return;
        java.io.File file = AccountProfiles.file(app, TRIED_FILE), temp = new java.io.File(file.getPath() + ".tmp");
        StringBuilder out = new StringBuilder();
        for (Map.Entry<String, Long> e : attempted.entrySet()) out.append(e.getKey()).append(' ').append(e.getValue()).append('\n');
        try (java.io.FileOutputStream stream = new java.io.FileOutputStream(temp)) {
            stream.write(out.toString().getBytes(java.nio.charset.StandardCharsets.UTF_8));
        } catch (java.io.IOException e) { return; }
        if (!temp.renameTo(file)) temp.delete();
    }
    // Under LOCK: reads the saved list, keeping only well-formed, unexpired entries.
    private static void loadTried(long now) {
        java.io.File file = AccountProfiles.file(app, TRIED_FILE);
        if (!file.isFile() || file.length() > 1_000_000) return;
        try {
            String text = new String(java.nio.file.Files.readAllBytes(file.toPath()), java.nio.charset.StandardCharsets.UTF_8);
            for (String line : text.split("\n")) {
                String[] part = line.trim().split(" ");
                if (part.length != 2 || encounterId("x:" + part[0]) == null) continue;
                try { attempted.put(part[0], Long.parseLong(part[1])); } catch (NumberFormatException ignored) { }
            }
        } catch (java.io.IOException ignored) { }
        pruneTried(now);
    }
    /** The encounter id behind a feed row id ("pokemon:123", "shiny:123"), or null when it is not one. */
    static String encounterId(String id) {
        if (id == null) return null;
        String digits = id.substring(id.lastIndexOf(':') + 1);
        if (digits.isEmpty() || digits.length() > 20) return null;
        try { return Long.toUnsignedString(Long.parseUnsignedLong(digits)); } catch (NumberFormatException e) { return null; }
    }
    private static int add(String id, int dex, String name, String source, double lat, double lon, long expiresAt, String revealCode) {
        String encounter = encounterId(id);
        if (encounter == null) return 0;
        synchronized (LOCK) {
            if (attempted.containsKey(encounter)) return 0;  // already checked; a feed refresh lists it again
            if (!wanted(dex)) return 0;
            Target known = candidates.get(encounter);
            if (known != null) { if (!known.located() && CooldownState.valid(lat, lon)) { known.lat = lat; known.lon = lon; } return 1; }
            if (candidates.size() >= MAX_CANDIDATES) return 0;
            candidates.put(encounter, new Target(id, encounter, dex, name == null || name.isEmpty() ? "Pokémon" : name, source, lat, lon, expiresAt, revealCode));
            return 1;
        }
    }

    // The nearest unchecked spawn with time left to reach and check it.
    // None located yet: reveal one PoGoCoordinates row (they come nearest first) when its quota allows.
    private static void pick(long now, double lat, double lon) {
        Target best = null, locked = null; double bestKm = Double.MAX_VALUE;
        synchronized (LOCK) {
            for (Iterator<Target> it = candidates.values().iterator(); it.hasNext(); ) {
                Target t = it.next();
                if (attempted.containsKey(t.encounter) || !wanted(t.dex) || (t.expiresAt > 0 && t.expiresAt - now < MIN_LEFT_MS)) { it.remove(); continue; }
                if (!t.located()) { if (locked == null && t.revealCode != null) locked = t; continue; }
                double km = CooldownState.distance(lat, lon, t.lat, t.lon);
                if (km < bestKm) { bestKm = km; best = t; }
            }
            if (best == null) {
                status = looking();
                if (locked == null) return;
            }
        }
        if (best == null) { reveal(locked); return; }
        Target chosen = best; double km = bestKm;
        synchronized (LOCK) {
            attempted.put(chosen.encounter, chosen.expiresAt > 0 ? chosen.expiresAt : now + TRIED_UNKNOWN_MS);
            pruneTried(now); saveTried();
            candidates.remove(chosen.encounter); teleporting = true;
        }
        main.post(() -> {
            if (!enabled) { synchronized (LOCK) { teleporting = false; } return; }
            Control.teleport(chosen.lat, chosen.lon);  // outside LOCK: Control may call back for config()
            synchronized (LOCK) {
                teleporting = false;
                if (!enabled) return;
                target = chosen; request++; targetSince = lastTeleport = System.currentTimeMillis(); teleports++; targetKm = km;
                status = String.format(Locale.US, "Checking %s (%s, %.1f km)", chosen.name, chosen.source, km);
            }
            Control.event("shundo-sniper", String.format(Locale.US, "target %s %s %.6f,%.6f from %s, %.1f km, %s left", chosen.encounter, chosen.name, chosen.lat, chosen.lon, chosen.source, km, chosen.expiresAt > 0 ? (chosen.expiresAt - System.currentTimeMillis()) / 1000 + " s" : "unknown"));
        });
    }
    private static void reveal(Target t) {
        PogoFeedClient client; synchronized (LOCK) { client = pogo; }
        if (client == null || client.revealWaitMs() > 0) return;
        try {
            double[] ll = client.revealOne(t.encounter, t.revealCode);
            synchronized (LOCK) {
                if (ll != null && ll.length == 2 && CooldownState.valid(ll[0], ll[1])) { t.lat = ll[0]; t.lon = ll[1]; }
                else candidates.remove(t.encounter);
            }
        } catch (Exception e) {
            synchronized (LOCK) { candidates.remove(t.encounter); pogoNote = "PoGoCoordinates: couldn't reveal a location"; }
        }
    }

    /** Start or stop from the on-map card; starting needs at least one feed ticked. */
    static void toggle(Activity activity) {
        boolean useAny; synchronized (LOCK) { useAny = useFeed || useLive || usePogo; }
        if (!enabled && !useAny) { show(activity); return; }
        setEnabled(activity, !enabled);
    }
    static boolean overlayShown() { return overlayShown; }
    /** The card's close button: stops the sniper if it is running and hides the card. */
    static void closeOverlay(Activity activity) { if (enabled) setEnabled(activity, false); overlayShown = false; }
    /** One line for the card: the spawn being checked (and for how long), a held shiny, or the idle state. */
    static String overlayStep() {
        synchronized (LOCK) {
            if (!enabled) return status.startsWith("Stopped") ? status : "Stopped";
            if (target != null && !holding) {
                long s = Math.max(0, (System.currentTimeMillis() - targetSince) / 1000);
                return String.format(Locale.US, "Checking %s · %.1f km · %d s", target.name, targetKm, s);
            }
            return status.isEmpty() ? looking() : status;
        }
    }
    static String overlayStats() {
        synchronized (LOCK) {
            return teleports + " teleports · " + checked + " checked · " + shinies + " shiny · " + caught + " caught"
                + (missed > 0 ? " · " + missed + " missed" : "");
        }
    }

    static String statusText() {
        synchronized (LOCK) {
            StringBuilder b = new StringBuilder();
            if (!status.isEmpty()) b.append(status);
            if (enabled && useLive && !liveNote.isEmpty()) b.append('\n').append(liveNote);
            if (enabled && useFeed && !feedNote.isEmpty()) b.append('\n').append(feedNote);
            if (enabled && usePogo && !pogoNote.isEmpty()) b.append('\n').append(pogoNote);
            b.append(b.length() > 0 ? "\n" : "").append("This session: checked ").append(checked).append(" · shiny ").append(shinies).append(" · caught ").append(caught);
            for (String line : recent) b.append("\n• ").append(line);
            return b.toString();
        }
    }

    public static void show(Activity activity) {
        if (activity == null || activity.isFinishing() || activity.isDestroyed()) return;
        Context ui = MapTheme.wrap(activity);
        LinearLayout body = new LinearLayout(ui); body.setOrientation(LinearLayout.VERTICAL);
        int pad = MapTheme.dp(ui, 16); body.setPadding(pad, pad, pad, pad);
        // Experimental, and it teleports and catches far more than a player could: say so before anything else.
        TextView warning = new TextView(ui);
        warning.setText("Experimental. Constant teleporting and remote catching raise the risk of an account warning or ban. Use an alt account, not your main.");
        warning.setTypeface(android.graphics.Typeface.DEFAULT_BOLD);
        warning.setPadding(0, 0, 0, pad / 2);
        body.addView(warning);
        CheckBox on = new CheckBox(ui); on.setText("Shundo Sniper on"); on.setChecked(enabled); body.addView(on);
        TextView help = new TextView(ui);
        help.setText("Teleports to 100% IV Pokémon and checks each for shiny. A shiny is held in its encounter until your cooldown ends, then caught. VGP+ pauses while this runs.");
        body.addView(help);
        CheckBox feedBox = new CheckBox(ui); feedBox.setText("PGRazor feed");
        CheckBox liveBox = new CheckBox(ui); liveBox.setText("LivePokeMap (sign-in)");
        CheckBox pogoBox = new CheckBox(ui); pogoBox.setText("PoGoCoordinates (slow)");
        synchronized (LOCK) { feedBox.setChecked(useFeed); liveBox.setChecked(useLive); pogoBox.setChecked(usePogo); }
        TextView sources = new TextView(ui); sources.setText("Sources"); sources.setPadding(0, pad / 2, 0, 0);
        body.addView(sources); body.addView(feedBox); body.addView(liveBox); body.addView(pogoBox);
        android.widget.Button speciesButton = new android.widget.Button(ui); speciesButton.setAllCaps(false);
        speciesButton.setText(speciesLabel()); body.addView(speciesButton);
        speciesButton.setOnClickListener(v -> SmartCatchSpeciesDialog.show(activity, "Shundo Sniper species", "species", MAX_SPECIES,
            ShundoSniper::speciesList, ShundoSniper::setSpecies, () -> speciesButton.setText(speciesLabel())));
        android.widget.Button gapButton = new android.widget.Button(ui); gapButton.setAllCaps(false);
        gapButton.setText(gapLabel()); body.addView(gapButton);
        gapButton.setOnClickListener(v -> {
            AlertDialog choose = ChoiceList.attach(new AlertDialog.Builder(ui).setTitle("Time between teleports")
                .setNegativeButton("Cancel", null).create(), GAP_LABELS, i -> {
                    synchronized (LOCK) { gapSeconds = GAPS[i]; }
                    Control.saveSettings(); gapButton.setText(gapLabel());
                    Control.event("shundo-sniper", "teleport gap " + GAPS[i] + " s");
                });
            DialogLifecycle.bind(activity, choose, null);
            choose.setOnShowListener(d -> MapTheme.styleButtons(choose));
            choose.show();
        });
        TextView state = new TextView(ui); state.setText(statusText()); state.setPadding(0, pad / 2, 0, 0); body.addView(state);
        on.setOnCheckedChangeListener((v, checked) -> {
            if (checked && !useFeed && !useLive && !usePogo) { on.setChecked(false); return; }
            setEnabled(activity, checked); state.setText(statusText());
        });
        liveBox.setOnCheckedChangeListener((v, checked) -> { synchronized (LOCK) { useLive = checked; nextLive = 0; } Control.saveSettings(); });
        feedBox.setOnCheckedChangeListener((v, checked) -> { synchronized (LOCK) { useFeed = checked; nextFeed = 0; } Control.saveSettings(); });
        pogoBox.setOnCheckedChangeListener((v, checked) -> { synchronized (LOCK) { usePogo = checked; nextPogo = 0; } Control.saveSettings(); });

        ScrollView scroll = new ScrollView(ui); scroll.addView(body);
        AlertDialog dialog = new AlertDialog.Builder(ui).setTitle("Shundo Sniper (Experimental)").setView(scroll).setPositiveButton("Done", null).create();
        Runnable refresh = new Runnable() {
            public void run() { if (dialog.isShowing()) { if (on.isChecked() != enabled) on.setChecked(enabled); state.setText(statusText()); main.postDelayed(this, 1000); } }
        };
        DialogLifecycle.bind(activity, dialog, () -> main.removeCallbacks(refresh));
        dialog.setOnShowListener(d -> {
            MapTheme.style(body); MapTheme.styleButtons(dialog);
            warning.setTextColor(MapTheme.dark() ? android.graphics.Color.rgb(255, 196, 87) : android.graphics.Color.rgb(150, 90, 0));
            dialog.getWindow().setBackgroundDrawable(new ColorDrawable(MapTheme.surface()));
            main.post(refresh);
        });
        dialog.show();
    }
}
