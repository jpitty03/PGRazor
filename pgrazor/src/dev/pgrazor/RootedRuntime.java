package dev.pgrazor;

import android.app.Application;
import android.app.Instrumentation;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.Signature;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.widget.Toast;
import java.io.*;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.util.function.Consumer;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import org.json.JSONObject;

/**
 * Stage 1 of PGRazor Root, running inside PGRazor's own class loader.
 *
 * <p>Extracts the payload into the game's private storage, loads the host-ABI ART hook library and
 * hooks {@link Instrumentation#callApplicationOnCreate}. That runs right after the game's content
 * providers are installed and before Application.onCreate: the same point where the patched APK's
 * BootstrapProvider starts. There it checks the game version, installs the bridge hooks and runs
 * the shared startup ({@link BootstrapProvider#start}). Any failure leaves the game unmodified.
 */
public final class RootedRuntime {
    static final String TAG = "PGRazor-root";
    private static Consumer<ClassLoader> gameLoader;

    private RootedRuntime() {}

    public static void start(byte[] payload, String dataDir, Consumer<ClassLoader> loader) throws Exception {
        gameLoader = loader;
        File directory = new File(new File(dataDir, "code_cache"), "pgrazor-root");
        if (!directory.isDirectory() && !directory.mkdirs()) throw new IOException("Cannot create " + directory);
        File archive = new File(directory, "payload.zip");
        writeIfChanged(archive, payload);
        PgAssets.usePayload(archive, directory);
        String abi = hostAbi();
        File art = extract(archive, "pgrazor/" + abi + "/libpgrazor-art.so", new File(directory, "libpgrazor-art.so"));
        System.load(art.getAbsolutePath());
        NativeJavaHooks.loaded = true;
        if (!NativeJavaHooks.ready()) throw new IllegalStateException("ART hooks unavailable on " + abi);
        AppCreate hook = new AppCreate();
        Method target = Instrumentation.class.getDeclaredMethod("callApplicationOnCreate", Application.class);
        synchronized (hook) {
            hook.backup = NativeJavaHooks.hook(target, hook, AppCreate.class.getMethod("invoke", Object[].class));
            if (hook.backup == null) throw new IllegalStateException("Application hook refused");
            hook.backup.setAccessible(true);
        }
        Log.i(TAG, "runtime ready abi=" + abi + " pid=" + android.os.Process.myPid());
    }

    /** The process's own ABI from /proc/self/exe; os.arch follows the kernel, which MuMu reports as aarch64. */
    static String hostAbi() throws IOException {
        String abi = RuntimeAbi.nativeProcessAbi();
        if (!abi.equals("arm64-v8a") && !abi.equals("x86_64")) throw new IOException("Unsupported process ABI " + abi);
        return abi;
    }

    /** Runs before the game's Application.onCreate on the main thread. Never throws into the game. */
    private static void startPgrazor(Application app) {
        try {
            gameLoader.accept(app.getClassLoader());
            JSONObject target = new JSONObject(readText(app, "pgrazor/rooted/target.json"));
            PackageInfo installedPackage = app.getPackageManager().getPackageInfo(app.getPackageName(), PackageManager.GET_SIGNING_CERTIFICATES);
            if (!app.getPackageName().equals(target.getString("package")) || !trustedCertificate(installedPackage, target.getString("certificateSha256"))) {
                Log.e(TAG, "PGRazor Root requires the official game signature; PGRazor stays off");
                return;
            }
            long supported = target.getLong("versionCode");
            long installed = installedPackage.getLongVersionCode();
            if (installed != supported) {
                String message = "PGRazor Root supports Pokémon GO " + target.optString("versionName") + ". This game version isn't supported yet, so PGRazor is off.";
                Log.w(TAG, message + " installed=" + installed);
                new Handler(Looper.getMainLooper()).postDelayed(() -> Toast.makeText(app, message, Toast.LENGTH_LONG).show(), 4000);
                return;
            }
            if (!RootedHooks.install(app, app.getClassLoader())) {
                Log.e(TAG, "bridge hooks incomplete; PGRazor stays off");
                return;
            }
            if (!BootstrapProvider.start(app)) Log.e(TAG, "PGRazor startup declined");
            else Log.i(TAG, "PGRazor started for " + target.optString("versionName"));
        } catch (Throwable failure) {
            Log.e(TAG, "PGRazor startup failed; game continues unmodified", failure);
        }
    }

    private static boolean trustedCertificate(PackageInfo info, String expected) throws Exception {
        if (info.signingInfo == null) return false;
        Signature[] signers = info.signingInfo.getApkContentsSigners();
        if (signers == null || signers.length != 1) return false;
        byte[] hash = java.security.MessageDigest.getInstance("SHA-256").digest(signers[0].toByteArray());
        StringBuilder hex = new StringBuilder();
        for (byte value : hash) hex.append(Character.forDigit((value >>> 4) & 15, 16)).append(Character.forDigit(value & 15, 16));
        return expected.equalsIgnoreCase(hex.toString());
    }

    private static String readText(Application app, String name) throws IOException {
        try (InputStream in = PgAssets.open(app, name); ByteArrayOutputStream out = new ByteArrayOutputStream()) {
            byte[] buffer = new byte[8192];
            int count;
            while ((count = in.read(buffer)) != -1) out.write(buffer, 0, count);
            return new String(out.toByteArray(), StandardCharsets.UTF_8);
        }
    }

    private static void writeIfChanged(File file, byte[] data) throws IOException {
        if (file.isFile() && file.length() == data.length && java.util.Arrays.equals(read(file), data)) return;
        File temp = new File(file.getPath() + ".tmp");
        try (FileOutputStream out = new FileOutputStream(temp)) { out.write(data); out.getFD().sync(); }
        if (!temp.renameTo(file)) throw new IOException("Cannot replace " + file);
    }

    private static byte[] read(File file) throws IOException {
        byte[] data = new byte[(int) file.length()];
        try (DataInputStream in = new DataInputStream(new FileInputStream(file))) { in.readFully(data); }
        return data;
    }

    private static File extract(File archive, String name, File destination) throws IOException {
        try (ZipFile zip = new ZipFile(archive)) {
            ZipEntry entry = zip.getEntry(name);
            if (entry == null) throw new FileNotFoundException(name);
            File temp = new File(destination.getPath() + ".tmp");
            try (InputStream in = zip.getInputStream(entry); FileOutputStream out = new FileOutputStream(temp)) {
                byte[] buffer = new byte[65536];
                int count;
                while ((count = in.read(buffer)) != -1) out.write(buffer, 0, count);
            }
            if (destination.exists() && !destination.setWritable(true)) throw new IOException("Cannot replace " + destination);
            if (!temp.renameTo(destination) || !destination.setReadOnly()) throw new IOException("Cannot install " + destination);
        }
        return destination;
    }

    /** Hook on Instrumentation.callApplicationOnCreate: start PGRazor once, then run the original. */
    public static final class AppCreate {
        private Method backup;
        private boolean ran;

        public Object invoke(Object[] args) throws Throwable {
            Method original;
            boolean first;
            synchronized (this) { original = backup; first = !ran; ran = true; }
            if (original == null) throw new IllegalStateException("Application backup not published");
            if (first && args.length == 2 && args[1] instanceof Application) startPgrazor((Application) args[1]);
            try { return original.invoke(args[0], args[1]); }
            catch (InvocationTargetException error) { throw error.getCause(); }
        }
    }
}
