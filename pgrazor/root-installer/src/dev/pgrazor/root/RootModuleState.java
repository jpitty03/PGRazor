package dev.pgrazor.root;

import java.util.Map;

/**
 * Pure interpretation of the PGRazor module's installed state and on/off switch, derived from the
 * parsed root probe. Holds no Android dependency, so {@link #parse} and the derived flags/label run
 * on a plain JVM and can be exercised without a device.
 *
 * <p>"Installed" means the live module directory carries a {@code module.prop}. The on/off switch is
 * the live {@code injection-off} flag; it is available only when the live module advertises the
 * {@code toggle-v1} marker and the root manager has not disabled or removal-marked the module. The
 * module stays installed either way — Off only pauses injection into the game, it does not uninstall.
 * A staged update under {@code modules_update} needs one reboot to apply ({@link #rebootNeeded}).
 */
final class RootModuleState {
    static final String PROBE = String.join("\n",
        "M=/data/adb/modules/pgrazor",
        "[ -f \"$M/module.prop\" ] && echo \"MODVER=$(sed -n 's/^version=//p' \"$M/module.prop\")\"",
        "[ -f \"$M/pgrazor/toggle-v1\" ] && echo MODTOGGLE=1",
        "[ -f \"$M/pgrazor/injection-off\" ] && echo MODOFF=1",
        "[ -f \"$M/disable\" ] && echo MODDISABLED=1",
        "[ -f \"$M/remove\" ] && echo MODREMOVE=1",
        "[ -d /data/adb/modules_update/pgrazor ] && echo MODPENDING=1",
        "true");
    final boolean rooted;
    final boolean installed;        // live module.prop present
    final String version;           // live module version, or null when not installed
    final boolean toggleSupported;  // live toggle-v1 marker present (module understands the switch)
    final boolean injectionOff;     // live injection-off flag present (installed but injection paused)
    final boolean disabled;         // live disable flag present (root manager disabled the module)
    final boolean removalPending;   // live remove flag present (module marked for removal)
    final boolean updatePending;    // a staged update waits under modules_update
    final boolean rebootNeeded;     // a pending update needs one reboot to apply
    final String label;             // honest, user-facing one-line status

    private RootModuleState(boolean rooted, boolean installed, String version, boolean toggleSupported,
                            boolean injectionOff, boolean disabled, boolean removalPending,
                            boolean updatePending) {
        this.rooted = rooted;
        this.installed = installed;
        this.version = version;
        this.toggleSupported = toggleSupported;
        this.injectionOff = injectionOff;
        this.disabled = disabled;
        this.removalPending = removalPending;
        this.updatePending = updatePending;
        this.rebootNeeded = updatePending || removalPending;
        this.label = label(rooted, installed, version, toggleSupported, injectionOff, disabled,
            removalPending, updatePending);
    }

    /** Reads the module half of a parsed probe (keys set by {@code RootStatus.MODULE_PROBE}). */
    static RootModuleState parse(Map<String, String> values, boolean rooted) {
        return new RootModuleState(
            rooted,
            values.get("MODVER") != null,
            values.get("MODVER"),
            "1".equals(values.get("MODTOGGLE")),
            "1".equals(values.get("MODOFF")),
            "1".equals(values.get("MODDISABLED")),
            "1".equals(values.get("MODREMOVE")),
            "1".equals(values.get("MODPENDING")));
    }

    /**
     * May the live module be switched on/off? Requires root, a live install, the {@code toggle-v1}
     * marker, and no disable or removal line. A pending update does not block toggling the live
     * module; it only means a reboot is still owed to apply that update.
     */
    boolean canToggle() {
        return rooted && installed && toggleSupported && !disabled && !removalPending;
    }

    private static String label(boolean rooted, boolean installed, String version, boolean toggleSupported,
                                boolean injectionOff, boolean disabled, boolean removalPending,
                                boolean updatePending) {
        if (!installed) {
            if (updatePending) return "Install pending — reboot to activate";
            return rooted ? "Not installed" : "Unknown (root access required)";
        }
        String state;
        if (removalPending) {
            state = "removal pending";
        } else if (disabled) {
            state = "disabled";
        } else if (toggleSupported) {
            state = injectionOff ? "PGRazor off (module stays installed)" : "PGRazor on";
        } else {
            state = "update and reboot once to use the on/off switch";
        }
        String text = "Installed " + version + " — " + state;
        if (updatePending) text += "; update pending, reboot to apply";
        return text;
    }
}
