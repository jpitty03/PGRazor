package dev.pgrazor;

// Lock gesture thresholds and state machine, shared by Pad and JoystickLockTest.
final class JoystickLock {
    static final double CENTER_RADIUS = 0.36;
    static final double CENTER_DRAG_MIN = 0.15;
    static final long CENTER_HOLD_MS = 1500;
    static final double PADLOCK_OFFSET = 0.65;
    static final double PADLOCK_HIT_RADIUS = 0.50;
    static final long UNLOCK_HOLD_MS = 500;

    private JoystickLock() {}

    /** Pure state machine for joystick lock transitions, testable without Android runtime. */
    static final class State {
        boolean isLocked;
        boolean holdingCenter;
        boolean centerArmed;
        boolean centerDragged;
        boolean unlockHolding;
        double lockAngle;
        int pointer = -1;

        boolean isOnPadlock(double normX, double normY) {
            if (!isLocked) return false;
            double px = Math.cos(lockAngle) * PADLOCK_OFFSET;
            double py = Math.sin(lockAngle) * PADLOCK_OFFSET;
            return Math.hypot(normX - px, normY - py) <= PADLOCK_HIT_RADIUS;
        }

        /** Handles touch down in normalized disc units; returns false to ignore touches off the padlock while locked. */
        boolean onDown(int pointerId, double normX, double normY) {
            if (isLocked) {
                if (!isOnPadlock(normX, normY)) return false;
                pointer = pointerId;
                unlockHolding = true;
                return true;
            }
            pointer = pointerId;
            centerArmed = false;
            centerDragged = false;
            holdingCenter = Math.hypot(normX, normY) <= CENTER_RADIUS;
            return true;
        }

        /** Tracks pointer movement while locked; returns false once the finger leaves the padlock hit target. */
        boolean onLockedMove(double normX, double normY) {
            if (!isLocked || pointer < 0) return false;
            if (!isOnPadlock(normX, normY)) unlockHolding = false;
            return unlockHolding;
        }

        /** Updates drag state; leaves center hold active only while the finger stays inside the centre knob. */
        void onMove(double dist, double angle) {
            if (isLocked || pointer < 0) return;
            if (holdingCenter && dist > CENTER_RADIUS) holdingCenter = false;
            if (centerArmed) {
                if (dist >= CENTER_DRAG_MIN) {
                    centerDragged = true;
                    lockAngle = angle;
                } else if (dist < 0.08) {
                    centerDragged = false;
                }
            }
        }

        boolean onCenterTimerFired() {
            if (pointer >= 0 && holdingCenter && !isLocked && !centerArmed) {
                holdingCenter = false;
                centerArmed = true;
                centerDragged = false;
                return true;
            }
            return false;
        }

        boolean onUnlockTimerFired() {
            if (isLocked && unlockHolding && pointer >= 0) {
                unlock();
                return true;
            }
            return false;
        }

        void lockTo(double angle) {
            isLocked = true;
            holdingCenter = false;
            centerArmed = false;
            centerDragged = false;
            unlockHolding = false;
            lockAngle = angle;
            pointer = -1;
        }

        void unlock() {
            release();
        }

        void release() {
            isLocked = false;
            holdingCenter = false;
            centerArmed = false;
            centerDragged = false;
            unlockHolding = false;
            pointer = -1;
        }

        void onCancel() {
            release();
        }

        /** Handles pointer lift; returns true when this lift engages the directional lock. */
        boolean onPointerUp(int pointerId) {
            if (pointer != pointerId) return false;
            if (isLocked) {
                pointer = -1;
                unlockHolding = false;
                return false;
            }
            if (centerArmed && centerDragged) {
                lockTo(lockAngle);
                return true;
            }
            release();
            return false;
        }

        boolean isHeld() {
            return pointer >= 0 || isLocked;
        }
    }
}
