package dev.pgrazor;

import java.lang.reflect.Field;
import java.util.concurrent.atomic.AtomicReference;

public final class AccountCommandStateTest {
    private static void require(boolean value) { if (!value) throw new AssertionError(); }
    public static void main(String[] args) throws Exception {
        AccountCommandState state = new AccountCommandState();
        state.issue("save-session:first");
        for (int i = 0; i < 100; i++) {
            // A new script attachment polls the same Java object; it cannot mint a new token.
            String[] snapshot = state.snapshot();
            require(snapshot[0].equals("save-session:first") && snapshot[1].equals("1"));
            snapshot[0] = "changed";
            state.issue("save-session:first");
        }
        state.clear();
        require(state.snapshot()[1].equals("0"));
        state.issue("reset-login:second");
        require(state.snapshot()[1].equals("2"));
        state.clear();
        state.issue("save-session:first");
        require(state.snapshot()[1].equals("3"));

        AccountCommandState concurrent = new AccountCommandState();
        AtomicReference<Throwable> failure = new AtomicReference<>();
        Thread writer = new Thread(() -> {
            try {
                for (int i = 1; i <= 10000; i++) { concurrent.issue("request:" + i); concurrent.clear(); }
            } catch (Throwable error) { failure.set(error); }
        });
        writer.start();
        do {
            String[] snapshot = concurrent.snapshot();
            if (snapshot[0].isEmpty()) require(snapshot[1].equals("0"));
            else require(snapshot[0].equals("request:" + snapshot[1]));
        } while (writer.isAlive());
        writer.join();
        require(failure.get() == null);

        Field counter = AccountCommandState.class.getDeclaredField("sequence");
        counter.setAccessible(true);
        counter.setLong(state, Long.MAX_VALUE);
        try { state.issue("reset-login:overflow"); throw new AssertionError("wrapped token"); }
        catch (IllegalStateException expected) { }
        require(state.current().equals("save-session:first") && state.snapshot()[1].equals("3"));
        System.out.println("PASS: AccountCommandState restart identity, atomic snapshot, clear, overflow");
    }
}
