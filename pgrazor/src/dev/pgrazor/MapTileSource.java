package dev.pgrazor;

import android.graphics.Bitmap;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.widget.TextView;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

/** Fetches OpenStreetMap tiles for the current viewport and hands decoded bitmaps back to the map.
 *  Owns HTTP retrieval, the two-tier cache, bounded worker scheduling and viewport/generation gating.
 *  Depends only on a {@link MapTileCache} and the owning view; never on Control or map projection. */
final class MapTileSource {
    /** A placed tile: identity (zoom/x/y/key) for loading plus a screen offset for drawing. */
    static final class Tile {
        final String key;
        final int zoom, x, y;
        final float left, top;
        Tile(int zoom, int x, int y, float left, float top) {
            this.zoom = zoom; this.x = x; this.y = y; this.left = left; this.top = top;
            key = zoom + "-" + x + "-" + y;
        }
    }

    /** Callbacks into the owning view: the current zoom (for status text) and a redraw request. */
    interface Host {
        int zoom();
        void onTilesChanged();
    }

    private final Host host;
    private final TextView status;
    private final MapTileCache cache;
    private final Handler main = new Handler(Looper.getMainLooper());
    // A single worker and a single replaceable viewport task bound queued work. The worker and one helper
    // download that task's tiles two at a time, the most the OpenStreetMap tile policy allows a client.
    private final ThreadPoolExecutor worker = new ThreadPoolExecutor(1, 1, 0, TimeUnit.MILLISECONDS, new LinkedBlockingQueue<Runnable>(1));
    private final ExecutorService helper = Executors.newSingleThreadExecutor();
    private final Map<String, Long> failures = new HashMap<>();
    private final Runnable request = this::loadVisible;

    private List<Tile> visible = Collections.emptyList();
    private volatile Set<String> visibleKeys = Collections.emptySet();
    private volatile boolean closed;
    private volatile int generation;
    private Future<?> pending;

    MapTileSource(Host host, TextView status, File cacheDirectory) {
        this.host = host; this.status = status; this.cache = new MapTileCache(cacheDirectory);
    }

    /** Latest viewport wins: record the placed tiles, bump the generation and debounce a load. */
    void setViewport(List<Tile> tiles) {
        if (closed) return;
        visible = tiles;
        Set<String> keys = new HashSet<>(); for (Tile tile : tiles) keys.add(tile.key); visibleKeys = keys;
        generation++;
        // Keep useful in-flight tiles; never disconnect a socket on the UI thread.
        if (!main.hasCallbacks(request)) main.postDelayed(request, 60);
    }

    /** Placed tiles for the latest viewport, iterated by the view when drawing. */
    List<Tile> tiles() { return visible; }
    /** Already decoded bitmap for a tile (or an ancestor key), or null when not resident. */
    Bitmap bitmap(String key) { return cache.memory(key); }
    /** Forget transient per-tile failures so a retry re-requests them immediately. */
    void clearFailures() { failures.clear(); }

