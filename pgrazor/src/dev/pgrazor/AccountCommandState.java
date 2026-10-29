package dev.pgrazor;

/** Process-lifetime command identity; independent of the attachable hook runtime. */
final class AccountCommandState {
    private static final class Entry {
        final String command;
        final long token;
        Entry(String command, long token) { this.command = command; this.token = token; }
    }
    private volatile Entry entry = new Entry("", 0);
    private long sequence;

    String current() { return entry.command; }

    String[] snapshot() {
        Entry value = entry;
        return new String[]{value.command, Long.toString(value.token)};
    }

    synchronized void issue(String command) {
        if (command == null || command.isEmpty()) throw new IllegalArgumentException("Empty command");
        if (command.equals(entry.command)) return;
        if (sequence == Long.MAX_VALUE) throw new IllegalStateException("Command token exhausted");
        entry = new Entry(command, ++sequence);
    }

    synchronized void clear() { entry = new Entry("", 0); }
}
