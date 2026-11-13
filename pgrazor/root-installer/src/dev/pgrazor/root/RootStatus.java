package dev.pgrazor.root;

import android.content.Context;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.Signature;
import android.content.pm.SigningInfo;
import java.io.File;
import java.security.MessageDigest;
import java.util.HashMap;
import java.util.Map;

/**
 * A read-only snapshot of the device's root solution, Zygisk provider, installed PGRazor module and
 * the official Pokémon GO install. Everything reported here is observed, never assumed: unknown
 * states are labelled "Unknown" rather than guessed. Detection runs one root shell probe and a
 * PackageManager query; {@link #detect} must be called off the main thread because of the probe.
 *
 * <p>Writes are limited to explicit user actions: {@link #install}, {@link #setInjection},
 * and {@link #reboot}. It makes no changes during detection.
 */
final class RootStatus {
    enum Manager { NONE, MAGISK, KERNELSU, APATCH }

    /** One read-only probe. Nested double quotes inside $(...) open a fresh shell quoting context. */
    private static final String PROBE = String.join("\n",
        "S=/data/adb/modules",
        "echo \"ROOT=$(id -u 2>/dev/null)\"",
        "echo \"MAGISK=$(magisk -V 2>/dev/null)\"",
        "echo \"KSUD=$(command -v ksud 2>/dev/null)\"",
        "[ -x /system/xbin/dusk ] && echo DUSK=/system/xbin/dusk",
        "[ -x /data/adb/ksud ] && echo KSUDPATH=/data/adb/ksud",
        "echo \"APD=$(command -v apd 2>/dev/null)\"",
        "[ -x /data/adb/apd ] && echo APDPATH=/data/adb/apd",
        "echo \"ZYGISK=$(magisk --sqlite \"select value from settings where key='zygisk'\" 2>/dev/null)\"",
        "for z in zygisksu rezygisk; do if [ -d \"$S/$z\" ]; then if [ -f \"$S/$z/disable\" ] || [ -f \"$S/$z/remove\" ]; then echo ZYGDISABLED=$z; else echo ZYGMOD=$z; fi; fi; done",
        RootModuleState.PROBE);

    boolean rooted;
    Manager manager = Manager.NONE;
    String managerBinary;                 // binary/path used to install the module
    String managerLabel = "Not detected";
    boolean managerSupported;             // Magisk >= 27000, or KernelSU/APatch present
    int magiskVersion;

    String zygiskLabel = "Unknown";

    String moduleLabel = "Not installed";
    boolean toggleSupported;              // live module advertises the on/off switch (toggle-v1 marker)
    boolean injectionOff;                 // live injection-off flag set: installed but injection paused
    boolean rebootNeeded;                 // a staged update must be rebooted once to apply
    private RootModuleState module;       // pure interpretation of the module half of the probe

    boolean gameInstalled;
    long gameVersionCode;
    String gameVersionName = "";
    boolean gameVersionMatch;
    boolean gameCertTrusted;

    String rawProbe = "";                 // surfaced in the on-screen log for transparency

    /** Blocks on a root probe (bounded by RootShell), then reads the game package. Off-main-thread. */
    static RootStatus detect(Context context) {
        RootStatus status = new RootStatus();
        RootShell.Result probe = RootShell.run(PROBE, 15000);
        status.rawProbe = probe.output;
        Map<String, String> values = new HashMap<>();
        if (probe.started && !probe.timedOut) {
            for (String line : probe.output.split("\n")) {
                int eq = line.indexOf('=');
                if (eq > 0) values.put(line.substring(0, eq).trim(), line.substring(eq + 1).trim());
            }
        }
        status.rooted = "0".equals(values.get("ROOT"));
        status.resolveManager(values);
        status.resolveZygisk(values);
        status.module = RootModuleState.parse(values, status.rooted);
        status.toggleSupported = status.module.toggleSupported;
        status.injectionOff = status.module.injectionOff;
        status.rebootNeeded = status.module.rebootNeeded;
        status.moduleLabel = status.module.label;
        status.readGame(context);
        return status;
    }

