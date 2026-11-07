package dev.pgrazor;

import java.nio.ByteBuffer;
import java.util.function.Consumer;

/**
 * Stage 0 of the rooted (Zygisk) build. The Zygisk loader calls {@link #start} in the game's main
 * process after specialization, before ActivityThread.main, through a throwaway loader whose parent
 * is the system loader.
 *
 * <p>This class must reference only JDK and Android classes. It creates the real PGRazor class
 * loader, whose parent resolves framework classes normally and game classes through the game's own
 * class loader once that exists, then hands control to {@code RootedRuntime} inside it.
 */
public final class RootedEntry {
    private RootedEntry() {}

    public static void start(byte[] dex, byte[] payload, String dataDir) throws Exception {
        GameDelegate parent = new GameDelegate();
        ClassLoader runtime = new dalvik.system.InMemoryDexClassLoader(ByteBuffer.wrap(dex), parent);
        Class.forName("dev.pgrazor.RootedRuntime", true, runtime)
            .getMethod("start", byte[].class, String.class, Consumer.class)
            .invoke(null, payload, dataDir, parent);
    }

    /** Parent of the PGRazor loader: framework classes first, then the game's loader once published. */
    static final class GameDelegate extends ClassLoader implements Consumer<ClassLoader> {
        private volatile ClassLoader game;

        GameDelegate() { super(ClassLoader.getSystemClassLoader()); }

        /** Publishes the game's class loader once; later calls are ignored. */
        @Override public synchronized void accept(ClassLoader loader) {
            if (game == null && loader != null) game = loader;
        }

        @Override protected Class<?> findClass(String name) throws ClassNotFoundException {
            ClassLoader target = game;
            // PGRazor classes always come from PGRazor's own dex, never from the game.
            if (target == null || name.startsWith("dev.pgrazor.")) throw new ClassNotFoundException(name);
            return target.loadClass(name);
        }
    }
}
