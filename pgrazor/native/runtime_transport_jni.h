#pragma once
#include <jni.h>

// Hidden JNI_OnLoad helper. Returns0 only when all three exact private static
// NativeRuntimeTransport natives were registered. Missing class/method is a hard
// registration failure: caller MUST return JNI_ERR, not silently omit transport.
// No Java class/global object is retained; ByteBuffer's boot-class method ID is
// VM-lifetime metadata. Production must ship NativeRuntimeTransport even in a
// diagnostic APK that loads this newer core.
int pgrazor_register_runtime_transport(JNIEnv *env);
