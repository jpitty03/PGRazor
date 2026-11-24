package dev.pgrazor;

import android.app.Activity;
import android.app.DownloadManager;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageInfo;
import android.content.pm.PackageInstaller;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.provider.Settings;
import android.widget.Toast;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.security.MessageDigest;
import java.util.Locale;
import org.json.JSONArray;
import org.json.JSONObject;

/**
 * In-app updates from the project's GitHub releases. A newer release shows one row at the top of the
 * settings home; tapping it downloads this build's release file and checks it against the release's
 * SHA256SUMS.txt before anything is installed.
 *
 * Each build takes its own file. The patched app downloads PGRazor.apk and hands it to the system
 * installer itself; Android asks the player to confirm unless it allows this app to update itself
 * silently (Android 12+, once an earlier update was installed this way). PGRazor Root runs inside the
 * official game, which Android does not let install packages, so it has the system download manager
 * fetch PGRazor-Root.apk, checks it the same way, and leaves the install to the download notification;
 * the player then opens the installer and taps Install as for a first setup. A patched build made
 * before the install permission was packaged updates the same way with PGRazor.apk.
 */
final class Updater {
    private Updater() {}

    private static final String LATEST = "https://api.github.com/repos/jpitty03/PGRazor/releases/latest";
    private static final String RELEASES = "https://github.com/jpitty03/PGRazor/releases/latest";
    private static final String STATUS_ACTION = "dev.pgrazor.UPDATE_STATUS";
    private static final String INSTALL_PERMISSION = "android.permission.REQUEST_INSTALL_PACKAGES";

    static final int NONE = 0, AVAILABLE = 1, DOWNLOADING = 2, INSTALLING = 3, FAILED = 4, DOWNLOADED = 5;

    private static final Object LOCK = new Object();
    private static final Handler main = new Handler(Looper.getMainLooper());
    private static volatile int state = NONE;
    private static volatile String tag = "", pageUrl = "", apkUrl = "", sumsUrl = "", note = "";
    // The release file this build updates from.
    private static volatile String asset = UpdateRules.assetName(false);
    private static volatile long size, received;
    // Whether this build can install the update itself; set by the first check.
    private static volatile boolean installsHere;
    private static long nextCheck;
    private static boolean busy, receiverRegistered;
    private static volatile Runnable listener;

    /** Called on the main thread whenever the row's text should change. */
    static void listen(Runnable onChange) { listener = onChange; }

    static boolean visible() { return state != NONE; }

    static String title() {
        switch (state) {
            case DOWNLOADING: return "Downloading " + tag;
            case INSTALLING: return "Installing " + tag;
            case FAILED: return "Update " + tag + " did not install";
            case DOWNLOADED: return "Update " + tag + " downloaded";
            default: return "Update available: " + tag;
        }
    }

    static String detail() {
        switch (state) {
            case DOWNLOADING:
                return size > 0 ? Math.min(100, received * 100 / size) + "% of " + UpdateRules.megabytes(size) : "Starting";
            case INSTALLING: return note.isEmpty() ? "Waiting for Android to install it" : note;
            case FAILED: return note + " · tap to try again";
            case DOWNLOADED:
                // Downloads lists the file under its download title, "PGRazor <tag>".
                return PgAssets.rooted() ? "Tap to open PGRazor " + tag + " in Downloads: update the installer, open it, tap Install Module, then reboot"
                    : "Tap to open PGRazor " + tag + " in Downloads and install it";
            default:
                String megabytes = UpdateRules.megabytes(size);
                String what = PgAssets.rooted() ? "Tap to download the Root installer" : installsHere ? "Tap to download and install" : "Tap to download";
                return megabytes.isEmpty() ? what : what + " (" + megabytes + ")";
        }
    }

    private static void changed() {
        Runnable target = listener;
        if (target != null) main.post(target);
    }

    private static void fail(String reason) {
        note = reason; state = FAILED;
        Control.event("update", "failed: " + reason);
        changed();
    }

