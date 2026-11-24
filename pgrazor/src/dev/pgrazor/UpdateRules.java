package dev.pgrazor;

import java.util.Locale;

/** The updater's decisions, kept free of Android so they can be tested on their own. */
final class UpdateRules {
    private UpdateRules() {}

    /** Looks for a new release at most this often; the unauthenticated GitHub API allows 60 requests an hour per address. */
    static final long CHECK_MS = 6 * 60 * 60_000L, RETRY_MS = 30 * 60_000L;
    /** The largest update accepted, and the free space kept beyond the download itself. */
    static final long MAX_BYTES = 600L * 1024 * 1024, SPARE_BYTES = 64L * 1024 * 1024;

    /**
     * The numbers of a release tag such as "v0.3.6" or "0.3.6-beta.1" (the suffix is ignored), or null
     * when it is not three dot-separated numbers.
     */
    static int[] parse(String tag) {
        if (tag == null) return null;
        String text = tag.trim();
        if (text.startsWith("v") || text.startsWith("V")) text = text.substring(1);
        int dash = text.indexOf('-');
        if (dash >= 0) text = text.substring(0, dash);
        String[] parts = text.split("\\.", -1);
        if (parts.length != 3) return null;
        int[] numbers = new int[3];
        for (int i = 0; i < 3; i++) {
            if (parts[i].isEmpty() || parts[i].length() > 6) return null;
            for (int c = 0; c < parts[i].length(); c++) if (!Character.isDigit(parts[i].charAt(c))) return null;
            numbers[i] = Integer.parseInt(parts[i]);
        }
        return numbers;
    }

    /** True when {@code latest} is a later release than {@code installed}; false when either is unreadable. */
    static boolean newer(String latest, String installed) {
        int[] a = parse(latest), b = parse(installed);
        if (a == null || b == null) return false;
        for (int i = 0; i < 3; i++) if (a[i] != b[i]) return a[i] > b[i];
        // The same numbers: a full release replaces a pre-release of it, nothing else does.
        return installed.indexOf('-') >= 0 && latest.indexOf('-') < 0;
    }

    /** The release file this build updates from: the full APK, or for PGRazor Root its installer. */
    static String assetName(boolean rooted) { return rooted ? "PGRazor-Root.apk" : "PGRazor.apk"; }

    /**
     * The SHA-256 of {@code file} from a checksum list ("<64 hex>  <name>" per line, a leading '*' on the
     * name allowed), lower case, or null when the list does not name it exactly once.
     */
    static String checksum(String list, String file) {
        if (list == null || file == null) return null;
        String found = null;
        for (String line : list.split("\n")) {
            String row = line.trim();
            int gap = row.indexOf(' ');
            if (gap != 64) continue;
            String hash = row.substring(0, 64).toLowerCase(Locale.US), name = row.substring(64).trim();
            if (name.startsWith("*")) name = name.substring(1);
            if (!name.equals(file)) continue;
            for (int i = 0; i < 64; i++) if (Character.digit(hash.charAt(i), 16) < 0) return null;
            if (found != null) return null;
            found = hash;
        }
        return found;
    }

    /** Only release files served by GitHub over HTTPS are downloaded. */
    static boolean trustedUrl(String url) {
        return url != null && url.startsWith("https://github.com/jpitty03/PGRazor/releases/download/");
    }

    static boolean sizeAllowed(long bytes) { return bytes > 0 && bytes <= MAX_BYTES; }

    /** "186 MB" for the menu row. */
    static String megabytes(long bytes) {
        return bytes <= 0 ? "" : Math.max(1, Math.round(bytes / (1024.0 * 1024.0))) + " MB";
    }
}
