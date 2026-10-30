package dev.pgrazor;

import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.CharBuffer;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;

/**
 * Compiled host owner for the native runtime transport (feature 52). This is the typed, binary replacement
 * for the JSON mailbox {@link RuntimeMailbox}/translated.js host side: it reads the live Control getters into
 * a bounded, versioned binary snapshot and exchanges it with the native transport over a JNI DirectByteBuffer
 * in both directions, then drains and dispatches typed action events back into Control.
 *
 * <p>No JSON, no JS interpreter and no file are involved. Construction is inert: it resolves every Control
 * signature once and allocates nothing native, with no timer, no Control invocation and no exchange. Call
 * {@link #start()} once to begin a single daemon ticking every 50 ms. The native method is bound by the
 * library with {@code RegisterNatives} from {@code JNI_OnLoad}; this class only declares it, so there is no
 * name-mangled JNI export. Tests inject a pure-Java {@link Exchange} through {@link #createForTest} and never
 * touch the native library.
 *
 * <p>This replaces the HOST transport only. The guest ARM gameplay producers, Control's own
 * feature/lease/domain authorization, and the Frida runtime are not retired here.
 */
public final class NativeRuntimeTransport {
    private static final int MAX_BYTES = 1048576, MAX_CALLS = 128;

    // Wire constants, compiled identically in runtime_transport.h.
    private static final int MAGIC_SNAPSHOT = 0x53524750, MAGIC_EVENTS = 0x45524750, FRAME_VERSION = 5, FLAG_INERT = 1;
    private static final int OFF_MAGIC = 0, OFF_VERSION = 4, OFF_FRAME_SIZE = 8, OFF_FLAGS = 12, OFF_SEQ = 16,
        OFF_WALL = 24, OFF_BOOLS = 32, OFF_INTS = 80, OFF_DOUBLES = 120, OFF_PULSES = 136, OFF_STRINGS = 160;
    private static final int EVENT_HEADER = 32, ACTION_COUNT = 59;

