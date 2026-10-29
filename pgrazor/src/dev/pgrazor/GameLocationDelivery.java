package dev.pgrazor;

import android.content.Context;
import android.location.Location;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationHandler;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;

/**
 * Compiled Java port of the runtime {@code installLocationDelivery} adapter (runtime/location.js).
 *
 * <p>This class owns the synthetic-fix delivery state machine only; it never installs any hook. The
 * Frida layer keeps its four private {@code ContextService} dispatch hooks ({@code invokeOnStart},
 * {@code invokeOnStop}, {@code invokeOnPause}, {@code invokeOnResume}) and drives this object through
 * {@link #start}, {@link #resume}, {@link #pause} and {@link #stop}. There is no ART/Dobby hook here
 * and {@code nativeLocationUpdate} is reflectively invoked as its original method, never replaced.
 *
 * <p>All game types are reached by reflection so the module carries no compile dependency on the
 * game. The handful of framework types ({@link Handler}, {@link Location}, {@link Context}) are the
 * platform's own and are used directly. Lifecycle calls and the periodic tick normally run on the service
 * handler thread; shutdown may arrive from the host thread; provider callbacks arrive on arbitrary threads and only touch their own immutable
 * {@link Record} plus the manager's {@code callbackLock}. State shared across those threads is
 * {@code volatile}; no internal state lock is ever held across a native call, a provider setter or an
 * original callback.
 *
 * <p>Behaviour mirrors the legacy adapter exactly: a strong reference to the current manager, a single
 * {@link Runnable} reposted every 1000ms (first post immediate), pause cancels the timer, resume
 * schedules it once, switching managers restores the old providers' listeners and cancels old pending
 * work, and {@link #shutdown()} detaches everything irreversibly. Each tick attaches newly seen
 * providers by wrapping them in a {@link Proxy} that delivers a synthetic fix (suppressing the original
 * callback only on a successful send) while active, and otherwise forwards the original callback
 * unchanged. Satellite callbacks are always forwarded.
 */
public final class GameLocationDelivery {
    // Resolved metadata (set once by create()/initialize(); treated as final afterwards).
    private Class<?> managerClass;
    private Class<?> listenerInterface;
    private ClassLoader proxyLoader;
    private Method nativeLocationUpdate;
    private Method getContextMethod;
    private Field providersField;
    private Field callbackLockField;
    private Field nativePointerField;
    private Handler handler;
    private Runnable runnable;

    // Delivery state. The manager/paused/shutdown flags are read from provider-callback threads, so
    // they are volatile; records and timer generations are protected by stateLock.
    private volatile Object manager;
    private volatile boolean paused;
    private volatile boolean shutdown;
    private volatile boolean first;
    private volatile boolean faultLogged;
    private final List<Record> records = new ArrayList<>();
    private final Object stateLock = new Object();
    private long generation, scheduleRevision;

    /** Inert constructor: nothing is resolved or captured until {@link #create()} runs. */
    private GameLocationDelivery() {}

    /**
     * Resolves the game metadata and acquires the service handler on the main thread. The instance is
     * installed (ready to receive lifecycle calls) once this returns.
     *
     * @throws IllegalStateException if called off the main thread or if any required class, method or
     *     field is missing; the cause carries the original (unwrapped) reflection failure.
     */
    public static GameLocationDelivery create() {
        GameLocationDelivery delivery = new GameLocationDelivery();
        delivery.initialize();
        return delivery;
    }