    private void resolveManager(Map<String, String> values) {
        String magisk = values.get("MAGISK");
        if (magisk != null && magisk.matches("\\d+")) {
            magiskVersion = Integer.parseInt(magisk);
            manager = Manager.MAGISK;
            managerBinary = "magisk";
            managerSupported = magiskVersion >= 27000;
            managerLabel = "Magisk (" + magiskVersion + ")" + (managerSupported ? "" : " — needs v27+ (27000)");
            return;
        }
        String ksu = firstNonEmpty(values.get("KSUD"), values.get("DUSK"), values.get("KSUDPATH"));
        if (ksu != null) {
            manager = Manager.KERNELSU;
            managerBinary = ksu;
            managerSupported = true;
            managerLabel = "KernelSU (" + ksu + ")";
            return;
        }
        String apd = firstNonEmpty(values.get("APD"), values.get("APDPATH"));
        if (apd != null) {
            manager = Manager.APATCH;
            managerBinary = apd;
            managerSupported = true;
            managerLabel = "APatch (" + apd + ")";
        }
    }

    private void resolveZygisk(Map<String, String> values) {
        String module = values.get("ZYGMOD");
        String magiskSetting = values.get("ZYGISK");
        if (module != null) {
            zygiskLabel = "Configured (" + module + ")";
        } else if (magiskSetting != null && magiskSetting.contains("value=1")) {
            zygiskLabel = "Enabled";
        } else if (values.containsKey("ZYGDISABLED")) {
            zygiskLabel = "Provider disabled or pending removal";
        } else if (magiskSetting != null && magiskSetting.contains("value=0")) {
            zygiskLabel = "Disabled — turn Zygisk on";
        } else if (manager == Manager.MAGISK) {
            zygiskLabel = "Unknown — check Zygisk in Magisk";
        } else if (manager == Manager.KERNELSU || manager == Manager.APATCH) {
            zygiskLabel = "Unknown — needs Zygisk Next or ReZygisk";
        }
    }

    private void readGame(Context context) {
        try {
            PackageInfo info = context.getPackageManager()
                .getPackageInfo(InstallerConfig.GAME_PACKAGE, PackageManager.GET_SIGNING_CERTIFICATES);
            gameInstalled = true;
            gameVersionCode = info.getLongVersionCode();
            gameVersionName = info.versionName == null ? "" : info.versionName;
            gameVersionMatch = gameVersionCode == InstallerConfig.GAME_VERSION_CODE;
            gameCertTrusted = certificateTrusted(info);
        } catch (PackageManager.NameNotFoundException absent) {
            gameInstalled = false;
        }
    }

    /** True only for a single-signer APK whose certificate digest matches the trusted Niantic one. */
    private static boolean certificateTrusted(PackageInfo info) {
        try {
            SigningInfo signing = info.signingInfo;
            if (signing == null) return false;
            Signature[] signers = signing.getApkContentsSigners();
            if (signers == null || signers.length != 1) return false;
            return InstallerConfig.GAME_CERT_SHA256.equalsIgnoreCase(sha256(signers[0].toByteArray()));
        } catch (Exception unexpected) {
            return false;
        }
    }

    /** Install may begin only with root and a supported manager; everything else is advisory. */
    boolean canInstall() { return rooted && managerSupported; }

    /**
     * May the live module be switched on/off right now? Requires root, a live install carrying the
     * {@code toggle-v1} marker, and no root-manager disable or removal line. Reflects this snapshot;
     * {@link #setInjection} re-validates the live state inside the shell before it writes anything.
     */
    boolean canToggle() { return module != null && module.canToggle(); }

    /** How long the bounded on/off switch (and the Off-preservation step after an update) may run. */
    private static final long TOGGLE_TIMEOUT = 20000;

    /**
     * Turns PGRazor injection on (remove the live {@code injection-off} flag) or off (create it) in
     * one bounded su command. The command re-validates the live marker and the no-disable/no-remove
     * state, writes the flag in the live and (if present) the staged module, verifies the final state,
     * force-stops the game only after successful writes, and echoes an explicit OK/FAIL token — so a
     * partial failure never looks like success. The module stays installed either way. Off-main-thread.
     */
    RootShell.Result setInjection(boolean on) {
        if (!canToggle()) return new RootShell.Result(false,false,-1,"The installed module cannot be switched. Re-check its status.");
        RootShell.Result result = RootShell.run(RootToggleCommand.command(on), TOGGLE_TIMEOUT);
        if (result.ok() && !RootToggleCommand.confirmed(on,result.output))
            return new RootShell.Result(true,false,1,result.output + "\nSwitch confirmation was missing.");
        return result;
    }

