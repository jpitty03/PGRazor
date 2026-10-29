package dev.pgrazor;

import java.lang.reflect.Method;

/** Version 1 host-ART boundary. Loaded only in the process ABI, before guest JNI. */
public final class NativeJavaHooks {
    public static final int VERSION = 1;
    private NativeJavaHooks() {}
    public static native boolean ready();
    static native Method hook(Method target, Object owner, Method callback);
    static native boolean deoptimize(Method method);
}