    /** Looks for a newer release in the background, at most every few hours. Safe to call often. */
    static void check(Context context) {
        final Context app = context.getApplicationContext();
        long now = System.currentTimeMillis();
        synchronized (LOCK) {
            if (busy || now < nextCheck || state == DOWNLOADING || state == INSTALLING || state == DOWNLOADED) return;
            busy = true; nextCheck = now + UpdateRules.RETRY_MS;
        }
        new Thread(() -> {
            try {
                installsHere = !PgAssets.rooted() && declaresInstallPermission(app);
                JSONObject release = new JSONObject(new String(read(LATEST, 512 * 1024), "UTF-8"));
                String latest = release.optString("tag_name", "");
                synchronized (LOCK) { nextCheck = System.currentTimeMillis() + UpdateRules.CHECK_MS; }
                if (release.optBoolean("draft") || release.optBoolean("prerelease") || !UpdateRules.newer(latest, BuildVersion.NAME)) return;
                String wanted = UpdateRules.assetName(PgAssets.rooted()), apk = "", sums = ""; long bytes = 0;
                JSONArray assets = release.optJSONArray("assets");
                for (int i = 0; assets != null && i < assets.length(); i++) {
                    JSONObject entry = assets.optJSONObject(i);
                    if (entry == null) continue;
                    String name = entry.optString("name", ""), url = entry.optString("browser_download_url", "");
                    if (!UpdateRules.trustedUrl(url)) continue;
                    if (name.equals(wanted)) { apk = url; bytes = entry.optLong("size", 0); }
                    else if (name.equals("SHA256SUMS.txt")) sums = url;
                }
                String page = release.optString("html_url", "");
                if (!page.startsWith("https://github.com/jpitty03/PGRazor/")) page = RELEASES;
                // A release still uploading its files is offered on a later check.
                if (apk.isEmpty()) return;
                tag = latest; pageUrl = page; apkUrl = apk; sumsUrl = sums; size = bytes; note = ""; asset = wanted;
                if (state == NONE || state == AVAILABLE || state == FAILED) state = AVAILABLE;
                Control.event("update", "available " + latest + " installed=" + BuildVersion.NAME + " file=" + wanted + " inApp=" + installsHere);
                changed();
            } catch (Exception e) {
                Control.event("update", "check failed: " + e);
            } finally {
                synchronized (LOCK) { busy = false; }
            }
        }, "pgrazor-update-check").start();
    }

    private static boolean declaresInstallPermission(Context app) {
        try {
            PackageInfo info = app.getPackageManager().getPackageInfo(app.getPackageName(), PackageManager.GET_PERMISSIONS);
            if (info.requestedPermissions != null)
                for (String permission : info.requestedPermissions) if (INSTALL_PERMISSION.equals(permission)) return true;
        } catch (Exception ignored) {}
        return false;
    }

    /** The row was tapped. */
    static void tap(Activity activity) {
        int now = state;
        if (now == NONE || now == DOWNLOADING || now == INSTALLING) return;
        if (now == DOWNLOADED) {
            try { activity.startActivity(new Intent(DownloadManager.ACTION_VIEW_DOWNLOADS).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)); }
            catch (Exception unavailable) { Toast.makeText(activity, "Open your Downloads to install " + asset, Toast.LENGTH_LONG).show(); }
            return;
        }
        // Android only lets an app install packages after the player allows it once in system settings.
        if (installsHere && !activity.getPackageManager().canRequestPackageInstalls()) {
            try {
                activity.startActivity(new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:" + activity.getPackageName())));
                Toast.makeText(activity, "Allow installs from this app, then tap the update again", Toast.LENGTH_LONG).show();
            } catch (Exception unavailable) { openPage(activity); }
            return;
        }
        final Context app = activity.getApplicationContext();
        synchronized (LOCK) {
            if (busy) return;
            busy = true;
        }
        received = 0; note = ""; state = DOWNLOADING; changed();
        new Thread(() -> {
            try { if (installsHere) downloadAndInstall(app); else downloadWithSystem(app); }
            catch (Exception e) { fail(e.getMessage() == null || e.getMessage().isEmpty() ? "Download failed" : e.getMessage()); }
            finally { synchronized (LOCK) { busy = false; } }
        }, "pgrazor-update").start();
    }

