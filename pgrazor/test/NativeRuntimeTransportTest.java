package dev.pgrazor;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;

/**
 * Host-side fixture for {@link NativeRuntimeTransport}. It drives the real class through an injected pure-Java
 * {@link NativeRuntimeTransport.Exchange} seam (no native library), builds binary event frames exactly as the
 * native transport would, and verifies the typed snapshot the class emits. It never loads the native library.
 */
public final class NativeRuntimeTransportTest {
    // Wire constants mirror runtime_transport.h / NativeRuntimeTransport.
    private static final int MAGIC_SNAPSHOT = 0x53524750, MAGIC_EVENTS = 0x45524750, VERSION = 8;
    private static final int OFF_BOOLS = 32, OFF_INTS = 80, OFF_DOUBLES = 120, OFF_PULSES = 136, OFF_STRINGS = 160;
    private static final int EVENT_HEADER = 32;

    private static void require(boolean yes) { if (!yes) throw new AssertionError("transport fixture assertion"); }

    /** Captures the pushed snapshot and serves a single preloaded event frame (then empty), emulating consume. */
    private static final class Fake implements NativeRuntimeTransport.Exchange {
        byte[] lastSnapshot; byte[] nextEvents; int exchanges; boolean stopped; int ret;
        public int exchange(ByteBuffer snapshot, int length, ByteBuffer events) {
            exchanges++;
            lastSnapshot = new byte[length];
            for (int i = 0; i < length; i++) lastSnapshot[i] = snapshot.get(i);
            if (ret < 0) return ret;  // simulate a negative native status
            byte[] out = nextEvents != null ? nextEvents : emptyFrame();
            nextEvents = null;  // consume-before-dispatch: a drained batch is never served twice
            events.clear();  // reset any limit a prior decode left on the reused buffer
            events.put(out);
            return out.length;
        }
        public void stop() { stopped = true; }
    }

    private static byte[] emptyFrame() { return new FrameBuilder().build(); }

    /** Builds a native-style event frame (little-endian). */
    private static final class FrameBuilder {
        private final ByteBuffer body = ByteBuffer.allocate(1 << 20).order(ByteOrder.LITTLE_ENDIAN);
        private int count;
        private long baseSeq = 1, generation = 7;  // defaults match a single first batch of a fresh instance
        { body.position(EVENT_HEADER); }
        FrameBuilder baseSeq(long value) { baseSeq = value; return this; }
        FrameBuilder generation(long value) { generation = value; return this; }
        FrameBuilder event(int opcode, long[] scalars, String[] strings) {
            body.putInt(opcode).putInt(scalars.length).putInt(strings.length);
            for (long s : scalars) body.putLong(s);
            for (String s : strings) { byte[] b = s.getBytes(StandardCharsets.UTF_8); body.putInt(b.length).put(b); }
            count++;
            return this;
        }
        byte[] build() {
            int size = body.position();
            body.putInt(0, MAGIC_EVENTS).putInt(4, VERSION).putInt(8, size).putInt(12, count)
                .putLong(16, baseSeq).putLong(24, generation);
            byte[] out = new byte[size];
            body.position(0);
            body.get(out, 0, size);
            return out;
        }
        byte[] buildCorrupt(int magic) { byte[] f = build(); ByteBuffer.wrap(f).order(ByteOrder.LITTLE_ENDIAN).putInt(0, magic); return f; }
    }

    private static long bits(double d) { return Double.doubleToRawLongBits(d); }

    private static NativeRuntimeTransport fresh(Fake fake) throws Exception {
        Control.reset();
        return NativeRuntimeTransport.createForTest(fake);
    }

