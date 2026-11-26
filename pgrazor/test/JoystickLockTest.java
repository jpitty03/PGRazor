package dev.pgrazor;

public final class JoystickLockTest {
    public static void main(String[] args) {
        testThresholds();
        testNormalDragStaysUnlocked();
        testCancellation();
        testUnlockLongPressOnPadlock();
        testCenterLongPressAndDragLock();
        testMultiTouchPointer();
        testCameraLossIdleWalkImmunity();
        System.out.println("JoystickLock tests passed");
    }

    private static void check(boolean ok, String name) {
        if (!ok) throw new AssertionError(name);
    }

    private static void testThresholds() {
        check(JoystickLock.CENTER_RADIUS == 0.36, "center hold radius is 0.36");
        check(JoystickLock.CENTER_DRAG_MIN == 0.15, "center drag min is 0.15");
        check(JoystickLock.CENTER_HOLD_MS == 1500, "center hold delay is 1500 ms");
        check(JoystickLock.PADLOCK_OFFSET == 0.65, "padlock sits at 0.65 radius");
        check(JoystickLock.PADLOCK_HIT_RADIUS == 0.50, "padlock hit radius is 0.50");
        check(JoystickLock.UNLOCK_HOLD_MS == 500, "padlock unlock hold delay is 500 ms");
    }

    private static void testNormalDragStaysUnlocked() {
        JoystickLock.State s = new JoystickLock.State();
        s.onDown(0, 0, 0);
        s.onMove(1.00, 0.5);
        s.onMove(1.50, 0.8);
        check(!s.holdingCenter && !s.centerArmed && !s.isLocked, "dragging out past boundary does not arm or lock");
        check(!s.onPointerUp(0), "releasing normal drag does not lock");
        check(!s.isLocked && !s.isHeld() && s.pointer == -1, "stick releases normally");
    }

    private static void testCancellation() {
        // 1. Cancel during center hold before 1.5s: clears hold without lock.
        JoystickLock.State s1 = new JoystickLock.State();
        s1.onDown(0, 0, 0);
        check(s1.holdingCenter, "center hold started");
        s1.onCancel();
        check(!s1.isLocked && !s1.holdingCenter && !s1.centerArmed && !s1.isHeld(), "cancel clears hold");

        // 2. Cancel during center-armed drag: clears hold without locking.
        JoystickLock.State sc = new JoystickLock.State();
        sc.onDown(0, 0, 0);
        check(sc.onCenterTimerFired(), "center hold armed");
        sc.onMove(0.6, 1.0);
        sc.onCancel();
        check(!sc.isLocked && !sc.centerArmed && !sc.centerDragged && !sc.isHeld(), "cancel during center drag clears all");

        // 3. Cancel while locked: releases lock immediately.
        JoystickLock.State s3 = new JoystickLock.State();
        s3.lockTo(1.5);
        check(s3.isLocked && s3.isHeld(), "stick is locked");
        s3.onCancel();
        check(!s3.isLocked && !s3.isHeld(), "cancel while locked unlocks stick");
    }

    private static void testUnlockLongPressOnPadlock() {
        JoystickLock.State s = new JoystickLock.State();
        s.lockTo(0.0); // padlock sits at (0.65, 0.0)
        check(s.isLocked && s.isHeld(), "stick locked");

        // Touching center or opposite side does not target the padlock.
        check(!s.onDown(0, 0.0, 0.0), "touching disc center while locked is ignored");
        check(!s.onDown(0, -0.65, 0.0), "touching opposite side of disc is ignored");
        check(!s.unlockHolding && !s.onUnlockTimerFired() && s.isLocked, "off-padlock press cannot unlock");

        // Short tap on padlock does not unlock.
        check(s.onDown(0, 0.65, 0.05), "touching padlock captures pointer");
        check(s.unlockHolding, "padlock hold started");
        check(!s.onPointerUp(0), "releasing before long-press finishes keeps lock");
        check(s.isLocked && !s.unlockHolding && s.pointer == -1, "early lift leaves stick locked");

        // Dragging off padlock cancels unlock hold.
        check(s.onDown(0, 0.65, 0.0), "padlock press starts");
        check(!s.onLockedMove(-0.2, 0.0), "moving off padlock cancels unlock hold");
        check(!s.unlockHolding && !s.onUnlockTimerFired() && s.isLocked, "moved-off press cannot unlock");
        check(!s.onPointerUp(0) && s.isLocked, "lifting after moving off leaves stick locked");

        // Long-pressing the padlock unlocks.
        check(s.onDown(0, 0.65, 0.0), "padlock press starts");
        check(s.onLockedMove(0.70, 0.05), "small jitter within padlock target preserves hold");
        check(s.onUnlockTimerFired(), "padlock long press unlocks stick");
        check(!s.isLocked && !s.isHeld() && !s.unlockHolding, "stick unlocked and released");
    }

