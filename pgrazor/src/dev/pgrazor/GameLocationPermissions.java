package dev.pgrazor;

import java.lang.reflect.Method;

/**
 * Compiled location-permission policy for the selected native pilot.
 *
 * <p>This helper owns the decisions the JS adapter used to make inline: whether the game's
 * foreground-location gates should see a synthetic grant, what permission-status value to
 * return for the Niantic location flags, and whether an Adventure Sync dialog should be
 * dismissed with its own negative action. It never grants a real Android OS permission, never
 * hooks a {@code Context} grantor, and only ever reads {@code checkSelfPermission} to decide
 * whether the dialog is being shown over a genuinely denied grant.
 *
 * <p>The game classes are resolved by reflection through the application class loader so this
 * class compiles with no game dependencies. {@link #create()} resolves the exact metadata it
 * needs and throws if any of it is missing: there is no partial, silent override.
 */
public final class GameLocationPermissions {
    private static final String FINE = "android.permission.ACCESS_FINE_LOCATION";
    private static final String COARSE = "android.permission.ACCESS_COARSE_LOCATION";
    private static final String PERMISSIONS_CLASS = "com.nianticlabs.platform.niaadvsynccore.permission.Permissions";
    private static final String STATUS_CLASS = "com.nianticlabs.platform.niaadvsynccore.permission.PermissionStatus";
    private static final String CONFIG_PKG = "com.nianticlabs.background.configuration.";
    private static final String VIEW_CLASS = "android.view.View";

    // Object-scoped: a shutdown flips this false immediately, so no in-flight callback can
    // keep overriding after the adapter decides to stop. Combined with Control.desiredEnabled()
    // this gates every override this helper can produce.
    private volatile boolean active = true;

    private final long locationFlag;
    private final long preciseFlag;
    private final Object grantedInUse;
    private final Object grantedAlways;

    private final Class<?> initialDialog;
    private final Class<?> manualDialog;
    private final Class<?> finalDialog;
    private final Class<?> viewClass;

    private GameLocationPermissions(long locationFlag, long preciseFlag, Object grantedInUse, Object grantedAlways,
                                    Class<?> initialDialog, Class<?> manualDialog, Class<?> finalDialog, Class<?> viewClass) {
        this.locationFlag = locationFlag;
        this.preciseFlag = preciseFlag;
        this.grantedInUse = grantedInUse;
        this.grantedAlways = grantedAlways;
        this.initialDialog = initialDialog;
        this.manualDialog = manualDialog;
        this.finalDialog = finalDialog;
        this.viewClass = viewClass;
    }

    /**
     * Resolve the exact Niantic permission metadata and return a ready helper. Throws if any
     * required flag or status value cannot be read: the caller must not install partial hooks.
     */
    public static GameLocationPermissions create() {
        try {
            Class<?> permissions = load(PERMISSIONS_CLASS);
            Class<?> status = load(STATUS_CLASS);
            // Early startup (notably MuMu) can expose null enum fields before the class
            // initializer has run. Force the enum API first, exactly like the legacy adapter.
            permissions.getMethod("values").invoke(null);
            status.getMethod("values").invoke(null);
            long locationFlag = readFlag(permissions, "LOCATION");
            long preciseFlag = readFlag(permissions, "PRECISE_LOCATION");
            Object grantedInUse = readStatus(status, "GRANTED_IN_USE");
            Object grantedAlways = readStatus(status, "GRANTED_ALWAYS");
            // The dialog classes are optional: a build without them simply never dismisses.
            Class<?> initial = tryLoad(CONFIG_PKG + "InitialDialog");
            Class<?> manual = tryLoad(CONFIG_PKG + "ManualPermissionDialog");
            Class<?> last = tryLoad(CONFIG_PKG + "FinalDialog");
            Class<?> view = tryLoad(VIEW_CLASS);
            return new GameLocationPermissions(locationFlag, preciseFlag, grantedInUse, grantedAlways,
                initial, manual, last, view);
        } catch (RuntimeException error) {
            throw error;
        } catch (Exception error) {
            throw new IllegalStateException("location-permission-metadata: " + error, error);
        }
    }

    /**
     * True only for the exact fine/coarse foreground-location permission names while the
     * override is active and the user preference is enabled. Any other name is rejected.
     */
    public boolean locationPermission(String permission) {
        return active && Control.desiredEnabled() && (FINE.equals(permission) || COARSE.equals(permission));
    }

