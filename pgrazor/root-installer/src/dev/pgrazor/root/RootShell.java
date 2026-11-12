package dev.pgrazor.root;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.TimeUnit;

/**
 * Runs a single root command ({@code su -c <command>}) with a bounded lifetime and a capped output
 * buffer. Must never be called on the main thread: it blocks for up to {@code timeoutMs} (plus a
 * short, fixed reader-drain window) before returning. The command string is handed to {@code su} as
 * one argument, so {@code su} runs it with its own {@code sh -c}; this class spawns no shell of its
 * own.
 *
 * <p>Output is drained on a separate daemon thread so a stuck read can never pin the caller. When
 * {@code su} is killed by the watchdog its descendants (e.g. the root manager's module-install
 * command) may keep running and may keep the inherited output pipe open; the caller must not block
 * on that. {@link #await} therefore waits only a bounded time for the reader after terminating
 * {@code su}, and reports the outcome as {@link Result#timedOut uncertain} rather than claiming the
 * descendants stopped. This class holds no Android dependency, so {@link #await} can be exercised
 * on a plain JVM with a fake {@link Process}.
 */
final class RootShell {
    /** Hard cap on captured output so a chatty command cannot exhaust memory. */
    static final int MAX_OUTPUT = 1 << 20;

    /**
     * How long {@link #await} waits for the reader to finish after {@code su} exits or is destroyed.
     * Short on purpose: an inherited, still-open pipe must not be allowed to block the caller, so we
     * drain for this long and then abandon the (daemon) reader and report the result as uncertain.
     */
    static final long READER_JOIN_MS = 500L;

    private RootShell() {}

    static final class Result {
        final boolean started;   // false when su could not be launched at all
        final boolean timedOut;  // true when the watchdog destroyed su, or the output is uncertain
        final int code;          // exit code, or -1 when it never exited cleanly
        final String output;     // combined stdout+stderr

        Result(boolean started, boolean timedOut, int code, String output) {
            this.started = started;
            this.timedOut = timedOut;
            this.code = code;
            this.output = output;
        }

        boolean ok() { return started && !timedOut && code == 0; }
    }

    /** Runs {@code su -c command}; destroys it after {@code timeoutMs} and reports what happened. */
    static Result run(String command, long timeoutMs) {
        Process process;
        try {
            process = new ProcessBuilder("su", "-c", command).redirectErrorStream(true).start();
        } catch (IOException noStart) {
            // An IOException here means su never launched: root access was denied/withdrawn by the
            // root manager, or su is not present. We cannot tell which, so we do not claim either.
            return new Result(false, false, -1,
                "Could not start a root shell: root access was denied or unavailable (" + noStart.getMessage() + ").");
        }
        return await(process, timeoutMs);
    }

    /**
     * Waits up to {@code timeoutMs} for {@code process} to exit while a daemon thread drains its
     * output, then returns a {@link Result}. Package-private so it can be driven on a plain JVM with
     * a fake {@link Process} (no {@code su}, no device). Never holds a monitor across a blocking read,
     * never blocks indefinitely on the reader, and never closes the process stream from this thread
     * (the reader owns and closes it), so a descendant that keeps the pipe open cannot pin the caller.
     */
    static Result await(Process process, long timeoutMs) {
        OutputCollector collector = new OutputCollector(process.getInputStream());
        Thread reader = new Thread(collector, "pgrazor-root-shell-reader");
        reader.setDaemon(true);
        reader.start();

        boolean exited;
        int code = -1;
        try {
            exited = process.waitFor(timeoutMs, TimeUnit.MILLISECONDS);
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
            exited = false;
        }
        if (exited) {
            code = process.exitValue();
        } else {
            // Watchdog: terminate su. A root-manager command it started may keep running and may
            // hold the output pipe open; we never assert its descendants stopped.
            // Some Process implementations close streams during destroy and can themselves block
            // behind an inherited pipe. Termination must not defeat the caller's deadline either.
            Thread killer = new Thread(() -> process.destroyForcibly(), "pgrazor-root-shell-killer");
            killer.setDaemon(true);
            killer.start();
        }

        // Bounded drain: give the reader a short, fixed window to finish, then move on. We do not
        // join forever and do not close the stream here, so an inherited, still-open pipe cannot
        // block this thread. The reader is a daemon and closes its own stream, so abandoning it is
        // safe.
        try {
            reader.join(READER_JOIN_MS);
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
        }
        boolean readerAlive = reader.isAlive();

        String text = collector.snapshot();
        if (!exited) {
            text = withNote(text, "[timed out after " + timeoutMs
                + " ms; su termination requested, but a root manager command it started may still be running]");
        } else if (readerAlive) {
            text = withNote(text,
                "[command exited but its output stream stayed open; captured output may be incomplete]");
        }
        // timedOut doubles as the "uncertain" signal: either we had to kill su, or output is still
        // flowing from something we could not wait on.
        boolean timedOut = !exited || readerAlive;
        return new Result(true, timedOut, exited ? code : -1, text);
    }

    private static String withNote(String text, String note) {
        return text.isEmpty() ? note : text + "\n" + note;
    }

    /**
     * Drains an input stream into a capped buffer. The blocking {@code read} is done with no monitor
     * held; only the short append to the shared buffer is synchronized, so {@link #snapshot} can read
     * a consistent view at any time even while a stuck reader is still alive.
     */
    private static final class OutputCollector implements Runnable {
        private final InputStream in;
        private final ByteArrayOutputStream buffer = new ByteArrayOutputStream();
        private boolean truncated;

        OutputCollector(InputStream in) { this.in = in; }

        @Override public void run() {
            byte[] chunk = new byte[8192];
            try {
                int n;
                while ((n = in.read(chunk)) != -1) {   // blocking read, no monitor held
                    append(chunk, n);
                }
            } catch (IOException closed) {
                // destroyForcibly / the stream closing lands here; keep whatever was captured.
            } finally {
                try { in.close(); } catch (IOException ignored) { /* best effort */ }
            }
        }

        private synchronized void append(byte[] chunk, int n) {
            int room = MAX_OUTPUT - buffer.size();
            if (room <= 0) { truncated = true; return; }
            if (n > room) { buffer.write(chunk, 0, room); truncated = true; }
            else { buffer.write(chunk, 0, n); }
        }

        synchronized String snapshot() {
            String text = new String(buffer.toByteArray(), StandardCharsets.UTF_8).trim();
            if (truncated) text = withNote(text, "[output truncated]");
            return text;
        }
    }
}