    private static void testCenterLongPressAndDragLock() {
        // 1. Touching outside center does not arm center lock.
        JoystickLock.State off = new JoystickLock.State();
        check(off.onDown(0, 0.55, 0.0) && !off.holdingCenter, "off-center touch does not start center hold");
        check(!off.onCenterTimerFired() && !off.centerArmed, "center timer ignored when not holding center");

        // 2. Moving out of center before 1.5s cancels center hold.
        JoystickLock.State movedEarly = new JoystickLock.State();
        check(movedEarly.onDown(0, 0.05, 0.05) && movedEarly.holdingCenter, "center touch starts center hold");
        movedEarly.onMove(0.45, 0.0);
        check(!movedEarly.holdingCenter, "leaving center before 1.5s cancels center hold");
        check(!movedEarly.onCenterTimerFired() && !movedEarly.centerArmed, "expired hold after leaving does not arm");

        // 3. Holding center 1.5s then lifting without dragging does not lock.
        JoystickLock.State noDrag = new JoystickLock.State();
        check(noDrag.onDown(0, 0.10, 0.0) && noDrag.holdingCenter, "inside center knob");
        noDrag.onMove(0.20, 0.0);
        check(noDrag.holdingCenter, "minor jitter inside center knob keeps timer alive");
        check(noDrag.onCenterTimerFired(), "1.5s center hold arms center lock");
        check(noDrag.centerArmed && !noDrag.centerDragged, "armed without drag direction yet");
        check(!noDrag.onPointerUp(0) && !noDrag.isLocked && !noDrag.isHeld(), "lifting without dragging cancels lock");

        // 4. Holding center 1.5s, dragging in a direction, and lifting locks there.
        JoystickLock.State lock = new JoystickLock.State();
        lock.onDown(0, 0.0, 0.0);
        check(lock.onCenterTimerFired(), "1.5s center hold arms lock");
        lock.onMove(0.60, 0.75);
        check(lock.centerDragged, "tracks direction");
        lock.onMove(0.85, 1.25);
        check(lock.onPointerUp(0), "releasing after dragging engages lock");
        check(lock.isLocked && !lock.centerArmed && Math.abs(lock.lockAngle - 1.25) < 1e-9, "locked to dragged direction");

        // 5. Dragging out after 1.5s then pulling back to deadzone cancels lock.
        JoystickLock.State cancelDeadzone = new JoystickLock.State();
        cancelDeadzone.onDown(0, 0.0, 0.0);
        check(cancelDeadzone.onCenterTimerFired(), "center hold armed");
        cancelDeadzone.onMove(0.70, 0.5);
        check(cancelDeadzone.centerDragged, "direction recorded");
        cancelDeadzone.onMove(0.05, 0.0);
        check(!cancelDeadzone.centerDragged, "returning to deadzone clears drag direction");
        check(!cancelDeadzone.onPointerUp(0) && !cancelDeadzone.isLocked, "releasing in deadzone does not lock");
    }

    private static void testMultiTouchPointer() {
        JoystickLock.State s = new JoystickLock.State();
        s.onDown(0, 0, 0);
        check(s.pointer == 0 && s.isHeld(), "pointer 0 active");
        check(!s.onPointerUp(1), "secondary pointer lift does not lock or stop");
        check(s.pointer == 0 && s.isHeld(), "secondary pointer lift does not stop active stick");
        check(!s.onPointerUp(0), "active pointer lift releases normally");
        check(s.pointer == -1 && !s.isHeld(), "active pointer lift releases");
    }

    private static void testCameraLossIdleWalkImmunity() {
        MotionEngine e = new MotionEngine();
        e.position(37.7749, -122.4194);
        e.enabled = true;
        e.speed(5.0);
        e.setIdleWalking(true);

        JoystickLock.State s = new JoystickLock.State();
        s.lockTo(Math.PI / 2); // locked north
        check(s.isHeld(), "lock reports stick held");

        e.input(0, 0);
        e.setIdleWalkingAllowed(!s.isHeld());
        for (int i = 0; i < 100; i++) e.step(0.1);
        check(!e.idleWalkingActive(), "camera loss while locked must never trigger idle walking");
        check(e.latitude == 37.7749 && e.longitude == -122.4194, "position unchanged during camera loss");

        s.unlock();
        check(!s.isHeld(), "stick released");
        e.setIdleWalkingAllowed(!s.isHeld());
        for (int i = 0; i < 35; i++) e.step(0.1);
        check(e.idleWalkingActive(), "idle walking starts normally after unlock");

        s.lockTo(0);
        e.setIdleWalkingAllowed(!s.isHeld());
        e.step(0.1);
        check(!e.idleWalkingActive(), "re-locking stick immediately halts idle walking");
    }
}
