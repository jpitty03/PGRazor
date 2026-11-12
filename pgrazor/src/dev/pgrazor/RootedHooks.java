package dev.pgrazor;

import android.content.Context;
import android.util.Log;
import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

/**
 * PGRazor Root's replacement for the patched APK's DEX rewrites (tools/dex_bridge/BridgeRewriter):
 * the same game methods and the same GameBridgeDispatch calls, installed as ART hooks on the
 * unmodified game instead of rewritten method bodies.
 *
 * <p>Google browser sign-in is not hooked: the official app's own Google sign-in works. Location
 * callers that ART may have compiled with Location getters inlined are deoptimized so the getter
 * hooks (NativeLocationGetters) see them, standing in for the patched build's call-site rewrites.
 */
final class RootedHooks {
    private RootedHooks() {}
    // A partial installation forwards every call unchanged. No bridge becomes
    // active until the complete supported set has been installed successfully.
    private static volatile boolean active;

    private static final String[][] SPECS = {
        // kind, class, method, parameter types (framework classes only, or J for long)
        {"permission", "com.unity3d.player.UnityPermissions", "hasUserAuthorizedPermission", "android.content.Context", "java.lang.String"},
        {"permissionActivity", "com.unity3d.plugin.UnityAndroidPermissions", "IsPermissionGranted", "android.app.Activity", "java.lang.String"},
        {"status", "com.nianticlabs.platform.niaadvsyncpermissions.internal.InternalPermissionManager", "permissionStatus", "J"},
        {"afterStart", "com.nianticlabs.nia.contextservice.ContextService", "invokeOnStart"},
        {"afterResume", "com.nianticlabs.nia.contextservice.ContextService", "invokeOnResume"},
        {"beforeStop", "com.nianticlabs.nia.contextservice.ContextService", "invokeOnStop"},
        {"beforePause", "com.nianticlabs.nia.contextservice.ContextService", "invokeOnPause"},
        {"dialogCancel", "com.nianticlabs.background.configuration.InitialDialog", "onCreate", "android.os.Bundle"},
        {"dialogCancel", "com.nianticlabs.background.configuration.ManualPermissionDialog", "onCreate", "android.os.Bundle"},
        {"dialogClose", "com.nianticlabs.background.configuration.FinalDialog", "onCreate", "android.os.Bundle"},
        {"renderLoop", "com.unity3d.player.W", "handleMessage", "android.os.Message"},
    };

    /** Installs every bridge hook; false if any could not be installed (PGRazor then stays off). */
    static boolean install(Context context, ClassLoader game) {
        Method callback;
        try { callback = Hook.class.getMethod("invoke", Object[].class); }
        catch (NoSuchMethodException impossible) { return false; }
        int installed = 0;
        for (String[] spec : SPECS) {
            try {
                Class<?> owner = Class.forName(spec[1], false, game);
                Class<?>[] parameters = new Class<?>[spec.length - 3];
                for (int i = 3; i < spec.length; i++)
                    parameters[i - 3] = spec[i].equals("J") ? long.class : Class.forName(spec[i]);
                Method target = owner.getDeclaredMethod(spec[2], parameters);
                Hook hook = new Hook(spec[0], Modifier.isStatic(target.getModifiers()));
                synchronized (hook) {
                    hook.backup = NativeJavaHooks.hook(target, hook, callback);
                    if (hook.backup == null) throw new IllegalStateException("hook refused");
                    hook.backup.setAccessible(true);
                }
                installed++;
            } catch (Throwable failure) {
                Log.e(RootedRuntime.TAG, "bridge hook failed: " + spec[1] + "." + spec[2] + ": " + failure);
            }
        }
        Log.i(RootedRuntime.TAG, "bridge hooks " + installed + "/" + SPECS.length);
        if (installed != SPECS.length) return false;
        deoptimizeLocationCallers(context, game);
        active = true;
        return true;
    }

    /** One hooked game method; dispatches exactly like the patched build's rewritten body. */
    public static final class Hook {
        private final String kind;
        private final boolean statik;
        private Method backup;

        Hook(String kind, boolean statik) { this.kind = kind; this.statik = statik; }