    // Legacy translated.js reader order, split by compiled type group. Indices here equal the native field ids.
    private static final String[] BOOL_READERS = {
        "accountManagementAllowed", "adventureSyncDismissAllowed", "gameRoutesAllowed", "active", "autoIncubate",
        "autoTransferAllowed", "battlesAllowed", "berriesAllowed", "buddyAllowed", "buddySamplingRequested",
        "cameraSteeringRequested", "desiredEnabled", "giftsAllowed", "incubateLongest", "incubatorAllowed",
        "interactionDiagnosticsReady", "itemAccessAllowed", "itemTransferEnabled", "ivCatchEnabled",
        "ivDisplayAllowed", "ivOwnedEnabled", "mapPoiAllowed", "mapTapAllowed", "quickBallAllowed",
        "quickSpinAllowed", "quickSummaryAllowed", "rocketAllowed", "spawnAllowed", "spawnBooster",
        "throwAllowed", "throwAlwaysHit", "throwAlwaysHitReady", "throwAr", "throwArReady", "throwCurve",
        "transferKeepAll", "vgpAllowed", "vgpEnabled", "vgpMapVisible", "eventBoxesAllowed"
    };
    private static final String[] INT_READERS = {
        "battleOptions", "buddyBerry", "giftOptions", "itemCatalogMode", "mapTapEpoch", "mapTapMode",
        "rocketOptions", "throwGrade", "transferProtect", "vgpOptions"
    };
    private static final String[] DOUBLE_READERS = {"latitude", "longitude"};
    private static final String[] STRING_READERS = {
        "accountCommand", "gameRoutesCommand", "effectiveBerryPriority", "itemRules", "transferKeepSpecies"
    };
    private static final String[] PULSE_READERS = {
        "takeCameraSampleRequest", "takeInteractionStopRequest", "takeInteractionTraceRequest"
    };
    // Action opcode order (== native kActions). name:comma-separated argument types:return type.
    private static final String[] ACTIONS = {
        "accountIdentity:String,String:void", "setAccountsReady:boolean:void", "accountSessionEnded::void",
        "accountLogoutComplete::void", "accountLogoutFailed::void", "accountSignInResetComplete::void",
        "accountSessionSaved::void", "setGameRoutesReady:boolean,String:void", "setGameRoutesSnapshot:String:void",
        "gameRoutesAction:String:void", "applyMapTap:double,double,int,int,double:void", "cameraDiagnostic:String:void",
        "configureCameraDiagnostics:boolean:void", "configureCameraSteering:boolean:void",
        "configureInteractionDiagnostics:boolean:void", "configureQuickBall:boolean:void",
        "configureQuickSummary:boolean:void", "cooldownAction:String:void", "embedded:String:void",
        "event:String,String:void", "interactionDiagnostic:String:void", "invalidateCameraHeading:String:void",
        "quickBallOutcome:String:boolean", "setAutoTransferReady:boolean,String:void", "setBattleStatus:String:void",
        "setBattlesReady:boolean,String:void", "setBerryReady:boolean:void", "setBuddyReady:boolean,String:void",
        "setBuddyStatus:String:void", "setCameraDiagnosticRunning:boolean:void", "setCooldownEncounter:String:void",
        "setEncounterBerry:boolean,boolean:void", "setGiftStatus:String:void", "setGiftsReady:boolean,String:void",
        "setIncubatorReady:boolean,String:void", "setIncubatorStatus:String:void",
        "setInteractionDiagnosticRunning:boolean:void", "setItemCatalog:String:void", "setItemReady:boolean,String:void",
        "setItemStatus:String:void", "setIvOwnedReady:boolean:void", "setMapPoiSnapshot:String:void",
        "setMapTapReady:boolean:void", "setQuickBallReady:boolean,String:void", "setQuickSpinReady:boolean,String:void",
        "setQuickSummaryReady:boolean,String:void", "setRocketReady:boolean,String:void", "setRocketStatus:String:void",
        "setSpawnRange:double,double:void", "setSpawnReady:boolean:void", "setThrowArReady:boolean:void",
        "setThrowAlwaysHitReady:boolean:void", "setThrowReady:boolean:void", "setVgpReady:boolean,String:void",
        "setVgpStatus:String:void", "setVgpTarget:boolean,double,double,boolean:void", "transferResult:String:void",
        "updateCameraHeadingAt:double,int,boolean,long:void", "vgpResult:String,boolean:void"
    };

    /** The native exchange seam. Production wraps the RegisterNatives method; tests inject a pure-Java fake. */
    interface Exchange {
        /** Push the snapshot in {@code snapshot[0,length)} and drain pending events into {@code events} from
         * position 0. Returns the number of event bytes written ({@code >= 0}) or a negative transport status. */
        int exchange(ByteBuffer snapshot, int length, ByteBuffer events) throws Exception;
        void stop();
    }

    private final Exchange exchange;
    private final Method[] boolReaders, intReaders, doubleReaders, stringReaders, pulseReaders, actions;
    private final Class<?>[][] actionParams;
    private final Method snapshotReader, smartCatchReader, eventAction;
    private final long[] pulseCounts = new long[PULSE_READERS.length];
    private ByteBuffer snapshotBuffer, eventBuffer;
    private long seq;
    // Event-frame replay/restart guards. The native drain stamps every frame with the session generation and
    // the base seq of its first event. We latch the generation once and reject any later change (a native
    // restart without revival), and we require each non-empty batch to carry the next contiguous seq so a
    // duplicate/stale frame can never replay an already-dispatched gameplay action.
    private long lastGeneration;
    private boolean generationLatched;
    private long lastEventSeq;
    private boolean stopped, ticking, started, ownsLifecycle;
    private ScheduledExecutorService executor;
    private ScheduledFuture<?> scheduled;