    /**
     * The Niantic permission-status value to report for the given location flag, or {@code null}
     * when this helper has no override (disabled, shut down, or an unrelated flag).
     */
    public Object permissionStatus(long flag) {
        if (!active || !Control.desiredEnabled()) return null;
        if (flag == locationFlag) return grantedInUse;
        if (flag == preciseFlag) return grantedAlways;
        return null;
    }

    /**
     * Dismiss an Adventure Sync dialog using the game's own negative/acknowledge action. This
     * fires only when the override is active and enabled, the activity is a whitelisted dialog
     * with the matching action, it is not finishing, and the real OS location grant is denied
     * for both fine and coarse. Any reflection failure is contained and logged.
     */
    public void dismissAdventureSync(Object activity, String action) {
        try {
            if (!active || !Control.desiredEnabled() || activity == null) return;
            boolean isFinal = finalDialog != null && finalDialog.isInstance(activity);
            boolean isCancel = (initialDialog != null && initialDialog.isInstance(activity))
                || (manualDialog != null && manualDialog.isInstance(activity));
            String label;
            if (isFinal) {
                if (!"onClickClose".equals(action)) return;
                label = "FinalDialog";
            } else if (isCancel) {
                if (!"onClickCancel".equals(action)) return;
                label = initialDialog != null && initialDialog.isInstance(activity) ? "InitialDialog" : "ManualPermissionDialog";
            } else {
                return; // Not a whitelisted dialog: never invoke an action on an unknown caller.
            }
            Class<?> ac = activity.getClass();
            if ((Boolean) ac.getMethod("isFinishing").invoke(activity)) return;
            // These OS checks are intentionally NOT the game's synthetic permission gates; they
            // observe the real grant and are never modified by this helper.
            Method checkSelf = ac.getMethod("checkSelfPermission", String.class);
            int fine = (Integer) checkSelf.invoke(activity, FINE);
            int coarse = (Integer) checkSelf.invoke(activity, COARSE);
            if (fine == 0 || coarse == 0) return; // At least one real grant exists: leave the dialog alone.
            Class<?> view = viewClass != null ? viewClass : tryLoad(VIEW_CLASS);
            if (view == null) return;
            Method negative = ac.getMethod(action, view);
            if (!active || !Control.desiredEnabled()) return;
            negative.invoke(activity, new Object[]{null}); // Preserve the game's null-View callback contract.
            Control.event("adventure-sync-dismissed", label + ": app location permission denied");
        } catch (Throwable error) {
            try { Control.event("adventure-sync-dismiss-error", String.valueOf(error)); }
            catch (Throwable ignored) { }
        }
    }

    /** Immediately stop producing overrides. Idempotent. */
    public void shutdown() {
        active = false;
    }

    private static long readFlag(Class<?> permissions, String name) throws Exception {
        Object constant = permissions.getField(name).get(null);
        if (constant == null) throw new IllegalStateException("permission-null: " + name);
        // Frida's .value is a field wrapper, not a field on the Java enum instance.
        Object flag = permissions.getMethod("getValue").invoke(constant);
        if (!(flag instanceof Number)) throw new IllegalStateException("permission-flag-type: " + name);
        return ((Number) flag).longValue();
    }

    private static Object readStatus(Class<?> status, String name) throws Exception {
        Object constant = status.getField(name).get(null);
        if (constant == null) throw new IllegalStateException("status-null: " + name);
        return constant;
    }

    private static Class<?> load(String name) throws ClassNotFoundException {
        ClassNotFoundException last = null;
        for (ClassLoader loader : loaders()) {
            if (loader == null) continue;
            try {
                return Class.forName(name, true, loader);
            } catch (ClassNotFoundException missing) {
                last = missing;
            }
        }
        try {
            return Class.forName(name);
        } catch (ClassNotFoundException missing) {
            throw last != null ? last : missing;
        }
    }

    private static Class<?> tryLoad(String name) {
        try {
            return load(name);
        } catch (Throwable missing) {
            return null;
        }
    }

    private static ClassLoader[] loaders() {
        return new ClassLoader[]{
            Thread.currentThread().getContextClassLoader(),
            GameLocationPermissions.class.getClassLoader(),
            ClassLoader.getSystemClassLoader()
        };
    }
}
