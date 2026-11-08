package dev.pgrazor;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.util.LruCache;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.Arrays;
import java.util.Comparator;
import java.util.concurrent.atomic.AtomicInteger;

/** Two-tier tile store: a process-wide in-memory bitmap LRU over a bounded on-disk PNG cache.
 *  Pure storage, decoding and eviction with byte/dimension bounds; no networking, view or Control ties. */
final class MapTileCache {
    // Shared across map instances so re-opening the picker reuses already decoded tiles.
    private static final LruCache<String, Bitmap> MEMORY = new LruCache<String, Bitmap>(16 * 1024 * 1024) {
        protected int sizeOf(String key, Bitmap b) { return b.getAllocationByteCount(); }
    };
    // This cache deliberately uses the policy's >=7-day fallback by file age instead of parsing HTTP cache headers.
    private static final long CACHE_TTL = 7L * 24 * 60 * 60 * 1000;
    private static final long CACHE_LIMIT = 64L * 1024 * 1024;
    private static final int TILE_BYTE_LIMIT = 512 * 1024;

    private final File directory;
    private final AtomicInteger writes = new AtomicInteger();

    MapTileCache(File directory) { this.directory = directory; }

    Bitmap memory(String key) { return MEMORY.get(key); }
    void remember(String key, Bitmap bitmap) { MEMORY.put(key, bitmap); }

    void ensureDirectory() throws IOException {
        if (!directory.isDirectory() && !directory.mkdirs()) throw new IOException("Cannot create map cache");
    }

    /** Fresh on-disk tile decoded to a bitmap, or null when missing/stale; a corrupt file is dropped. */
    Bitmap readDisk(String key) throws IOException {
        File file = new File(directory, key + ".png");
        if (file.isFile() && System.currentTimeMillis() - file.lastModified() < CACHE_TTL) {
            try (InputStream stream = new FileInputStream(file)) { return decode(readBounded(stream)); }
            catch (IOException e) { file.delete(); }
        }
        return null;
    }

    /** Persist raw tile bytes through a temporary file, then bound disk use. A disk failure never discards a usable tile. */
    void writeDisk(String key, byte[] bytes) {
        File file = new File(directory, key + ".png");
        File temporary = new File(directory, key + ".part");
        try {
            try (FileOutputStream out = new FileOutputStream(temporary)) { out.write(bytes); }
            if (file.exists()) file.delete();
            if (!temporary.renameTo(file)) temporary.delete();
            // Bound disk use even if the user keeps changing viewport before a batch finishes; listing and
            // sorting the whole directory after every tile slowed loading, so only every 32nd write.
            if (writes.incrementAndGet() % 32 == 0) trim();
        } catch (IOException e) { temporary.delete(); }
    }

    /** Evict the oldest files until within the byte and count ceilings. */
    synchronized void trim() {
        File[] files = directory.listFiles(); if (files == null) return;
        Arrays.sort(files, Comparator.comparingLong(File::lastModified));
        long total = 0; for (File file : files) total += file.length();
        int remaining = files.length;
        for (File file : files) {
            if (total <= CACHE_LIMIT && remaining <= 1024) break;
            long length = file.length(); if (file.delete()) { total -= length; remaining--; }
        }
    }

    static byte[] readBounded(InputStream stream) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buffer = new byte[8192]; int count;
        while ((count = stream.read(buffer)) != -1) {
            if (Thread.currentThread().isInterrupted()) throw new IOException("Map closed");
            if (out.size() + count > TILE_BYTE_LIMIT) throw new IOException("Oversize map tile");
            out.write(buffer, 0, count);
        }
        return out.toByteArray();
    }

    static Bitmap decode(byte[] bytes) throws IOException {
        BitmapFactory.Options bounds = new BitmapFactory.Options(); bounds.inJustDecodeBounds = true;
        BitmapFactory.decodeByteArray(bytes, 0, bytes.length, bounds);
        if (bounds.outWidth != 256 || bounds.outHeight != 256) throw new IOException("Invalid tile dimensions");
        Bitmap bitmap = BitmapFactory.decodeByteArray(bytes, 0, bytes.length);
        if (bitmap == null) throw new IOException("Invalid map tile");
        return bitmap;
    }
}
