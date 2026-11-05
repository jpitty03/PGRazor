package dev.pgrazor;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.SocketTimeoutException;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import org.json.JSONException;
import org.json.JSONObject;

/**
 * Minimal HTTPS client for LivePokeMap built on {@link HttpURLConnection} (no third-party deps).
 *
 * <p>It talks to two endpoints only: {@code POST /api/auth/login} and {@code POST /api/map/viewport}.
 * It never activates a remote game session, never sends an {@code X-Credential-ID}, and never logs or
 * echoes credentials or raw server bodies. The viewport request uses a bounded 1..5 km box around the
 * given centre, or the whole map for {@link #GLOBAL} (the service then only sends 100% IV Pokémon).
 * A 401 clears the stored token, a 403 is reported as a
 * lack of access and leaves the session intact. All methods are synchronous and must be called off the
 * main thread; a single instance is owned by the UI and {@link #cancel()} aborts the in-flight call.
 */
public final class LiveFeedClient {
    private static final String BASE = "https://play.livepokemap.com";
    private static final int CONNECT_TIMEOUT_MS = 10000;
    private static final int READ_TIMEOUT_MS = 15000;
    private static final int MAX_RESPONSE_BYTES = 8 * 1024 * 1024;
    private static final int MAP_ZOOM = 15;
    private static final String USER_AGENT = "PGRazor/0.1 (+https://github.com/jpitty03/PGRazor)";
    private static final double KM_PER_DEGREE_LAT = 111.0;
    private static final double MAX_LON_DELTA_DEGREES = 45.0; // guards against a huge box near the poles
    /** Radius value meaning "the whole map": the viewport becomes a world box instead of a 1..5 km box. */
    public static final double GLOBAL = Double.POSITIVE_INFINITY;
    private static final long DEFAULT_RETRY_AFTER_MS = 10000;

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

    public LiveFeedClient(LiveFeedSession session) { this.session = session; }

    public boolean signedIn() { return session.signedIn(); }

    public void logout() { cancel(); synchronized(requestLock) { session.clear(); } }

    /** Aborts the connection currently in flight, if any; the blocked call fails with a safe message. */
    public void cancel() {
        HttpURLConnection current;
        synchronized(requestLock) { epoch++; current = connection; }
        if (current != null) { try { current.disconnect(); } catch (Exception ignored) { } }
    }

    /** Signs in and stores the returned bearer token encrypted. Call off the UI thread. */
    public void login(String username, String password) throws Exception {
        if (username == null || username.trim().isEmpty() || password == null || password.isEmpty())
            throw new FeedException("Enter your username and password.");
        long requestEpoch; synchronized(requestLock) { requestEpoch = epoch; }
        JSONObject body = new JSONObject();
        body.put("username", username.trim());
        body.put("password", password);
        JSONObject response = request("/api/auth/login", body, null, requestEpoch);
        // The token sits inside the success/data envelope, but tolerate a top-level token too.
        String token = null;
        JSONObject data = response.optJSONObject("data");
        if (data != null) token = nonEmpty(data.optString("token", ""));
        if (token == null) token = nonEmpty(response.optString("token", ""));
        if (token == null) throw new FeedException("Sign-in did not return a session. Please try again.");
        synchronized(requestLock) { checkActive(requestEpoch); session.save(token); }
    }

    /**
     * Legacy overload: fetches map objects within {@code radiusKm} (clamped 1..5) of the given centre,
     * applying an IV floor both server-side and client-side. Retained for existing callers; new code
     * should use {@link #fetch(double, double, double, LiveFeedFilters)}, which trusts the server's
     * filtering so it cannot strip the "always show" exceptions. Throws {@link FeedException}
     * (status 401) and clears the stored token when the session is rejected. Call off the UI thread.
     */
    public LiveFeedData.Snapshot fetch(double lat, double lon, double radiusKm, int minIv) throws Exception {
        long requestEpoch; synchronized(requestLock) { requestEpoch = epoch; }
        String token = requireToken();
        requireLocation(lat, lon);
        int iv = (int) clamp(minIv, 0, 100);
        JSONObject body = viewportBody(lat, lon, radiusKm, displayFilters(iv));
        JSONObject response = send("/api/map/viewport", body, token, requestEpoch);
        LiveFeedData.Snapshot parsed = LiveFeedData.parse(response, System.currentTimeMillis());
        return filterByIv(parsed, iv);
    }