    private void initialize() {
        if (Looper.myLooper() != Looper.getMainLooper())
            throw new IllegalStateException("GameLocationDelivery.create must run on the main thread");
        try {
            managerClass = gameClass("com.nianticlabs.nia.location.NianticLocationManager");
            Class<?> serviceClass = gameClass("com.nianticlabs.nia.contextservice.ContextService");
            listenerInterface = gameClass("com.nianticlabs.nia.location.NianticLocationProvider$ProviderListener");
            proxyLoader = listenerInterface.getClassLoader();
            // Exact parameter types pin the one native overload; a mismatch would silently deliver wrong.
            nativeLocationUpdate = findMethod(managerClass, "nativeLocationUpdate",
                String.class, Location.class, int.class, Context.class);
            getContextMethod = findMethod(managerClass, "getContext");
            providersField = findField(managerClass, "providers");
            callbackLockField = findField(managerClass, "callbackLock");
            nativePointerField = findField(managerClass, "nativeClassPointer");
            Method getServiceHandler = findMethod(serviceClass, "getServiceHandler");
            Object resolvedHandler = getServiceHandler.invoke(null);
            if (!(resolvedHandler instanceof Handler))
                throw new IllegalStateException("ContextService.getServiceHandler returned no Handler");
            handler = (Handler) resolvedHandler;
            runnable = new Runnable() { public void run() { tick(); } };
        } catch (IllegalStateException error) {
            throw error;
        } catch (Throwable error) {
            Throwable cause = error instanceof InvocationTargetException ? unwrap((InvocationTargetException) error) : error;
            throw new IllegalStateException("Game location metadata unavailable: " + cause, cause);
        }
        Control.event("location-delivery", "Game location adapter installed; OS permission unchanged");
    }

    // --- Public lifecycle (driven by the Frida dispatch hooks on the handler thread) ---------------

    /** Starts/captures delivery for {@code owner}; non-manager owners are ignored. */
    public void start(Object owner) {
        if (owner == null || !managerClass.isInstance(owner)) return;
        capture(owner);
    }

    /** Resumes delivery for {@code owner}; identical capture to {@link #start}. */
    public void resume(Object owner) {
        start(owner);
    }

    /** Pauses the timer, but only for the currently captured manager. */
    public void pause(Object owner) {
        synchronized (stateLock) {
            if (owner == null || !isSame(owner)) return;
            paused = true;
            scheduleRevision++;
            handler.removeCallbacks(runnable);
        }
    }

    public void stop(Object owner) {
        List<Record> retired;
        synchronized (stateLock) {
            if (owner == null || !isSame(owner)) return;
            retired = detachLocked();
        }
        restoreRecords(retired);
    }

    public void shutdown() {
        List<Record> retired;
        synchronized (stateLock) {
            if (shutdown) return;
            shutdown = true;
            retired = detachLocked();
        }
        restoreRecords(retired);
    }

    private void capture(Object owner) {
        List<Record> retired;
        long expected;
        synchronized (stateLock) {
            if (shutdown) return;
            if (isSame(owner)) { scheduleLocked(); return; }
            retired = detachLocked();
            expected = generation;
        }
        restoreRecords(retired);
        synchronized (stateLock) {
            // A setter may have restarted or shut down delivery while restoring. Preserve it.
            if (shutdown || generation != expected || manager != null) return;
            manager = owner;
            first = true;
            faultLogged = false;
            scheduleLocked();
        }
    }

    private void scheduleLocked() {
        paused = false;
        scheduleRevision++;
        handler.removeCallbacks(runnable);
        handler.post(runnable);
    }

    private List<Record> detachLocked() {
        manager = null;
        generation++;
        scheduleRevision++;
        handler.removeCallbacks(runnable);
        List<Record> retired = new ArrayList<>(records);
        records.clear();
        for (Record record : retired) record.active = false;
        return retired;
    }

    private void tick() {
        Object current;
        long epoch, revision;
        synchronized (stateLock) {
            if (shutdown || manager == null || paused) return;
            current = manager; epoch = generation; revision = scheduleRevision;
        }
        try {
            attachProviders(current, epoch);
            if (isEnabled(current)) deliver(current, "fused", epoch, null);
        } catch (Throwable error) {
            logFaultBounded("tick", error);
        } finally {
            synchronized (stateLock) {
                if (!shutdown && manager == current && generation == epoch && !paused
                        && scheduleRevision == revision) handler.postDelayed(runnable, 1000);
            }
        }
    }

