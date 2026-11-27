package dev.pgrazor;

import android.content.Context;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.HashMap;
import java.util.Iterator;
import java.util.Locale;
import java.util.Map;
import org.json.JSONArray;
import org.json.JSONObject;

/**
 * Current Team GO Rocket line-ups for Smart battling, from ScrapedDuck's copy of LeekDuck's list
 * (github.com/bigfoott/ScrapedDuck, data branch). Downloaded at most once a day and cached; never
 * bundled. Sent to native as lines "key|slot1|slot2|slot3", each slot "type1,type2,dex;...", where key
 * is the character's sorted name words (see keyOf), matching the game's InvasionCharacter names.
 */
final class RocketLineups {
    private RocketLineups() {}
    static final String URL_JSON = "https://raw.githubusercontent.com/bigfoott/ScrapedDuck/data/rocketLineups.json";
    private static final long DAY_MS = 24L * 60 * 60 * 1000;
    private static final int LIMIT = 512 * 1024;
    private static final String[] TYPES = {"normal","fighting","flying","poison","ground","rock","bug","ghost","steel","fire","water","grass","electric","psychic","ice","dragon","dark","fairy"};
    private static volatile String sent;
    private static volatile boolean fetching;

    /** From the overlay refresh: pushes the cached list, and refreshes it in the background when a day old. */
    static void push(Context context) {
        if (sent == null) send(context);
        File cache = cache(context);
        if (fetching || (cache.exists() && System.currentTimeMillis() - cache.lastModified() < DAY_MS)) return;
        fetching = true;
        new Thread(() -> {
            try { download(cache); sent = null; }
            catch (Exception e) { if (cache.exists()) cache.setLastModified(System.currentTimeMillis() - DAY_MS + 60L * 60 * 1000); }  // retry in an hour
            finally { fetching = false; }
        }, "pgrazor-rocket-lineups").start();
    }
    private static File cache(Context context) { return new File(context.getFilesDir(), "rocket-lineups.json"); }

    private static void send(Context context) {
        File cache = cache(context);
        String text = "";
        try { if (cache.exists()) text = convert(read(cache), dexByName(context)); } catch (Exception ignored) {}
        try { NativeCore.rocketLineups(text); sent = text; } catch (UnsatisfiedLinkError ignored) { sent = ""; }
    }
    private static String read(File file) throws java.io.IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        try (InputStream in = new java.io.FileInputStream(file)) { copy(in, out); }
        return out.toString("UTF-8");
    }
    private static void copy(InputStream in, ByteArrayOutputStream out) throws java.io.IOException {
        byte[] buffer = new byte[8192]; int n;
        while ((n = in.read(buffer)) > 0) { if (out.size() + n > LIMIT) throw new java.io.IOException("too large"); out.write(buffer, 0, n); }
    }

    private static void download(File cache) throws Exception {
        HttpURLConnection http = (HttpURLConnection) new URL(URL_JSON).openConnection();
        try {
            http.setConnectTimeout(10000); http.setReadTimeout(15000);
            http.setRequestProperty("User-Agent", "PGRazor (+https://github.com/jpitty03/PGRazor)");
            if (http.getResponseCode() != 200) throw new java.io.IOException("HTTP " + http.getResponseCode());
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            try (InputStream in = http.getInputStream()) { copy(in, out); }
            String json = out.toString("UTF-8");
            new JSONArray(json);  // validate before replacing the cache
            File tmp = new File(cache.getPath() + ".tmp");
            try (FileOutputStream f = new FileOutputStream(tmp)) { f.write(json.getBytes(StandardCharsets.UTF_8)); }
            if (!tmp.renameTo(cache)) { cache.delete(); if (!tmp.renameTo(cache)) throw new java.io.IOException("cache"); }
        } finally { http.disconnect(); }
    }

    private static Map<String, Integer> dexByName(Context context) {
        Map<String, Integer> out = new HashMap<>();
        JSONObject names = PokemonSpeciesDialog.readNames(context);
        if (names == null) return out;
        for (Iterator<String> it = names.keys(); it.hasNext(); ) {
            String id = it.next();
            try { out.put(names.getString(id).toLowerCase(Locale.US), Integer.parseInt(id)); } catch (Exception ignored) {}
        }
        return out;
    }

    /** "Fire-type Female Grunt" -> "female-fire-grunt"; steel is "metal" in the game's names. */
    static String keyOf(String name) {
        String[] words = name.toLowerCase(Locale.US).replace("steel", "metal").split("[^a-z]+");
        String[] kept = Arrays.stream(words).filter(w -> !w.isEmpty() && !w.equals("type")).sorted().toArray(String[]::new);
        return String.join("-", kept);
    }

    static String convert(String json, Map<String, Integer> dex) throws Exception {
        JSONArray all = new JSONArray(json);
        StringBuilder out = new StringBuilder();
        for (int i = 0; i < all.length(); i++) {
            JSONObject c = all.getJSONObject(i);
            StringBuilder line = new StringBuilder(keyOf(c.optString("name")));
            for (String slot : new String[]{"firstPokemon", "secondPokemon", "thirdPokemon"}) {
                line.append('|');
                JSONArray list = c.optJSONArray(slot);
                boolean first = true;
                for (int j = 0; list != null && j < list.length(); j++) {
                    JSONObject p = list.getJSONObject(j);
                    JSONArray types = p.optJSONArray("types");
                    int t1 = typeId(types, 0), t2 = typeId(types, 1);
                    if (t1 == 0) continue;
                    // Regional forms keep their species' base stats here; their types come from the list.
                    String species = p.optString("name").toLowerCase(Locale.US).replaceFirst("^(alolan|galarian|hisuian|paldean) ", "");
                    Integer id = dex.get(species);
                    if (!first) line.append(';');
                    first = false;
                    line.append(t1).append(',').append(t2).append(',').append(id == null ? 0 : id);
                }
            }
            out.append(line).append('\n');
        }
        return out.toString();
    }
    private static int typeId(JSONArray types, int index) {
        if (types == null || index >= types.length()) return 0;
        String t = types.optString(index).toLowerCase(Locale.US);
        for (int i = 0; i < TYPES.length; i++) if (TYPES[i].equals(t)) return i + 1;
        return 0;
    }
}