    /**
     * Fetches map objects within {@code radiusKm} (clamped 1..5) of the given centre using the full
     * display-filter set. Unlike the legacy overload this re-applies no client-side IV floor: the
     * server already honours the filters, and re-filtering here would discard the perfect/zero/rare
     * "always show" exceptions the server deliberately included. Throws {@link FeedException}
     * (status 401) and clears the stored token when the session is rejected. Call off the UI thread.
     */
    public LiveFeedData.Snapshot fetch(double lat, double lon, double radiusKm, LiveFeedFilters filters) throws Exception {
        long requestEpoch; synchronized(requestLock) { requestEpoch = epoch; }
        String token = requireToken();
        requireLocation(lat, lon);
        JSONObject body = viewportBody(lat, lon, radiusKm, (filters == null ? new LiveFeedFilters() : filters).toRequestJson());
        JSONObject response = send("/api/map/viewport", body, token, requestEpoch);
        return LiveFeedData.parse(response, System.currentTimeMillis());
    }

    private String requireToken() throws FeedException {
        String token = session.token();
        if (token == null) throw new FeedException("Please sign in to load the live map.", 401, 0);
        return token;
    }

    private static void requireLocation(double lat, double lon) throws FeedException {
        if (!Double.isFinite(lat) || !Double.isFinite(lon) || lat < -90 || lat > 90 || lon < -180 || lon > 180)
            throw new FeedException("That location is outside the valid range.");
    }

    // Issues the POST and, on an expired-session 401 for this still-current request, clears the token so
    // the UI falls back to the login form. A 403 (no access) deliberately leaves the session intact.
    private JSONObject send(String path, JSONObject body, String token, long requestEpoch) throws Exception {
        try {
            return request(path, body, token, requestEpoch);
        } catch (FeedException fe) {
            if (fe.status == 401) synchronized(requestLock) { if (epoch == requestEpoch) session.clear(); }
            throw fe;
        }
    }

    // A bounded box around the centre (radius clamped 1..5 km), or the whole map for GLOBAL, plus the
    // given display filters.
    private static JSONObject viewportBody(double lat, double lon, double radiusKm, JSONObject displayFilters) throws JSONException {
        JSONObject body = new JSONObject();
        if (radiusKm == GLOBAL) {
            body.put("min_lat", -85.0);
            body.put("max_lat", 85.0);
            body.put("min_lon", -180.0);
            body.put("max_lon", 180.0);
        } else {
            double radius = clamp(radiusKm, 1, 5);
            double dLat = radius / KM_PER_DEGREE_LAT;
            double cosLat = Math.max(Math.cos(Math.toRadians(lat)), 1e-6); // avoid divide-by-zero at the poles
            double dLon = Math.min(radius / (KM_PER_DEGREE_LAT * cosLat), MAX_LON_DELTA_DEGREES);
            body.put("min_lat", clamp(lat - dLat, -90, 90));
            body.put("max_lat", clamp(lat + dLat, -90, 90));
            body.put("min_lon", clamp(lon - dLon, -180, 180));
            body.put("max_lon", clamp(lon + dLon, -180, 180));
        }
        body.put("zoom", MAP_ZOOM);
        body.put("display_filters", displayFilters);
        return body;
    }

    // show_all keeps the server from hiding anything; the explicit floors plus disabled "always show"
    // overrides mean the server respects the IV floor, and we re-apply it client-side as a backstop.
    private static JSONObject displayFilters(int minIv) throws JSONException {
        JSONObject filters = new JSONObject();
        filters.put("show_all", true);
        filters.put("min_iv", minIv);
        filters.put("max_iv", 100);
        filters.put("min_level", 0);
        filters.put("max_level", 50);
        filters.put("always_show_perfect", false);
        filters.put("always_show_zero", false);
        filters.put("always_show_rare", false);
        filters.put("always_show_size", false);
        return filters;
    }