        public Object invoke(Object[] args) throws Throwable {
            Method original;
            synchronized (this) { original = backup; }
            if (original == null) throw new IllegalStateException("Bridge backup not published");
            Object self = statik ? null : args[0];
            Object[] params = new Object[args.length - (statik ? 0 : 1)];
            System.arraycopy(args, statik ? 0 : 1, params, 0, params.length);
            if (!active) return call(original, self, params);
            switch (kind) {
                case "permission":
                    if (GameBridgeDispatch.locationPermission((String) params[1])) return Boolean.TRUE;
                    return call(original, self, params);
                case "permissionActivity":
                    if (params[0] != null && GameBridgeDispatch.locationPermission((String) params[1])) return Boolean.TRUE;
                    return call(original, self, params);
                case "status": {
                    Object status = GameBridgeDispatch.permissionStatus((Long) params[0]);
                    return status != null ? status : call(original, self, params);
                }
                case "afterStart": { Object result = call(original, self, params); GameBridgeDispatch.afterStart(self); return result; }
                case "afterResume": { Object result = call(original, self, params); GameBridgeDispatch.afterResume(self); return result; }
                case "beforeStop": GameBridgeDispatch.beforeStop(self); return call(original, self, params);
                case "beforePause": GameBridgeDispatch.beforePause(self); return call(original, self, params);
                case "dialogCancel": { Object result = call(original, self, params); GameBridgeDispatch.dismissDialog(self, "onClickCancel"); return result; }
                case "dialogClose": { Object result = call(original, self, params); GameBridgeDispatch.dismissDialog(self, "onClickClose"); return result; }
                case "renderLoop": {
                    // Runs after Unity's render-loop message returns normally. onRendered retries
                    // until IL2CPP is ready and is a no-op once gameplay has started.
                    Object result = call(original, self, params);
                    if (params[0] instanceof android.os.Message && ((android.os.Message) params[0]).what == 2269) {
                        BootstrapProvider.startRootedNativeAfterRender();
                        NativeRuntimeBootstrap.onRendered();
                    }
                    return result;
                }
                default: throw new IllegalStateException("Unknown bridge " + kind);
            }
        }

        private static Object call(Method original, Object self, Object[] params) throws Throwable {
            try { return original.invoke(self, params); }
            catch (InvocationTargetException error) { throw error.getCause(); }
        }
    }

    /** Game methods that call Location getters, listed at packaging time from the game's DEX. */
    private static void deoptimizeLocationCallers(Context context, ClassLoader game) {
        int done = 0, skipped = 0;
        try (BufferedReader in = new BufferedReader(new InputStreamReader(
                PgAssets.open(context, "pgrazor/rooted/location-callers.txt"), StandardCharsets.UTF_8))) {
            String line;
            while ((line = in.readLine()) != null) {
                line = line.trim();
                if (line.isEmpty()) continue;
                try {
                    Method method = resolve(line, game);
                    if (method != null && NativeJavaHooks.deoptimize(method)) done++; else skipped++;
                } catch (Throwable failure) {
                    skipped++;
                }
            }
        } catch (Throwable failure) {
            Log.e(RootedRuntime.TAG, "Location caller list unavailable: " + failure);
        }
        Log.i(RootedRuntime.TAG, "Location callers deoptimized " + done + ", skipped " + skipped);
    }

    /** Resolves {@code Lpkg/Cls;->name(params)ret}; constructors are skipped (null). */
    private static Method resolve(String descriptor, ClassLoader game) throws ClassNotFoundException, NoSuchMethodException {
        int arrow = descriptor.indexOf("->"), open = descriptor.indexOf('(', arrow), close = descriptor.indexOf(')', open);
        String name = descriptor.substring(arrow + 2, open);
        if (name.startsWith("<")) return null;
        Class<?> owner = type(descriptor.substring(0, arrow), game);
        List<Class<?>> parameters = new ArrayList<>();
        String list = descriptor.substring(open + 1, close);
        for (int i = 0; i < list.length(); ) {
            int start = i;
            while (list.charAt(i) == '[') i++;
            if (list.charAt(i) == 'L') i = list.indexOf(';', i);
            i++;
            parameters.add(type(list.substring(start, i), game));
        }
        return owner.getDeclaredMethod(name, parameters.toArray(new Class<?>[0]));
    }

    private static Class<?> type(String descriptor, ClassLoader game) throws ClassNotFoundException {
        switch (descriptor) {
            case "Z": return boolean.class;
            case "B": return byte.class;
            case "C": return char.class;
            case "S": return short.class;
            case "I": return int.class;
            case "J": return long.class;
            case "F": return float.class;
            case "D": return double.class;
            default:
                String name = descriptor.startsWith("[") ? descriptor.replace('/', '.')
                    : descriptor.substring(1, descriptor.length() - 1).replace('/', '.');
                return Class.forName(name, false, game);
        }
    }
}