    /**
     * Installs the module, then — if injection is currently Off, read live at this moment rather than
     * from a possibly stale snapshot — carries the Off choice into the freshly staged update so it
     * survives the reboot. The returned result keeps the install's own success/uncertainty; a failure
     * or uncertain outcome of the preservation step is appended to the output as an honest warning
     * rather than being hidden. Off-main-thread; bounded.
     */
    RootShell.Result install(File zip) {
        RootShell.Result choice = RootShell.run(RootToggleCommand.LIVE_INJECTION_PROBE, 15000);
        if (!choice.ok()) return new RootShell.Result(choice.started,choice.timedOut,1,
            "Could not read the current on/off choice; module install was not started.\n" + choice.output);
        boolean wasOff = RootToggleCommand.injectionOffFromProbe(choice.output);

        RootShell.Result installed = runInstall(zip);
        if (!installed.ok() || !wasOff) return installed;

        RootShell.Result preserved = RootShell.run(RootToggleCommand.preserveOffCommand(), TOGGLE_TIMEOUT);
        String note;
        if (preserved.ok() && RootToggleCommand.hasLine(preserved.output,RootToggleCommand.OK_PREFIX + "=preserved_off")) {
            note = "PGRazor was Off; the update keeps it Off (reboot to apply).";
        } else if (preserved.timedOut) {
            note = "WARNING: PGRazor was Off, but keeping it Off in the update could not be confirmed. "
                + "After rebooting, turn PGRazor Off again if it came back On.";
        } else {
            note = "WARNING: PGRazor was Off, but the update may not have kept it Off. "
                + "After rebooting, turn PGRazor Off again if it came back On.";
        }
        String combined = installed.output
            + (preserved.output.isEmpty() ? "" : "\n" + preserved.output)
            + "\n" + note;
        return new RootShell.Result(installed.started, installed.timedOut, installed.code, combined);
    }

    /** Runs the manager's module-install command on the copied zip. Off-main-thread; bounded. */
    private RootShell.Result runInstall(File zip) {
        String path = "\"" + zip.getAbsolutePath() + "\"";
        String command;
        switch (manager) {
            case MAGISK:
                command = "magisk --install-module " + path;
                break;
            case KERNELSU:
            case APATCH:
                command = "\"" + managerBinary + "\" module install " + path;
                break;
            default:
                return new RootShell.Result(false, false, -1, "No supported root manager detected.");
        }
        return RootShell.run(command, 180000);
    }

    /** User-tapped only. May not return if the reboot takes effect before the call completes. */
    static RootShell.Result reboot() {
        return RootShell.run("svc power reboot || reboot", 15000);
    }

    String describe() {
        StringBuilder text = new StringBuilder();
        text.append("Root access: ").append(rooted ? "granted" : "not granted").append('\n');
        text.append("Root manager: ").append(managerLabel).append('\n');
        text.append("Zygisk: ").append(zygiskLabel).append('\n');
        text.append("PGRazor module: ").append(moduleLabel).append('\n');
        text.append("Pokémon GO: ").append(describeGame());
        return text.toString();
    }

    private String describeGame() {
        if (!gameInstalled) return "not installed";
        String base = (gameVersionName.isEmpty() ? "?" : gameVersionName) + " (" + gameVersionCode + ")";
        if (!gameCertTrusted) return base + " — not the official Niantic signature";
        if (gameVersionMatch) return base + " — supported";
        return base + " — not " + InstallerConfig.GAME_VERSION_NAME + "; PGRazor will stay off until it matches";
    }

    private static String firstNonEmpty(String... options) {
        for (String option : options) {
            if (option != null && !option.isEmpty()) return option;
        }
        return null;
    }

    private static String sha256(byte[] data) throws Exception {
        byte[] digest = MessageDigest.getInstance("SHA-256").digest(data);
        StringBuilder hex = new StringBuilder(digest.length * 2);
        for (byte b : digest) hex.append(Character.forDigit((b >> 4) & 0xF, 16)).append(Character.forDigit(b & 0xF, 16));
        return hex.toString();
    }
}
