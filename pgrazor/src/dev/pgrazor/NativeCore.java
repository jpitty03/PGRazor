package dev.pgrazor;

/**
 * Minimal Java surface for the native migration pilot (libpgrazor-core).
 *
 * <p>The native library binds these methods with {@code RegisterNatives} from its
 * {@code JNI_OnLoad}, so there are no name-mangled JNI exports and the version script
 * only publishes {@code JNI_OnLoad} and {@code pgrazor_native_api}. The gameplay runtime
 * (Frida shim) talks to the native layer through the {@code pgrazor_native_api} C table;
 * this Java class exists only for host-side validation.
 *
 * <p>The coordinator loads the correct host .so for the current ABI before calling any
 * method here. Both calls are pure/deterministic and do not touch Dobby, the process
 * image, or any connected device.
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
}