    // Keep every non-pokemon object; for pokemon honour the IV floor but, when the floor is 0, keep
    // ordinary wild spawns whose IV is unknown.
    private static LiveFeedData.Snapshot filterByIv(LiveFeedData.Snapshot snapshot, int minIv) {
        if (minIv <= 0) return snapshot;
        List<LiveFeedData.Entry> kept = new ArrayList<>(snapshot.entries.size());
        for (LiveFeedData.Entry e : snapshot.entries) {
            if (e.kind != LiveFeedData.Kind.POKEMON || (e.hasStats && e.iv >= minIv)) kept.add(e);
        }
        return new LiveFeedData.Snapshot(kept, snapshot.fetchedAt, snapshot.refreshSeconds, snapshot.note);
    }

    private JSONObject request(String path, JSONObject body, String bearer, long requestEpoch) throws Exception {
        URL url = new URL(BASE + path);
        if (!"https".equalsIgnoreCase(url.getProtocol())) throw new FeedException("A secure connection is required.");
        HttpURLConnection http = (HttpURLConnection) url.openConnection();
        synchronized(requestLock) { checkActive(requestEpoch); connection = http; }
        try {
            http.setConnectTimeout(CONNECT_TIMEOUT_MS);
            http.setReadTimeout(READ_TIMEOUT_MS);
            http.setInstanceFollowRedirects(false); // refuse redirects so the bearer token cannot leak
            http.setRequestMethod("POST");
            http.setDoOutput(true);
            http.setRequestProperty("Content-Type", "application/json; charset=utf-8");
            http.setRequestProperty("Accept", "application/json");
            http.setRequestProperty("User-Agent", USER_AGENT);
            if (bearer != null) http.setRequestProperty("Authorization", "Bearer " + bearer);

            byte[] payload = body.toString().getBytes(StandardCharsets.UTF_8);
            try (OutputStream out = http.getOutputStream()) { out.write(payload); }

            int code = http.getResponseCode();
            if (code >= 300 && code < 400)
                throw new FeedException("The service redirected the request; refusing to continue.", code, 0);
            if (code == 401)
                throw new FeedException("Your session has expired. Please sign in again.", 401, 0);
            if (code == 403)
                throw new FeedException("Your account doesn't have access to this feature.", 403, 0);
            if (code == 429)
                throw new FeedException("Too many requests. Please wait before refreshing.", 429, retryAfterMs(http));
            if (code < 200 || code >= 300)
                throw new FeedException("The live map service is unavailable (HTTP " + code + ").", code, 0);

            byte[] bytes;
            try (InputStream in = http.getInputStream()) { bytes = readBounded(in); }
            JSONObject json;
            try {
                json = new JSONObject(new String(bytes, StandardCharsets.UTF_8));
            } catch (JSONException e) {
                throw new FeedException("The live map service returned an unexpected response.", code, 0);
            }
            // A structured failure envelope: report safely without echoing the server's message.
            if (json.has("success") && !json.optBoolean("success", true))
                throw new FeedException("The live map service could not complete the request.", code, 0);
            synchronized(requestLock) { checkActive(requestEpoch); }
            return json;
        } catch (SocketTimeoutException e) {
            throw new FeedException("The live map service timed out. Please retry.");
        } catch (IOException e) {
            synchronized(requestLock) { checkActive(requestEpoch); }
            throw new FeedException("Could not reach the live map service.");
        } finally {
            try { http.disconnect(); } catch (Exception ignored) { }
            if (connection == http) connection = null;
        }
    }

    private void checkActive(long requestEpoch) throws FeedException {
        if(epoch != requestEpoch || Thread.currentThread().isInterrupted()) throw new FeedException("Request cancelled.");
    }

    private static long retryAfterMs(HttpURLConnection http) {
        String header = http.getHeaderField("Retry-After");
        if (header != null) {
            try {
                long seconds = Long.parseLong(header.trim());
                if (seconds >= 0) return Math.min(seconds, 3600) * 1000L; // cap at one hour
            } catch (NumberFormatException ignored) {
                // HTTP-date form is not parsed here; fall through to the default.
            }
        }
        return DEFAULT_RETRY_AFTER_MS;
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

    private static String nonEmpty(String value) {
        if (value == null) return null;
        String trimmed = value.trim();
        return trimmed.isEmpty() ? null : trimmed;
    }
}
