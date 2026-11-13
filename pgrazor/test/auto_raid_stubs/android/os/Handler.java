package android.os;

import java.util.ArrayList;
import java.util.List;

/**
 * Boundary shell for the main-thread handler. Posted work (the AutoRaid travel
 * runnable) is queued and runs only when a test drains it explicitly, so tests
 * can observe the controller's deferred, main-thread re-validation of a travel
 * after state has changed between post and execution.
 */
public class Handler {
    public static final List<Runnable> queue = new ArrayList<>();
    public Handler(Looper looper) {}
    public boolean post(Runnable r) { queue.add(r); return true; }
    public static void drain() { while (!queue.isEmpty()) queue.remove(0).run(); }
}