    public static void main(String[] args) throws Exception {
        // 1. Inert factory: no Control reads and no exchange before start()/tick().
        Fake fake = new Fake();
        NativeRuntimeTransport t = fresh(fake);
        require(Control.reads == 0 && Control.mutations == 0 && fake.exchanges == 0 && fake.lastSnapshot == null);

        // 2. First tick builds a well-formed typed snapshot from the Control getters.
        Control.pulse = true;
        t.tick();
        require(fake.exchanges == 1 && Control.reads > 0);
        ByteBuffer snap = ByteBuffer.wrap(fake.lastSnapshot).order(ByteOrder.LITTLE_ENDIAN);
        require(snap.getInt(0) == MAGIC_SNAPSHOT && snap.getInt(4) == VERSION && snap.getInt(8) == fake.lastSnapshot.length);
        require(snap.get(OFF_BOOLS + 3) == 0);         // active default false
        require(snap.getInt(OFF_INTS) == 0);           // battleOptions default 0
        require(Double.isNaN(snap.getDouble(OFF_DOUBLES)));         // latitude NaN -> null coord
        require(Double.isNaN(snap.getDouble(OFF_DOUBLES + 8)));     // longitude NaN
        require(snap.getLong(OFF_PULSES) == 1);        // one pulse edge counted this tick
        require(snap.getLong(OFF_PULSES + 8) == 0 && snap.getLong(OFF_PULSES + 16) == 0);
        // String heap: 5 scalar strings (all ""), then the account command/token pair.
        int at = OFF_STRINGS;
        for (int i = 0; i < 5; i++) { require(snap.getInt(at) == 0); at += 4; }
        require(readString(snap, at).equals("command")); at += 4 + "command".length();
        require(readString(snap, at).equals("18446744073709551615"));
        at += 4 + "18446744073709551615".length();
        require(readString(snap, at).isEmpty()); at += 4; // Smart Catcher retains slot 7.
        require(readString(snap, at).equals("1;1;0;0;50;10")); at += 4 + "1;1;0;0;50;10".length(); // Gym config is slot 8.
        require(readString(snap, at).isEmpty()); at += 4; // Buddy config slot 9.
        require(readString(snap, at).isEmpty()); at += 4; // Buddy history slot 10.
        require(readString(snap, at).isEmpty()); // Batch evolution slot 11.

        // 3. A valid event batch dispatches to Control exactly once (no replay on the next tick).
        fake.nextEvents = new FrameBuilder()
            .event(0 /*accountIdentity*/, new long[0], new String[]{"id", "name"})
            .event(6 /*accountSessionSaved*/, new long[0], new String[0]).build();
        t.tick();
        require(Control.mutations == 2);
        t.tick();
        require(Control.mutations == 2);  // empty frame now: nothing replays

        // 4. Exact 64-bit scalars + domain checks: updateCameraHeadingAt carries a large exact long.
        t = fresh(fake);
        fake.nextEvents = new FrameBuilder()
            .event(57 /*updateCameraHeadingAt: double,int,boolean,long*/,
                   new long[]{bits(20.0), 7, 1, 9007199254740991L}, new String[0]).build();
        t.tick();
        require(Control.timestamp == 9007199254740991L && !t.isStopped());

        // 5. Validation / domain / type rejections all fault closed (stop + constant diagnostic, no secrets).
        requireFault(57, new long[]{bits(20.0), 7, 1, -1}, new String[0]);                 // negative long arg
        requireFault(57, new long[]{bits(20.0), 7, 2, 0}, new String[0]);                   // boolean not 0/1
        requireFault(57, new long[]{bits(20.0), 1L << 40, 1, 0}, new String[0]);            // int out of range
        requireFault(57, new long[]{bits(Double.POSITIVE_INFINITY), 7, 1, 0}, new String[0]); // nonfinite double
        requireFault(10 /*applyMapTap*/, new long[]{bits(91), bits(0), 1, 0, bits(100)}, new String[0]); // lat > 90
        requireFault(57, new long[]{bits(20.0), 7, 1, 0, 0}, new String[0]);                // wrong scalar arity

        // 6. Malformed frames fault: bad magic, negative native status.
        t = fresh(fake);
        fake.nextEvents = new FrameBuilder().event(6, new long[0], new String[0]).buildCorrupt(0xDEADBEEF);
        t.tick();
        require(t.isStopped() && Control.mutations == 0 && Control.lastDiagnostic.equals("native-transport-error"));
        t = fresh(fake);
        fake.ret = -14;  // negative transport status
        t.tick();
        require(t.isStopped());
        fake.ret = 0;

        // 7. Reentry: a Control setter that re-enters tick() is guarded (no double dispatch, no corruption).
        t = fresh(fake);
        NativeRuntimeTransport reentry = t;
        Control.onMutation = () -> reentry.tick();
        fake.nextEvents = new FrameBuilder().event(0, new long[0], new String[]{"id", "name"}).build();
        t.tick();
        require(Control.mutations == 1 && !t.isStopped());

        // 8. Stop during dispatch abandons the remaining calls in the batch.
        t = fresh(fake);
        NativeRuntimeTransport stopper = t;
        Control.onMutation = () -> stopper.stop();
        fake.nextEvents = new FrameBuilder()
            .event(0, new long[0], new String[]{"id", "name"})
            .event(6, new long[0], new String[0]).build();
        t.tick();
        require(Control.mutations == 1 && fake.stopped);

        // 9. Account snapshot shape is enforced: a non-2 pair faults.
        t = fresh(fake);
        Control.snapshot = new String[]{"only-one"};
        t.tick();
        require(t.isStopped());

        // 10. Java owns the cadence: start() is idempotent and a stopped transport cannot restart.
        fake = new Fake();
        t = fresh(fake);
        t.start();
        t.start();
        long deadline = System.nanoTime() + 2_000_000_000L;
        while (fake.exchanges == 0 && System.nanoTime() < deadline) Thread.sleep(10);
        t.stop();
        require(fake.exchanges > 0 && fake.stopped);
        int after = fake.exchanges;
        t.start();
        Thread.sleep(80);
        require(fake.exchanges == after);  // no restart after stop

        // 11. A native session-generation change (restart WITHOUT revival) faults closed, never resumes.
        fake = new Fake();
        t = fresh(fake);
        t.tick();  // first frame (empty, generation 7) latches the generation
        require(!t.isStopped());
        fake.nextEvents = new FrameBuilder().generation(8).event(6, new long[0], new String[0]).build();
        t.tick();
        require(t.isStopped() && Control.mutations == 0 && Control.lastDiagnostic.equals("native-transport-error"));

        // 12. A stale/replayed event frame (base seq not advancing) is refused, never re-dispatched.
        fake = new Fake();
        t = fresh(fake);
        fake.nextEvents = new FrameBuilder().baseSeq(1).event(6, new long[0], new String[0]).build();
        t.tick();
        require(Control.mutations == 1 && !t.isStopped());  // seq 1 dispatched once
        fake.nextEvents = new FrameBuilder().baseSeq(1).event(6, new long[0], new String[0]).build();
        t.tick();
        require(t.isStopped() && Control.mutations == 1);  // replay of seq 1 refused, no second dispatch

        // 13. Exact 64-bit long: Long.MAX_VALUE survives the typed boundary (no JS safe-integer clamp).
        t = fresh(fake);
        fake.nextEvents = new FrameBuilder()
            .event(57 /*updateCameraHeadingAt: double,int,boolean,long*/,
                   new long[]{bits(20.0), 7, 1, Long.MAX_VALUE}, new String[0]).build();
        t.tick();
        require(Control.timestamp == Long.MAX_VALUE && !t.isStopped());

        // 14. Strict UTF-8 encoding: a Control string with an unpaired surrogate faults closed instead of
        // silently shipping U+FFFD-substituted bytes across the typed boundary.
        fake = new Fake();
        t = fresh(fake);
        Control.badBerry = "\uD800";  // lone high surrogate -> malformed UTF-16
        t.tick();
        require(t.isStopped() && Control.lastDiagnostic.equals("native-transport-error"));

        // Compiled lifecycle has one owner and revokes immediately on transport failure.
        fake = new Fake(); t = fresh(fake);
        Control.connects = Control.renewals = Control.disconnects = 0;
        t.ownLifecycle(); t.ownLifecycle();
        require(Control.connects == 1);
        t.tick(); require(Control.renewals == 1 && !t.isStopped());
        t.stop(); t.stop(); t.ownLifecycle(); t.tick();
        require(Control.disconnects == 1 && Control.connects == 1 && Control.renewals == 1);
        fake = new Fake(); t = fresh(fake); t.ownLifecycle();
        Control.badBerry = "\uD800";
        t.tick();
        require(t.isStopped() && Control.disconnects == 2);

        System.out.println("PASS NativeRuntimeTransport: inert factory, typed binary snapshot (bool/int/NaN-coord/"
            + "pulse/command-pair), valid batch dispatch + no replay, exact 64-bit scalars (incl. Long.MAX_VALUE), "
            + "type/domain/arity rejection, malformed-frame + negative-status fault, reentry guard, stop during "
            + "dispatch, snapshot shape, Java-owned start/stop cadence, event-frame generation-mismatch + "
            + "replay-seq fault, strict UTF-8 encode fault");
    }

    private static void requireFault(int opcode, long[] scalars, String[] strings) throws Exception {
        Fake fake = new Fake();
        NativeRuntimeTransport t = fresh(fake);
        fake.nextEvents = new FrameBuilder().event(opcode, scalars, strings).build();
        t.tick();
        require(t.isStopped() && Control.lastDiagnostic.equals("native-transport-error"));
        require(!Control.lastDiagnostic.contains("id-secret"));
    }

    private static String readString(ByteBuffer buffer, int offset) {
        int len = buffer.getInt(offset);
        byte[] bytes = new byte[len];
        for (int i = 0; i < len; i++) bytes[i] = buffer.get(offset + 4 + i);
        return new String(bytes, StandardCharsets.UTF_8);
    }
}
