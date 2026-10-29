package dev.pgrazor;

import android.location.Location;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;

/** Compiled policy and process-local ART ownership for the four Location getters. */
public final class NativeLocationGetters {
    private static boolean attempted;
    private static volatile boolean installed;
    private static final Handler[] handlers = new Handler[4];
    private NativeLocationGetters() {}

    public static synchronized boolean install() throws ReflectiveOperationException {
        if (attempted) return installed;
        attempted = true;
        if (!NativeJavaHooks.ready()) return false;
        String[] names = {"getLatitude", "getLongitude", "getSpeed", "getBearing"};
        Method callback = Handler.class.getMethod("invoke", Object[].class);
        for (int i = 0; i < names.length; i++) {
            Method target = Location.class.getDeclaredMethod(names[i]);
            Class<?> expected = i < 2 ? double.class : float.class;
            if (target.getReturnType() != expected) return false;
            Handler handler = new Handler(i);
            // A concurrent first callback waits for backup publication. Earlier
            // published hooks stay passive if a later hook is refused. Allocation
            // failure inside the hook engine after patching is a terminal install
            // failure; it cannot be advertised as a safe fallback to another engine.
            synchronized (handler) {
                handler.backup = NativeJavaHooks.hook(target, handler, callback);
                if (handler.backup == null) return false;
                handler.backup.setAccessible(true);
                handlers[i] = handler;
            }
        }
        installed = true;
        return true;
    }

    public static boolean isInstalled() { return installed; }

    // Rewritten game DEX calls this boundary, including callers ART may inline.
    // The backup bypasses our hook, so OFF telemetry is emitted exactly once.
    static Number read(int kind, Location location) throws Throwable {
        if (location == null) throw new NullPointerException("Location receiver");
        if (installed) return (Number) handlers[kind].invoke(new Object[]{location});
        switch (kind) {
            case 0: return Double.valueOf(location.getLatitude());
            case 1: return Double.valueOf(location.getLongitude());
            case 2: return Float.valueOf(location.getSpeed());
            case 3: return Float.valueOf(location.getBearing());
            default: throw new IllegalArgumentException("Location getter kind");
        }
    }

    public static final class Handler {
        private final int kind;
        private Method backup;
        Handler(int kind) { this.kind = kind; }

        public Object invoke(Object[] args) throws Throwable {
            if (args == null || args.length != 1 || !(args[0] instanceof Location))
                throw new IllegalArgumentException("Location getter receiver");
            Method original;
            synchronized (this) { original = backup; }
            if (original == null) throw new IllegalStateException("Location backup not published");
            if (installed && Control.active()) {
                switch (kind) {
                    case 0: return Double.valueOf(Control.latitude());
                    case 1: return Double.valueOf(Control.longitude());
                    case 2: return Float.valueOf(Control.speed());
                    case 3: return Float.valueOf(Control.bearing());
                    default: throw new IllegalStateException("Location getter kind");
                }
            }
            Object result;
            try { result = original.invoke(args[0]); }
            catch (InvocationTargetException error) { throw error.getCause(); }
            if (installed && kind < 2) {
                try {
                    if (!Control.desiredEnabled())
                        Control.observeRealCoordinate(kind == 0, ((Double) result).doubleValue());
                } catch (Throwable ignored) { /* Telemetry cannot change the original getter. */ }
            }
            return result;
        }
    }
}
