package dev.pgrazor;

// Lock gesture thresholds, shared by Pad and JoystickLockTest.
final class JoystickLock {
    static final double POP_DISTANCE = 0.45;
    static final double RESET_DISTANCE = 0.35;
    static final long POP_DELAY_MS = 250;
    private JoystickLock() {}
    static boolean popsOut(double distance) { return distance >= POP_DISTANCE; }
    static boolean resetsHold(double distance) { return distance < RESET_DISTANCE; }
}