    private NativeRuntimeTransport(Exchange exchange) throws Exception {
        if (exchange == null) throw invalid();
        this.exchange = exchange;
        boolReaders = resolveReaders(BOOL_READERS, boolean.class);
        intReaders = resolveReaders(INT_READERS, int.class);
        doubleReaders = resolveReaders(DOUBLE_READERS, double.class);
        stringReaders = resolveReaders(STRING_READERS, String.class);
        pulseReaders = resolveReaders(PULSE_READERS, boolean.class);
        snapshotReader = reader("accountCommandSnapshot", String[].class);
        smartCatchReader = reader("smartCatchConfig", String.class);
        actions = new Method[ACTIONS.length];
        actionParams = new Class<?>[ACTIONS.length][];
        if (ACTIONS.length != ACTION_COUNT) throw invalid();
        for (int i = 0; i < ACTIONS.length; i++) {
            String[] fields = ACTIONS[i].split(":", -1);
            String[] names = fields[1].isEmpty() ? new String[0] : fields[1].split(",");
            Class<?>[] signature = new Class<?>[names.length];
            for (int j = 0; j < names.length; j++) signature[j] = type(names[j]);
            Method method = Control.class.getMethod(fields[0], signature);
            if (!Modifier.isStatic(method.getModifiers()) || method.getReturnType() != type(fields[2])) throw invalid();
            actions[i] = method;
            actionParams[i] = signature;
        }
        eventAction = actions[19];  // event(String,String): the only self-report channel on fault
    }

    /** Resolve the production transport. Inert: resolves signatures only; the native handle, timer, buffers
     * and first exchange are all deferred to {@link #start()}. */
    public static NativeRuntimeTransport create(int leaseMs, int maxEvents, int maxStringBytes, int queueBudget) {
        try {
            return new NativeRuntimeTransport(new NativeExchange(leaseMs, maxEvents, maxStringBytes, queueBudget));
        } catch (Exception error) {
            throw new IllegalArgumentException("Runtime transport configuration invalid");
        }
    }

    /** Test-only factory with an injected pure-Java exchange seam (no native library). */
    static NativeRuntimeTransport createForTest(Exchange exchange) throws Exception {
        return new NativeRuntimeTransport(exchange);
    }

    private static Class<?> type(String name) throws Exception {
        switch (name) {
            case "boolean": return boolean.class;
            case "int": return int.class;
            case "long": return long.class;
            case "double": return double.class;
            case "String": return String.class;
            case "String[]": return String[].class;
            case "void": return void.class;
            default: throw invalid();
        }
    }
    private static Method reader(String name, Class<?> expected) throws Exception {
        Method method = Control.class.getMethod(name);
        if (!Modifier.isStatic(method.getModifiers()) || method.getReturnType() != expected) throw invalid();
        return method;
    }
    private static Method[] resolveReaders(String[] names, Class<?> expected) throws Exception {
        Method[] out = new Method[names.length];
        for (int i = 0; i < names.length; i++) out[i] = reader(names[i], expected);
        return out;
    }
    private static Exception invalid() { return new IllegalStateException("Runtime transport protocol invalid"); }

    // start is idempotent; construction remains inert. Java owns the sole 50 ms cadence.
    public synchronized void start() {
        if (stopped || started) return;
        started = true;
        try {
            ensureBuffers();
            executor = Executors.newSingleThreadScheduledExecutor(task -> {
                Thread thread = new Thread(task, "PGRazor-NativeRuntimeTransport");
                thread.setDaemon(true);
                return thread;
            });
            scheduled = executor.scheduleWithFixedDelay(this::tick, 0, 50, TimeUnit.MILLISECONDS);
        } catch (Exception error) { fail(); }
    }

    /** Called only after compiled Location and DEX bridges are ready. */
    public synchronized void ownLifecycle() {
        if (stopped || ownsLifecycle) return;
        ownsLifecycle = true;
        try { Control.connect(); }
        catch (Throwable failure) { fail(); }
    }

