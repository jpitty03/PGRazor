// JNI glue for dev.pgrazor.NativeCore.
//
// Binds the two host-side validation methods with RegisterNatives from JNI_OnLoad so
// no mangled JNI_* symbols are exported; the version script publishes only JNI_OnLoad
// and pgrazor_native_api. Host-only policy self-check: no Dobby, no device, no process
// image mutation here.

#include <jni.h>

#include "pgrazor_native_api.h"
#include "pgrazor_internal.h"
#include "runtime_transport_jni.h"
#include "runtime_bootstrap.h"
#include "runtime_host.h"

namespace {

jint JNICALL jni_api_version(JNIEnv *, jclass) {
    return static_cast<jint>(PGRAZOR_NATIVE_API_VERSION);
}

jint JNICALL jni_policy_selfcheck(JNIEnv *, jclass) {
    return static_cast<jint>(pgrazor_policy_selfcheck());
}

jlong JNICALL jni_bootstrap_probe(JNIEnv *,jclass){
    const auto *api=static_cast<const PgrBootApi *>(pgrazor_runtime_bootstrap_api(1));
    PgrBootConfig config{};config.version=1;config.size=sizeof(config);config.reserved=1;
    PgrBootBinding result{};const int status=api->probe(&config,&result);
    const uint64_t packed=(static_cast<uint64_t>(static_cast<uint32_t>(status))<<32)|static_cast<uint32_t>(result.detail);
    return static_cast<jlong>(packed);
}

jint JNICALL jni_runtime_start(JNIEnv *,jclass,jint exclusive_owner){
    const auto *api=static_cast<const PgrRuntimeHostApi *>(pgrazor_runtime_host_api(1));
    PgrRuntimeHostConfig config{1,sizeof(config),static_cast<uint32_t>(exclusive_owner),0};
    return api->configure(&config);
}

const JNINativeMethod kMethods[] = {
    {const_cast<char *>("apiVersion"), const_cast<char *>("()I"),
     reinterpret_cast<void *>(&jni_api_version)},
    {const_cast<char *>("policySelfcheck"), const_cast<char *>("()I"),
     reinterpret_cast<void *>(&jni_policy_selfcheck)},
    {const_cast<char *>("bootstrapProbe"),const_cast<char *>("()J"),reinterpret_cast<void *>(&jni_bootstrap_probe)},
    {const_cast<char *>("runtimeStart"),const_cast<char *>("(I)I"),reinterpret_cast<void *>(&jni_runtime_start)},
};

} // namespace

extern "C" __attribute__((visibility("default")))
jint JNI_OnLoad(JavaVM *vm, void *) {
    JNIEnv *env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK || env == nullptr) {
        return JNI_ERR;
    }

    jclass clazz = env->FindClass("dev/pgrazor/NativeCore");
    if (clazz == nullptr) return JNI_ERR;

    if (env->RegisterNatives(clazz, kMethods,
                             static_cast<jint>(sizeof(kMethods) / sizeof(kMethods[0]))) != 0) {
        return JNI_ERR;
    }

    env->DeleteLocalRef(clazz);
    if(pgrazor_register_runtime_transport(env)!=0)return JNI_ERR;
    return JNI_VERSION_1_6;
}
