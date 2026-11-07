package dev.pgrazor;

import android.content.Context;
import android.graphics.Typeface;
import java.io.*;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/**
 * PGRazor's bundled files. The patched APK reads them from its own assets; PGRazor Root reads the
 * same names (for example {@code pgrazor/mode.json}) from the module payload, because the game's
 * APK is untouched and has no PGRazor assets.
 */
public final class PgAssets {
    private static volatile ZipFile payload;
    private static volatile File cache;

    private PgAssets() {}

    /** Rooted build only: serve every asset from this payload archive from now on. */
    static synchronized void usePayload(File archive, File cacheDirectory) throws IOException {
        if (payload != null) return;
        cache = cacheDirectory;
        payload = new ZipFile(archive);
    }

    /** True when running as PGRazor Root (module payload), false in the patched APK. */
    public static boolean rooted() { return payload != null; }

    public static InputStream open(Context context, String name) throws IOException {
        ZipFile archive = payload;
        if (archive == null) return context.getAssets().open(name);
        ZipEntry entry = archive.getEntry(name);
        if (entry == null || entry.isDirectory()) throw new FileNotFoundException(name);
        return archive.getInputStream(entry);
    }

    /** Typeface from a bundled font; null when the font is missing or unreadable. */
    public static Typeface typeface(Context context, String name) {
        try {
            if (payload == null) return Typeface.createFromAsset(context.getAssets(), name);
            File file = new File(cache, "font-" + name.replace('/', '_'));
            if (!file.isFile()) {
                File temp = new File(file.getPath() + ".tmp");
                try (InputStream in = open(context, name); FileOutputStream out = new FileOutputStream(temp)) {
                    byte[] buffer = new byte[65536];
                    int count;
                    while ((count = in.read(buffer)) != -1) out.write(buffer, 0, count);
                }
                if (!temp.renameTo(file)) return null;
            }
            return Typeface.createFromFile(file);
        } catch (Exception | Error missing) {
            return null;
        }
    }
}