    private void cancelTimer() {
        if (scheduled != null) { scheduled.cancel(false); scheduled = null; }
        if (executor != null) { executor.shutdown(); executor = null; }
    }

    // Direct buffers are allocated on first use, not in the inert factory. The public tick() stays usable by
    // fixtures without start().
    private void ensureBuffers() {
        if (snapshotBuffer == null) snapshotBuffer = ByteBuffer.allocateDirect(MAX_BYTES).order(ByteOrder.LITTLE_ENDIAN);
        if (eventBuffer == null) eventBuffer = ByteBuffer.allocateDirect(MAX_BYTES).order(ByteOrder.LITTLE_ENDIAN);
    }

    public synchronized void tick() {
        if (stopped || ticking) return;
        ticking = true;
        try {
            if (ownsLifecycle) Control.renewLease();
            ensureBuffers();
            buildSnapshot();
            if (stopped) return;
            int written = exchange.exchange(snapshotBuffer, snapshotBuffer.limit(), eventBuffer);
            if (written < 0) throw invalid();  // negative native status faults the host transport
            Call[] calls = decode(written);
            // Native drain already consumed these events; validate the whole batch, then dispatch. A setter
            // exception or host reentry abandons the remaining calls and can never replay a gameplay action.
            for (Call call : calls) { if (stopped) return; call.method.invoke(null, call.args); }
        } catch (Throwable error) {
            // Throwable, not Exception: a LinkageError/UnsatisfiedLinkError from the native binding (or any
            // other Error) would otherwise escape the scheduled task, silently cancelling the daemon WITHOUT
            // stopping/revoking the transport. Fault closed on anything so the host always lands in a clean
            // stopped state with the native side revoked.
            if (!stopped) fail();
        } finally { ticking = false; }
    }

    /** Test/diagnostic accessor: true once stopped by {@link #stop()} or a transport fault. */
    synchronized boolean isStopped() { return stopped; }

    public synchronized void stop() {
        if (stopped) return;
        stopped = true;
        cancelTimer();
        try { exchange.stop(); } catch (Throwable ignored) {}
        disconnectLifecycle();
    }

    private void fail() {
        stopped = true;
        cancelTimer();
        try { exchange.stop(); } catch (Throwable ignored) {}
        disconnectLifecycle();
        // Constant diagnostic; never emits account content or exception text.
        try { eventAction.invoke(null, "native-transport-error", "Runtime transport failed"); } catch (Exception ignored) {}
    }

    private void disconnectLifecycle() {
        if (!ownsLifecycle) return;
        ownsLifecycle = false;
        try { Control.disconnect(); } catch (Throwable ignored) {}
    }

