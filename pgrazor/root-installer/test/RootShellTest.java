package dev.pgrazor.root;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.TimeUnit;

/**
 * Drives {@link RootShell#await} on a plain JVM with fake {@link Process}es — no {@code su}, no
 * Android, no device. Proves the timeout fix: when the command does not exit and its output stream
 * stays open (a descendant still holding the inherited pipe after {@code su} is killed), {@code await}
 * still returns within a bounded time instead of blocking on the read forever.
 */
public final class RootShellTest {
    public static void main(String[] args) throws Exception {
        successfulCompletionCapturesOutput();
        outputIsCappedAndMarkedTruncated();
        timeoutReturnsBoundedWithStuckReader();
        exitedButOpenPipeReportsUncertain();
        blockedDestroyStillReturns();
        System.out.println("PASS root shell: completion, output cap, bounded timeout with stuck reader, uncertain output");
    }

    /** A command that exits cleanly with output is reported started, not-timed-out, with the output. */
    private static void successfulCompletionCapturesOutput() {
        InputStream out = new ByteArrayInputStream("hello\n".getBytes(StandardCharsets.UTF_8));
        RootShell.Result result = RootShell.await(new ExitedProcess(out, 0), 1000);
        check(result.started, "started");
        check(!result.timedOut, "not timed out on clean exit");
        check(result.code == 0, "exit code 0");
        check(result.ok(), "ok() true on clean success");
        check("hello".equals(result.output), "output trimmed to 'hello' (was: " + result.output + ")");
    }

    /** Output above the cap is kept to MAX_OUTPUT bytes and flagged, never grown without bound. */
    private static void outputIsCappedAndMarkedTruncated() {
        byte[] big = new byte[RootShell.MAX_OUTPUT + 100];
        java.util.Arrays.fill(big, (byte) 'a');   // non-whitespace so trim() cannot shrink the cap
        RootShell.Result result = RootShell.await(new ExitedProcess(new ByteArrayInputStream(big), 0), 1000);
        check(!result.timedOut, "clean exit even when chatty");
        String marker = "\n[output truncated]";
        check(result.output.endsWith("[output truncated]"), "truncation marker present");
        check(result.output.length() == RootShell.MAX_OUTPUT + marker.length(),
            "captured exactly MAX_OUTPUT bytes plus marker (len=" + result.output.length() + ")");
    }

    /**
     * The core regression: su never exits and its stdout never closes (a descendant keeps the pipe
     * open after destroyForcibly). {@code await} must still return promptly — bounded by the timeout
     * plus the fixed reader-drain window — and must report the kill honestly without claiming the
     * descendant stopped.
     */
    private static void timeoutReturnsBoundedWithStuckReader() {
        StuckProcess process = new StuckProcess(new BlockingInputStream());
        long timeoutMs = 120;
        long start = System.nanoTime();
        RootShell.Result result = RootShell.await(process, timeoutMs);
        long elapsedMs = (System.nanoTime() - start) / 1_000_000L;

        long bound = timeoutMs + RootShell.READER_JOIN_MS + 1500; // generous slack for CI scheduling
        check(elapsedMs < bound, "await returned within " + bound + " ms (took " + elapsedMs + " ms)");
        check(result.started, "started");
        check(result.timedOut, "timed out / uncertain");
        check(!result.ok(), "ok() false on timeout");
        check(result.code == -1, "no clean exit code");
        check(process.destroyed, "su was destroyed");
        check(result.output.contains("timed out"), "output notes the timeout");
        check(result.output.contains("may still be running"),
            "output warns the root manager command may still be running");
    }

    /** su exited cleanly but a descendant kept the pipe open: output is honestly marked uncertain. */
    private static void exitedButOpenPipeReportsUncertain() {
        long start = System.nanoTime();
        RootShell.Result result = RootShell.await(new ExitedProcess(new BlockingInputStream(), 0), 1000);
        long elapsedMs = (System.nanoTime() - start) / 1_000_000L;
        check(elapsedMs < RootShell.READER_JOIN_MS + 1500, "returned within the drain window (took " + elapsedMs + " ms)");
        check(result.started, "started");
        check(result.timedOut, "uncertain because the reader is still alive");
        check(!result.ok(), "ok() false when output is uncertain");
        check(result.output.contains("output stream stayed open"), "output notes the open stream");
    }

    private static void check(boolean condition, String reason) {
        if (!condition) throw new AssertionError(reason);
    }

    private static void blockedDestroyStillReturns() {
        StuckProcess process = new StuckProcess(new BlockingInputStream()) {
            @Override public Process destroyForcibly() {
                try { new BlockingInputStream().read(); } catch (IOException ignored) {}
                return this;
            }
        };
        long start = System.nanoTime();
        RootShell.Result result = RootShell.await(process, 50);
        check((System.nanoTime() - start) / 1_000_000L < 2000, "blocked destroy obeys caller deadline");
        check(result.timedOut && !result.ok(), "blocked destroy reports uncertain");
    }

    // --- Fakes -----------------------------------------------------------------------------------

    /** Minimal {@link Process} whose only real behavior is the stdout stream the collector drains. */
    private abstract static class FakeProcess extends Process {
        private final InputStream out;
        FakeProcess(InputStream out) { this.out = out; }
        @Override public OutputStream getOutputStream() { return new ByteArrayOutputStream(); }
        @Override public InputStream getInputStream() { return out; }
        @Override public InputStream getErrorStream() { return new ByteArrayInputStream(new byte[0]); }
        @Override public int waitFor() { return exitValue(); } // not used by await; kept valid.
    }

    /** Exits immediately with a fixed code; its stream drains to EOF on its own. */
    private static final class ExitedProcess extends FakeProcess {
        private final int code;
        ExitedProcess(InputStream out, int code) { super(out); this.code = code; }
        @Override public boolean waitFor(long t, TimeUnit u) { return true; }
        @Override public int exitValue() { return code; }
        @Override public void destroy() {}
        @Override public Process destroyForcibly() { return this; }
        @Override public boolean isAlive() { return false; }
    }

    /**
     * Never exits; honors the requested wait by sleeping for it (so await's timing is realistic),
     * and {@code destroyForcibly} does NOT close the stream — modelling descendants that keep the
     * inherited pipe open after su is killed.
     */
    private static class StuckProcess extends FakeProcess {
        volatile boolean destroyed;
        StuckProcess(InputStream out) { super(out); }
        @Override public boolean waitFor(long t, TimeUnit u) throws InterruptedException {
            Thread.sleep(u.toMillis(t));
            return false;
        }
        @Override public int exitValue() { throw new IllegalThreadStateException(); }
        @Override public void destroy() { destroyed = true; }
        @Override public Process destroyForcibly() { destroyed = true; return this; }
        @Override public boolean isAlive() { return true; }
    }

    /** A stream whose read blocks until closed — a descendant holding the pipe's write end open. */
    private static final class BlockingInputStream extends InputStream {
        private final Object lock = new Object();
        private boolean closed;
        @Override public int read() throws IOException {
            byte[] one = new byte[1];
            int n = read(one, 0, 1);
            return n == -1 ? -1 : (one[0] & 0xFF);
        }
        @Override public int read(byte[] b, int off, int len) throws IOException {
            synchronized (lock) {
                while (!closed) {
                    // Ignore interrupts the way a blocking native pipe read would, so only closing
                    // the stream ends the wait — exactly what await must survive without blocking.
                    try { lock.wait(); } catch (InterruptedException ignored) {}
                }
            }
            return -1;
        }
        @Override public void close() {
            synchronized (lock) { closed = true; lock.notifyAll(); }
        }
    }
}