    private void loadVisible() {
        // Keep ownership until the main-thread completion callback clears pending. Future.isDone()
        // can become true first; admitting another task then lets the old callback erase its owner.
        if (closed || pending != null) return;
        final int revision = generation;
        final List<Tile> wanted = new ArrayList<>();
        long now = SystemClock.elapsedRealtime();
        int blocked = 0;
        for (Tile tile : visible) {
            Long failedAt = failures.get(tile.key);
            if (cache.memory(tile.key) != null) continue;
            if (failedAt != null && now - failedAt < 60000) { blocked++; continue; }
            wanted.add(tile);
        }
        status.setText(wanted.isEmpty() ? (blocked > 0 ? "Map unavailable in some areas. Check internet, then Retry." : "Zoom " + host.zoom() + " · Tap map to select") : "Loading map…");
        if (wanted.isEmpty()) return;
        // Middle of the screen first: tiles nearest the centre of the placed ones load before the edges.
        float cx = 0, cy = 0; for (Tile tile : visible) { cx += tile.left; cy += tile.top; }
        final float midX = cx / visible.size(), midY = cy / visible.size();
        Collections.sort(wanted, (a, b) -> Float.compare(distance(a, midX, midY), distance(b, midX, midY)));
        final int blockedTiles = blocked;
        worker.purge();
        pending = worker.submit(() -> {
            final AtomicInteger errors = new AtomicInteger(), next = new AtomicInteger();
            try {
                cache.ensureDirectory();
                Runnable drain = () -> {
                    for (int i; (i = next.getAndIncrement()) < wanted.size(); ) {
                        Tile tile = wanted.get(i);
                        if (!visibleKeys.contains(tile.key)) continue;
                        if (closed || Thread.currentThread().isInterrupted()) return;
                        try {
                            Bitmap bitmap = readTile(tile);
                            main.post(() -> {
                                if (closed) { bitmap.recycle(); return; }
                                cache.remember(tile.key, bitmap); failures.remove(tile.key); host.onTilesChanged();
                            });
                        } catch (IOException | RuntimeException e) {
                            if (closed || Thread.currentThread().isInterrupted()) return;
                            errors.incrementAndGet();
                            main.post(() -> {
                                if (!closed) {
                                    if (failures.size() >= 128) failures.clear();
                                    failures.put(tile.key, SystemClock.elapsedRealtime());
                                }
                            });
                        }
                    }
                };
                Future<?> second = helper.submit(drain);
                drain.run();
                try { second.get(); }
                catch (InterruptedException e) { second.cancel(true); Thread.currentThread().interrupt(); return; }
                catch (ExecutionException e) { errors.incrementAndGet(); }
                if (closed || Thread.currentThread().isInterrupted()) return;
                cache.trim();
                final int finalErrors = errors.get() + blockedTiles;
                main.post(() -> { if (!closed && revision == generation) status.setText(finalErrors == 0 ? "Zoom " + host.zoom() + " · Tap map to select" : "Map unavailable in some areas. Check internet, then Retry."); });
            } catch (IOException | RuntimeException e) {
                main.post(() -> { if (!closed && revision == generation) status.setText("Map cache unavailable. Close map and retry."); });
            } finally { main.post(() -> { if (!closed) { pending = null; if (revision != generation) loadVisible(); } }); }
        });
    }

    private static float distance(Tile tile, float x, float y) { float dx = tile.left - x, dy = tile.top - y; return dx * dx + dy * dy; }

    private Bitmap readTile(Tile tile) throws IOException {
        Bitmap cached = cache.readDisk(tile.key);
        if (cached != null) return cached;
        if (closed) throw new IOException("Map closed");
        HttpURLConnection request = (HttpURLConnection) new URL("https://tile.openstreetmap.org/" + tile.zoom + "/" + tile.x + "/" + tile.y + ".png").openConnection();
        // A tile read to the end returns its connection to the pool for the next tile (each new one costs a
        // TLS handshake, about 3x the tile itself); only a failed request is disconnected.
        boolean reusable = false;
        try {
            request.setConnectTimeout(8000); request.setReadTimeout(8000);
            request.setInstanceFollowRedirects(false);
            request.setRequestProperty("User-Agent", "PGRazor/0.1 (+https://github.com/jpitty03/PGRazor)");
            request.setRequestProperty("Accept", "image/png");
            if (request.getResponseCode() != 200) throw new IOException("Tile HTTP " + request.getResponseCode());
            byte[] bytes;
            try (InputStream stream = request.getInputStream()) { bytes = MapTileCache.readBounded(stream); }
            Bitmap bitmap = MapTileCache.decode(bytes);
            if (closed) { bitmap.recycle(); throw new IOException("Map closed"); }
            cache.writeDisk(tile.key, bytes);
            reusable = true;
            return bitmap;
        } finally { if (!reusable) request.disconnect(); }
    }

    void close() {
        if (closed) return;
        closed = true; generation++;
        main.removeCallbacksAndMessages(null);
        if (pending != null) pending.cancel(true);
        worker.shutdownNow(); helper.shutdownNow();
        failures.clear();
    }
}
