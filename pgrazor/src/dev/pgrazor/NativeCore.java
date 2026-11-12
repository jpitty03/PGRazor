package dev.pgrazor;

/**
 * Java bootstrap surface for the compiled native gameplay runtime.
 * JNI_OnLoad binds these methods with RegisterNatives after the ABI-specific library loads.
 * bootstrapProbe discovers IL2CPP on Unity's rendered thread; runtimeStart installs the
 * process-lifetime scheduler. Java transport/location readiness alone does not attest gameplay readiness.
 */
public final class NativeCore {
    private NativeCore() {}

    /** Returns the native C-API table version the loaded library implements (see pgrazor_native_api.h). */
    public static native int apiVersion();

    /**
     * Runs the deterministic spawn-policy self-check (radius floor + enable/allow/deadline gate).
     * Returns 0 on success, or the id of the first failing case. No hooking is performed.
     */
    public static native int policySelfcheck();

    /** Compiled discovery only: high32=status, low32=metadata detail; no hooks installed. */
    public static native long bootstrapProbe();

    /** Owner-thread scheduler startup; exactly1 attests no competing gameplay hook owners. */
    public static native int runtimeStart(int exclusiveOwner);

    /** Atomic runtime counters only; safe off the Unity thread, never invokes game methods. */
    public static native long[] runtimeHealth();
}