    private void attachProviders(Object current, long epoch) throws Throwable {
        Object providersObject = getFieldValue(providersField, current);
        if (!(providersObject instanceof Map)) return;
        List<Object> snapshot = new ArrayList<>(((Map<?, ?>) providersObject).values());
        for (Object providerObject : snapshot) {
            synchronized (stateLock) {
                if (shutdown || manager != current || generation != epoch) return;
                if (providerObject == null || knownLocked(providerObject)) continue;
            }
            Class<?> providerClass = providerObject.getClass();
            Field listenerField = findField(providerClass, "providerListener");
            Method getProviderName = findMethod(providerClass, "getProviderName");
            Method setListener = findSetListener(providerClass);
            Object original = listenerField.get(providerObject);
            if (original == null) continue;
            String name = String.valueOf(getProviderName.invoke(providerObject));
            Record record = new Record(providerObject, original, name, current, listenerField, setListener, epoch);
            record.proxy = Proxy.newProxyInstance(proxyLoader, new Class<?>[]{listenerInterface}, record);
            synchronized (stateLock) {
                if (shutdown || manager != current || generation != epoch) return;
                if (knownLocked(providerObject)) continue;
                record.active = true;
                records.add(record);
            }
            try {
                setListener.invoke(providerObject, record.proxy);
            } catch (Throwable error) {
                synchronized (stateLock) { record.active = false; records.remove(record); }
                // Some setters mutate before throwing. Restore only our own installed proxy.
                restoreRecord(record);
                throw error instanceof InvocationTargetException ? unwrap((InvocationTargetException) error) : error;
            }
            boolean stale;
            synchronized (stateLock) {
                stale = !record.active || shutdown || manager != current || generation != epoch;
                if (stale) { record.active = false; records.remove(record); }
            }
            if (stale) { restoreRecord(record); return; }
        }
    }

    private boolean knownLocked(Object providerObject) {
        for (Record record : records) if (record.provider == providerObject) return true;
        return false;
    }

    private void restoreRecords(List<Record> retired) {
        for (Record record : retired) restoreRecord(record);
    }

    private void restoreRecord(Record record) {
        record.active = false;
        try {
            if (record.listenerField.get(record.provider) == record.proxy)
                record.setListener.invoke(record.provider, record.original);
        } catch (Throwable error) {
            logFaultBounded("listener-restore",
                error instanceof InvocationTargetException ? unwrap((InvocationTargetException) error) : error);
        }
    }

    private boolean valid(Object current, long epoch, Record record) {
        synchronized (stateLock) {
            return !shutdown && !paused && manager == current && generation == epoch
                && (record == null || record.active);
        }
    }

    // --- Eligibility and synthetic delivery -------------------------------------------------------

    private boolean isSame(Object owner) {
        Object current = manager;
        return current != null && current == owner;
    }

    private boolean isEnabled(Object owner) {
        return !shutdown && !paused && isSame(owner) && Control.active();
    }

    /**
     * Delivers one synthetic fix under the manager's {@code callbackLock}, re-checking eligibility and
     * the native pointer inside the lock so an old real fix can never slip between timer ticks.
     *
     * @return true only if the native update was actually invoked (the caller then suppresses the
     *     original callback); false means the caller must forward the original callback.
     */
    private boolean deliver(Object current, String providerName, long epoch, Record record) throws Throwable {
        Object lock = getFieldValue(callbackLockField, current);
        if (lock == null) return false;
        synchronized (lock) {
            if (!valid(current, epoch, record) || !Control.active()) return false;
            if (nativePointerIsZero(getFieldValue(nativePointerField, current))) return false;
            Location fix = new Location(providerName);
            fix.setLatitude(Control.latitude());
            fix.setLongitude(Control.longitude());
            fix.setSpeed(Control.speed());
            fix.setBearing(Control.bearing());
            fix.setAltitude(10);
            fix.setAccuracy(5);
            fix.setTime(System.currentTimeMillis());
            fix.setElapsedRealtimeNanos(SystemClock.elapsedRealtimeNanos());
            Object context;
            try {
                context = getContextMethod.invoke(current);
            } catch (InvocationTargetException error) {
                throw unwrap(error);
            }
            if (!valid(current, epoch, record) || !Control.active()
                    || nativePointerIsZero(getFieldValue(nativePointerField, current))) return false;
            try {
                nativeLocationUpdate.invoke(current, providerName, fix, 3, context);
            } catch (InvocationTargetException error) {
                throw unwrap(error);
            }
            if (first) {
                first = false;
                try { Control.event("location-delivery", "Synthetic location updates active"); }
                catch (Throwable ignored) { } // A logging failure must not deliver the original too.
            }
            return true;
        }
    }

    private void logFaultBounded(String where, Throwable error) {
        if (faultLogged) return;
        faultLogged = true;
        try {
            Control.event("location-delivery-error", where + ": " + error);
        } catch (Throwable ignored) {
            // Never let a logging failure drop the original callback.
        }
    }

