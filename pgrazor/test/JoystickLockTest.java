package dev.pgrazor;

public final class JoystickLockTest {
    public static void main(String[] args) {
        check(!JoystickLock.popsOut(0.44), "just inside the pop distance does not pop");
        check(JoystickLock.popsOut(0.45), "pops at the pop distance");
        check(JoystickLock.popsOut(1.0), "fully deflected pops");
        check(JoystickLock.resetsHold(0.34), "well inside resets the hold");
        check(!JoystickLock.resetsHold(0.35), "reset boundary is exclusive");
        check(!JoystickLock.resetsHold(0.40) && !JoystickLock.popsOut(0.40), "gap between thresholds keeps the hold");
        check(JoystickLock.POP_DELAY_MS == 250, "pop delay is 250 ms");
        System.out.println("JoystickLock tests passed");
    }

    private static void check(boolean ok, String name) {
        if (!ok) throw new AssertionError(name);
    }
}
