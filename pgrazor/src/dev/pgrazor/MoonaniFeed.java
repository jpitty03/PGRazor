package dev.pgrazor;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

/**
 * moonani.com's public 100% IV list (the table on moonani.com/PokeList). Read from the phone: the site's
 * Cloudflare blocks server addresses. It gives exact coordinates but no encounter id, so every row is a
 * spot the sniper matches by species and place (FeedSpots). One request covers the whole list.
 */
final class MoonaniFeed {
    private MoonaniFeed() {}
    static final String URL = "https://moonani.com/PokeList/ajax.php?page=pokemon&action=load";
    static final String BODY = "draw=1&start=0&length=500&pokemons=&iv=100&pvp=0";
    static final long POLL_MS = 60_000, BLOCKED_MS = 15 * 60_000;

    static final class Spot {
        final String key; final int dex; final double lat, lon; final long expiresAt;
        Spot(String key, int dex, double lat, double lon, long expiresAt) { this.key = key; this.dex = dex; this.lat = lat; this.lon = lon; this.expiresAt = expiresAt; }
    }
    /** The site refused (403) or rate limited (429) the request: wait retryMs before asking again. */
    static final class Blocked extends IOException {
        final long retryMs;
        Blocked(String message, long retryMs) { super(message); this.retryMs = retryMs; }
    }

    static List<Spot> fetch(long now) throws IOException {
        HttpURLConnection http = (HttpURLConnection) new URL(URL).openConnection();
        http.setConnectTimeout(15_000); http.setReadTimeout(20_000);
        http.setRequestMethod("POST"); http.setDoOutput(true);
        http.setRequestProperty("Content-Type", "application/x-www-form-urlencoded; charset=UTF-8");
        http.setRequestProperty("Accept", "application/json");
        http.setRequestProperty("X-Requested-With", "XMLHttpRequest");
        http.setRequestProperty("Referer", "https://moonani.com/PokeList/index.php");
        http.setRequestProperty("User-Agent", "PGRazor-ShundoSniper");
        try {
            try (OutputStream out = http.getOutputStream()) { out.write(BODY.getBytes(StandardCharsets.UTF_8)); }
            int code = http.getResponseCode();
            if (code == 403) throw new Blocked("blocked by the site", BLOCKED_MS);
            if (code == 429) {
                long retry = POLL_MS;
                try { retry = Math.max(POLL_MS, Long.parseLong(http.getHeaderField("Retry-After").trim()) * 1000); } catch (Exception ignored) { }
                throw new Blocked("rate limited", retry);
            }
            if (code != 200) throw new IOException("HTTP " + code);
            ByteArrayOutputStream bytes = new ByteArrayOutputStream();
            try (InputStream in = http.getInputStream()) {
                byte[] buffer = new byte[16384]; int n;
                while ((n = in.read(buffer)) != -1) { if (bytes.size() + n > 4 * 1024 * 1024) throw new IOException("list too large"); bytes.write(buffer, 0, n); }
            }
            try { return parse(new JSONObject(bytes.toString("UTF-8")), now); }
            catch (org.json.JSONException e) { throw new IOException("unexpected answer"); }
        } finally { http.disconnect(); }
    }

    /** The 15/15/15 rows with at least a minute left. Its times are the site's local clock (FeedSpots.offsetMs). */
    static List<Spot> parse(JSONObject root, long now) throws org.json.JSONException {
        JSONArray data = root.getJSONArray("data");
        List<JSONObject> rows = new ArrayList<>();
        long newestStart = Long.MIN_VALUE;
        for (int i = 0; i < data.length() && i < 2000; i++) {
            JSONObject o = data.optJSONObject(i);
            if (o == null) continue;
            long start = FeedSpots.naiveUtc(o.optString("Start Time"));
            if (start != Long.MIN_VALUE) newestStart = Math.max(newestStart, start);
            rows.add(o);
        }
        List<Spot> out = new ArrayList<>();
        Long offset = FeedSpots.offsetMs(newestStart, now);
        if (offset == null) return out;
        for (JSONObject o : rows) {
            if (!"15".equals(o.optString("Attack")) || !"15".equals(o.optString("Defense")) || !"15".equals(o.optString("HP"))) continue;
            int dex;
            try { dex = Integer.parseInt(o.optString("Number").trim()); } catch (NumberFormatException e) { continue; }
            double[] at = FeedSpots.coords(o.optString("Coords"));
            long end = FeedSpots.naiveUtc(o.optString("End Time"));
            if (dex <= 0 || dex > 2000 || at == null || end == Long.MIN_VALUE) continue;
            long expiresAt = end - offset;
            if (expiresAt < now + 60_000 || expiresAt > now + 2 * 3_600_000) continue;
            out.add(new Spot(FeedSpots.key(dex, at[0], at[1], expiresAt), dex, at[0], at[1], expiresAt));
        }
        return out;
    }
}