    private static void openPage(Activity activity) {
        try { activity.startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(pageUrl.isEmpty() ? RELEASES : pageUrl))); }
        catch (Exception unavailable) { Toast.makeText(activity, "No app available to open the download page", Toast.LENGTH_SHORT).show(); }
    }

    private static String expectedChecksum(String sums) throws Exception {
        String wanted = sums.isEmpty() ? null : UpdateRules.checksum(new String(read(sums, 64 * 1024), "UTF-8"), asset);
        if (wanted == null) throw new IOException("This release has no checksum to verify against");
        return wanted;
    }

    private static String hex(MessageDigest digest) {
        StringBuilder text = new StringBuilder();
        for (byte b : digest.digest()) text.append(String.format(Locale.US, "%02x", b));
        return text.toString();
    }

    /** The patched app: download into the app's cache, verify, then install through the system installer. */
    private static void downloadAndInstall(Context app) throws Exception {
        String url = apkUrl, sums = sumsUrl, version = tag; long expected = size;
        if (!UpdateRules.trustedUrl(url)) throw new IOException("Unexpected download address");
        if (!UpdateRules.sizeAllowed(expected)) throw new IOException("Unexpected download size");
        // The checksum list is small; without it the download is not trusted.
        String wanted = expectedChecksum(sums);
        File folder = new File(app.getCacheDir(), "pgrazor-update");
        File[] old = folder.listFiles();
        if (old != null) for (File file : old) file.delete();
        if (!folder.isDirectory() && !folder.mkdirs()) throw new IOException("No room to save the update");
        if (folder.getUsableSpace() < expected + UpdateRules.SPARE_BYTES)
            throw new IOException("Not enough free space (" + UpdateRules.megabytes(expected + UpdateRules.SPARE_BYTES) + " needed)");
        File file = new File(folder, "update.apk");
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        HttpURLConnection http = open(url);
        try {
            if (http.getResponseCode() != 200) throw new IOException("Download failed (HTTP " + http.getResponseCode() + ")");
            long total = 0, shown = 0;
            try (InputStream in = http.getInputStream(); OutputStream out = new FileOutputStream(file)) {
                byte[] buffer = new byte[64 * 1024]; int count;
                while ((count = in.read(buffer)) != -1) {
                    total += count;
                    if (total > expected) throw new IOException("Download is larger than the release says");
                    out.write(buffer, 0, count); digest.update(buffer, 0, count);
                    received = total;
                    if (total - shown >= expected / 100 + 1) { shown = total; changed(); }
                }
            }
            if (total != expected) throw new IOException("Download was cut short");
        } finally { http.disconnect(); }
        if (!hex(digest).equals(wanted)) { file.delete(); throw new IOException("Download did not match the release checksum"); }
        Control.event("update", "downloaded " + version + " bytes=" + expected + " checksum ok");
        state = INSTALLING; note = ""; changed();
        install(app, file);
    }

    /**
     * For builds that cannot install packages: the system download manager saves the file to Downloads,
     * the bytes are checked against the release checksum, and its notification opens the installer.
     */
    private static void downloadWithSystem(Context app) throws Exception {
        String url = apkUrl, sums = sumsUrl, version = tag, name = asset; long expected = size;
        if (!UpdateRules.trustedUrl(url)) throw new IOException("Unexpected download address");
        if (!UpdateRules.sizeAllowed(expected)) throw new IOException("Unexpected download size");
        String wanted = expectedChecksum(sums);
        DownloadManager manager = (DownloadManager) app.getSystemService(Context.DOWNLOAD_SERVICE);
        if (manager == null) throw new IOException("Downloads are not available on this device");
        DownloadManager.Request request = new DownloadManager.Request(Uri.parse(url))
            .setTitle("PGRazor " + version).setDescription(name)
            .setMimeType("application/vnd.android.package-archive")
            .setNotificationVisibility(DownloadManager.Request.VISIBILITY_VISIBLE_NOTIFY_COMPLETED)
            .setDestinationInExternalPublicDir(Environment.DIRECTORY_DOWNLOADS, name.replace(".apk", "-" + version + ".apk"));
        long id = manager.enqueue(request);
        boolean kept = false;
        try {
            long deadline = System.currentTimeMillis() + 30 * 60_000L;
            while (true) {
                int status = -1, reason = 0; long soFar = 0;
                try (Cursor row = manager.query(new DownloadManager.Query().setFilterById(id))) {
                    if (row != null && row.moveToFirst()) {
                        status = row.getInt(row.getColumnIndexOrThrow(DownloadManager.COLUMN_STATUS));
                        reason = row.getInt(row.getColumnIndexOrThrow(DownloadManager.COLUMN_REASON));
                        soFar = row.getLong(row.getColumnIndexOrThrow(DownloadManager.COLUMN_BYTES_DOWNLOADED_SO_FAR));
                    }
                }
                if (status == -1) throw new IOException("Download was cancelled");
                if (status == DownloadManager.STATUS_FAILED) throw new IOException("Download failed (" + reason + ")");
                if (status == DownloadManager.STATUS_SUCCESSFUL) break;
                if (soFar != received) { received = soFar; changed(); }
                if (System.currentTimeMillis() > deadline) throw new IOException("Download took too long");
                Thread.sleep(1000);
            }
            MessageDigest digest = MessageDigest.getInstance("SHA-256");
            long total = 0;
            try (ParcelFileDescriptor saved = manager.openDownloadedFile(id); InputStream in = new FileInputStream(saved.getFileDescriptor())) {
                byte[] buffer = new byte[64 * 1024]; int count;
                while ((count = in.read(buffer)) != -1) { total += count; digest.update(buffer, 0, count); }
            }
            if (total != expected || !hex(digest).equals(wanted)) throw new IOException("Download did not match the release checksum");
            kept = true;
            Control.event("update", "downloaded " + version + " " + name + " bytes=" + expected + " checksum ok (system download)");
            state = DOWNLOADED; note = ""; changed();
        } finally {
            // A failed or mismatching download is removed, with its notification, so it cannot be installed.
            if (!kept) manager.remove(id);
        }
    }

    private static void install(Context app, File file) throws Exception {
        PackageInstaller installer = app.getPackageManager().getPackageInstaller();
        PackageInstaller.SessionParams params = new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
        params.setAppPackageName(app.getPackageName());
        params.setSize(file.length());
        // Android 12+: no confirmation when this app is updating itself and installed its current version.
        if (Build.VERSION.SDK_INT >= 31) params.setRequireUserAction(PackageInstaller.SessionParams.USER_ACTION_NOT_REQUIRED);
        registerReceiver(app);
        int id = installer.createSession(params);
        PackageInstaller.Session session = installer.openSession(id);
        try {
            try (InputStream in = new FileInputStream(file); OutputStream out = session.openWrite("pgrazor", 0, file.length())) {
                byte[] buffer = new byte[64 * 1024]; int count;
                while ((count = in.read(buffer)) != -1) out.write(buffer, 0, count);
                session.fsync(out);
            }
            int flags = PendingIntent.FLAG_UPDATE_CURRENT | (Build.VERSION.SDK_INT >= 31 ? PendingIntent.FLAG_MUTABLE : 0);
            PendingIntent result = PendingIntent.getBroadcast(app, id, new Intent(STATUS_ACTION).setPackage(app.getPackageName()), flags);
            session.commit(result.getIntentSender());
        } catch (Exception e) {
            session.abandon();
            throw e;
        } finally {
            session.close();
        }
    }

    private static void registerReceiver(Context app) {
        synchronized (LOCK) {
            if (receiverRegistered) return;
            receiverRegistered = true;
        }
        BroadcastReceiver receiver = new BroadcastReceiver() {
            @Override public void onReceive(Context context, Intent intent) {
                int status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE);
                if (status == PackageInstaller.STATUS_PENDING_USER_ACTION) {
                    Intent confirm = intent.getParcelableExtra(Intent.EXTRA_INTENT);
                    note = "Confirm the update in the Android prompt"; changed();
                    try { if (confirm != null) context.startActivity(confirm.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)); }
                    catch (Exception e) { fail("Android could not show the install prompt"); }
                    return;
                }
                // A successful update replaces this process, so success is rarely seen here.
                if (status == PackageInstaller.STATUS_SUCCESS) { state = NONE; changed(); return; }
                String reason = intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE);
                fail(status == PackageInstaller.STATUS_FAILURE_ABORTED ? "Install cancelled"
                    : status == PackageInstaller.STATUS_FAILURE_STORAGE ? "Not enough free space to install"
                    : status == PackageInstaller.STATUS_FAILURE_CONFLICT || status == PackageInstaller.STATUS_FAILURE_INCOMPATIBLE
                        ? "Android refused the update (" + (reason == null ? "incompatible" : reason) + ")"
                    : "Android could not install it" + (reason == null || reason.isEmpty() ? "" : " (" + reason + ")"));
            }
        };
        IntentFilter filter = new IntentFilter(STATUS_ACTION);
        // Only this app sends the status; Android 13+ wants that said when registering.
        if (Build.VERSION.SDK_INT >= 33) app.registerReceiver(receiver, filter, Context.RECEIVER_NOT_EXPORTED);
        else app.registerReceiver(receiver, filter);
    }

    private static HttpURLConnection open(String url) throws IOException {
        HttpURLConnection http = (HttpURLConnection) new URL(url).openConnection();
        http.setConnectTimeout(15_000); http.setReadTimeout(30_000);
        http.setInstanceFollowRedirects(true);
        http.setRequestProperty("User-Agent", "PGRazor-Updater");
        http.setRequestProperty("Accept", url.startsWith("https://api.github.com/") ? "application/vnd.github+json" : "*/*");
        return http;
    }

    private static byte[] read(String url, int limit) throws IOException {
        HttpURLConnection http = open(url);
        try {
            if (http.getResponseCode() != 200) throw new IOException("HTTP " + http.getResponseCode());
            ByteArrayOutputStream bytes = new ByteArrayOutputStream();
            try (InputStream in = http.getInputStream()) {
                byte[] buffer = new byte[16 * 1024]; int count;
                while ((count = in.read(buffer)) != -1) {
                    if (bytes.size() + count > limit) throw new IOException("Response too large");
                    bytes.write(buffer, 0, count);
                }
            }
            return bytes.toByteArray();
        } finally { http.disconnect(); }
    }
}
