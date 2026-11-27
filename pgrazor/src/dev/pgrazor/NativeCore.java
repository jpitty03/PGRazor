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

    /** Fast/Charged TM rows on the Pokemon screen: [visible, x, y, count] x 3, positions as screen fractions. */
    public static native float[] tmState();
    /** Starts the game's TM flow for row 0 (Fast) or 1/2 (Charged). */
    public static native void tmUse(int row);

    /** Tag catch config (TagCatchSettings.config); applied by the owner-thread tick. */
    public static native void tagCatchConfig(String config);
    /** Newline-joined names of the player's own Pokemon tags, as last read. */
    public static native String tagCatchTags();
    /** Current Team GO Rocket line-ups for Smart battling (RocketLineups format). */
    public static native void rocketLineups(String text);
    /** Counters picked for the last Rocket battle ("key" line, then "species,form,cp,shadow" lines), or null. */
    public static native String rocketCounters();
    /** Full plain-data Pokemon storage as a JSON array; consumers pick only the fields they need. */
    public static native String ownedPokemonJson();
    /** VGP+ tag rules: berries for the current manual encounter in fallback order, packed 10 bits each from the low end; 0 for none. */
    public static native int tagCatchManualBerries();
}