    // --- snapshot (host -> gameplay) -----------------------------------------------
    private void buildSnapshot() throws Exception {
        ByteBuffer buffer = snapshotBuffer;
        buffer.clear();
        for (int i = 0; i < boolReaders.length; i++)
            buffer.put(OFF_BOOLS + i, (byte) (((Boolean) boolReaders[i].invoke(null)) ? 1 : 0));
        for (int i = 0; i < intReaders.length; i++)
            buffer.putInt(OFF_INTS + i * 4, (Integer) intReaders[i].invoke(null));
        for (int i = 0; i < doubleReaders.length; i++) {
            double value = (Double) doubleReaders[i].invoke(null);
            // Legacy JSON.stringify(NaN) -> null; nonfinite coords ride across as the NaN sentinel.
            buffer.putDouble(OFF_DOUBLES + i * 8, Double.isFinite(value) ? value : Double.NaN);
        }
        for (int i = 0; i < pulseReaders.length; i++) {
            if ((Boolean) pulseReaders[i].invoke(null)) {
                if (pulseCounts[i] == Long.MAX_VALUE) throw invalid();  // exact 64-bit; guard only true overflow
                pulseCounts[i]++;
            }
            buffer.putLong(OFF_PULSES + i * 8, pulseCounts[i]);
        }
        buffer.position(OFF_STRINGS);
        for (Method method : stringReaders) putString(buffer, method.invoke(null));
        Object pair = snapshotReader.invoke(null);
        if (!(pair instanceof String[]) || ((String[]) pair).length != 2) throw invalid();
        for (String part : (String[]) pair) { if (part == null) throw invalid(); putString(buffer, part); }
        putString(buffer, smartCatchReader.invoke(null)); // slot 7; existing account indices stay stable
        int frameSize = buffer.position();
        buffer.putInt(OFF_MAGIC, MAGIC_SNAPSHOT);
        buffer.putInt(OFF_VERSION, FRAME_VERSION);
        buffer.putInt(OFF_FRAME_SIZE, frameSize);
        buffer.putInt(OFF_FLAGS, 0);
        buffer.putLong(OFF_SEQ, ++seq);
        buffer.putLong(OFF_WALL, System.currentTimeMillis());
        buffer.position(0).limit(frameSize);
    }
    private static void putString(ByteBuffer buffer, Object value) throws Exception {
        if (!(value instanceof String)) throw invalid();  // no generic Object.toString fallback
        // Strict encode: String.getBytes(UTF_8) silently substitutes U+FFFD for unpaired surrogates / malformed
        // UTF-16, smuggling corrupt bytes across the typed boundary. REPORT instead so a malformed Control
        // string faults the host closed rather than shipping silently mangled data the native strict decoder
        // would then mis-handle.
        byte[] bytes = encodeStrict((String) value);
        if (bytes.length > MAX_BYTES || buffer.position() + 4 + bytes.length > buffer.capacity()) throw invalid();
        buffer.putInt(bytes.length);
        buffer.put(bytes);
    }
    private static byte[] encodeStrict(String value) throws Exception {
        ByteBuffer encoded = StandardCharsets.UTF_8.newEncoder()
            .onMalformedInput(CodingErrorAction.REPORT)
            .onUnmappableCharacter(CodingErrorAction.REPORT)
            .encode(CharBuffer.wrap(value));
        byte[] bytes = new byte[encoded.remaining()];
        encoded.get(bytes);
        return bytes;
    }