    // --- Proxy backing a single wrapped provider --------------------------------------------------

    /**
     * One wrapped provider. Each proxy's {@link InvocationHandler} is its own record, so a callback can
     * always reach {@link #original} directly; a stale callback (after restore/switch/stop) therefore
     * forwards the original instead of being dropped. The {@link #active} flag plus manager identity
     * keep stale callbacks from suppressing originals.
     */
    private final class Record implements InvocationHandler {
        private final Object provider;
        private final Object original;
        private final String name;
        private final Object owner;
        private final Field listenerField;
        private final Method setListener;
        private final long epoch;
        private volatile boolean active;
        private Object proxy;

        Record(Object provider, Object original, String name, Object owner, Field listenerField, Method setListener, long epoch) {
            this.provider = provider;
            this.original = original;
            this.name = name;
            this.owner = owner;
            this.listenerField = listenerField;
            this.setListener = setListener;
            this.epoch = epoch;
        }

        @Override
        public Object invoke(Object proxyObject, Method method, Object[] args) throws Throwable {
            String methodName = method.getName();
            int argc = method.getParameterCount();
            // Object methods resolve by identity to avoid recursion into the proxy.
            if (argc == 1 && methodName.equals("equals") && method.getParameterTypes()[0] == Object.class)
                return proxyObject == args[0];
            if (argc == 0 && methodName.equals("hashCode"))
                return System.identityHashCode(proxyObject);
            if (argc == 0 && methodName.equals("toString"))
                return "GameLocationDeliveryProxy@" + Integer.toHexString(System.identityHashCode(proxyObject));
            // Satellite callbacks are never synthesized; status/location may be when eligible.
            if (methodName.equals("onProviderStatus") || methodName.equals("onProviderLocation")) {
                try {
                    if (active && isEnabled(owner) && deliver(owner, name, epoch, this)) return null;
                } catch (Throwable fault) {
                    // A reflection/delivery fault must not drop the original callback.
                    logFaultBounded("delivery",
                        fault instanceof InvocationTargetException ? unwrap((InvocationTargetException) fault) : fault);
                }
            }
            try {
                return method.invoke(original, args);
            } catch (InvocationTargetException error) {
                // Preserve the original callback's own failure cause.
                throw unwrap(error);
            }
        }
    }

    // --- Reflection helpers -----------------------------------------------------------------------

    private static Class<?> gameClass(String name) throws ClassNotFoundException {
        ClassLoader own = GameLocationDelivery.class.getClassLoader();
        try {
            return Class.forName(name, false, own);
        } catch (ClassNotFoundException missing) {
            ClassLoader context = Thread.currentThread().getContextClassLoader();
            if (context != null && context != own) return Class.forName(name, false, context);
            throw missing;
        }
    }

    private static Field findField(Class<?> type, String name) throws NoSuchFieldException {
        for (Class<?> current = type; current != null; current = current.getSuperclass()) {
            try {
                Field field = current.getDeclaredField(name);
                field.setAccessible(true);
                return field;
            } catch (NoSuchFieldException ignored) {
                // Walk the superclass chain.
            }
        }
        throw new NoSuchFieldException(type.getName() + "." + name);
    }

    private static Method findMethod(Class<?> type, String name, Class<?>... params) throws NoSuchMethodException {
        for (Class<?> current = type; current != null; current = current.getSuperclass()) {
            try {
                Method method = current.getDeclaredMethod(name, params);
                method.setAccessible(true);
                return method;
            } catch (NoSuchMethodException ignored) {
                // Walk the superclass chain.
            }
        }
        throw new NoSuchMethodException(type.getName() + "." + name);
    }

    private Method findSetListener(Class<?> type) throws NoSuchMethodException {
        return findMethod(type, "setListener", listenerInterface);
    }

    private static Object getFieldValue(Field field, Object target) throws IllegalAccessException {
        return field.get(target);
    }

    private static boolean nativePointerIsZero(Object value) {
        if (value == null) return true;
        if (value instanceof Number) return ((Number) value).longValue() == 0L;
        return "0".equals(String.valueOf(value));
    }

    private static Throwable unwrap(InvocationTargetException error) {
        Throwable cause = error.getCause();
        return cause != null ? cause : error;
    }
}