    // --- events (gameplay -> host) -------------------------------------------------
    private static final class Call {
        final Method method; final Object[] args;
        Call(Method m, Object[] a) { method = m; args = a; }
    }
    private Call[] decode(int length) throws Exception {
        ByteBuffer buffer = eventBuffer;
        buffer.position(0).limit(length);
        if (length < EVENT_HEADER || buffer.getInt(OFF_MAGIC) != MAGIC_EVENTS ||
            buffer.getInt(OFF_VERSION) != FRAME_VERSION || buffer.getInt(OFF_FRAME_SIZE) != length) throw invalid();
        int count = buffer.getInt(12);
        if (count < 0 || count > MAX_CALLS) throw invalid();
        long baseSeq = buffer.getLong(16), generation = buffer.getLong(24);
        // Reject a native session restart (generation change) without revival: fault closed, never resume.
        if (generationLatched) { if (generation != lastGeneration) throw invalid(); }
        else { lastGeneration = generation; generationLatched = true; }
        // Non-empty batches must carry the next contiguous seq; a non-advancing base seq is a stale/replayed
        // frame and is refused rather than re-dispatched. (Empty frames carry base seq 0 and are skipped.)
        if (count > 0) {
            if (baseSeq != lastEventSeq + 1) throw invalid();
            lastEventSeq = baseSeq + count - 1;
        }
        buffer.position(EVENT_HEADER);
        Call[] result = new Call[count];
        for (int i = 0; i < count; i++) {
            int opcode = buffer.getInt(), scalarCount = buffer.getInt(), stringCount = buffer.getInt();
            if (opcode < 0 || opcode >= actions.length) throw invalid();
            Class<?>[] types = actionParams[opcode];
            int expectScalars = 0, expectStrings = 0;
            for (Class<?> t : types) { if (t == String.class) expectStrings++; else expectScalars++; }
            if (scalarCount != expectScalars || stringCount != expectStrings) throw invalid();
            long[] scalars = new long[scalarCount];
            for (int j = 0; j < scalarCount; j++) scalars[j] = buffer.getLong();
            String[] strings = new String[stringCount];
            for (int j = 0; j < stringCount; j++) strings[j] = readString(buffer);
            Object[] args = new Object[types.length];
            int si = 0, sti = 0;
            for (int j = 0; j < types.length; j++)
                args[j] = types[j] == String.class ? strings[sti++] : scalar(types[j], scalars[si++]);
            domain(opcode, args);
            result[i] = new Call(actions[opcode], args);
        }
        if (buffer.position() != length) throw invalid();  // no trailing bytes
        return result;
    }
    private static String readString(ByteBuffer buffer) throws Exception {
        int len = buffer.getInt();
        if (len < 0 || len > MAX_BYTES || buffer.remaining() < len) throw invalid();
        byte[] bytes = new byte[len];
        buffer.get(bytes);
        return StandardCharsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
            .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(bytes)).toString();
    }
    private static Object scalar(Class<?> target, long bits) throws Exception {
        if (target == boolean.class) { if (bits != 0 && bits != 1) throw invalid(); return bits == 1; }
        if (target == int.class) { if (bits < Integer.MIN_VALUE || bits > Integer.MAX_VALUE) throw invalid(); return (int) bits; }
        // Longs carry the EXACT 64-bit value across the typed native wire -- no JS safe-integer clamp. Every
        // value Control accepts (e.g. a timestamp up to Long.MAX_VALUE) survives; the per-opcode domain check
        // (updateCameraHeadingAt timestamp >= 0) is the only constraint and runs in domain().
        if (target == long.class) return bits;
        if (target == double.class) {
            double value = Double.longBitsToDouble(bits);
            if (!Double.isFinite(value)) throw invalid();  // finite only; no legacy JS-safe magnitude clamp
            return value;
        }
        throw invalid();
    }
    private static void domain(int opcode, Object[] args) throws Exception {
        if (opcode == 10 /*applyMapTap*/ && (Math.abs((Double) args[0]) > 90 || Math.abs((Double) args[1]) > 180 ||
            (Integer) args[2] < 0 || (Integer) args[2] > 3 || (Double) args[4] < 0)) throw invalid();
        if (opcode == 57 /*updateCameraHeadingAt*/ && (Long) args[3] < 0) throw invalid();
    }

    // --- production native seam ----------------------------------------------------
    // The library binds nativeInstall/nativeExchange/nativeStop with RegisterNatives from JNI_OnLoad (no
    // name-mangled export). The native wrapper resolves the DirectByteBuffer addresses and forwards to the
    // feature-52 C table (query_feature(52,1)). This class never dlopen/loadLibrary; the bootstrap does.
    private static native long nativeInstall(int leaseMs, int maxEvents, int maxStringBytes, int queueBudget);
    private static native int nativeExchange(long handle, ByteBuffer snapshot, int length, ByteBuffer events, int capacity);
    private static native void nativeStop(long handle);

    private static final class NativeExchange implements Exchange {
        private final int leaseMs, maxEvents, maxStringBytes, queueBudget;
        private long handle;
        NativeExchange(int leaseMs, int maxEvents, int maxStringBytes, int queueBudget) {
            this.leaseMs = leaseMs; this.maxEvents = maxEvents;
            this.maxStringBytes = maxStringBytes; this.queueBudget = queueBudget;
        }
        public int exchange(ByteBuffer snapshot, int length, ByteBuffer events) {
            if (handle == 0) handle = nativeInstall(leaseMs, maxEvents, maxStringBytes, queueBudget);
            return nativeExchange(handle, snapshot, length, events, events.capacity());
        }
        public void stop() { if (handle != 0) nativeStop(handle); }
    }
}
