// JNI glue for dev.pgrazor.NativeCore.
//
// RegisterNatives binds validation, owner-thread bootstrap and read-only health methods.
// The version script publishes only JNI_OnLoad and pgrazor_native_api.

#include <jni.h>

#include "pgrazor_native_api.h"
#include "pgrazor_internal.h"
#include "runtime_transport_jni.h"
#include "runtime_bootstrap.h"
#include "runtime_host.h"
#include "tm_shortcut.h"
#include "tag_catch.h"
#include "smart_battle.h"
#include "pokemon_storage.h"

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

// Read-only atomic host counters; no IL2CPP/managed calls from Java's transport thread.
jlongArray JNICALL jni_runtime_health(JNIEnv *env,jclass){
    const auto *api=static_cast<const PgrRuntimeHostApi *>(pgrazor_runtime_host_api(1));
    PgrRuntimeHostSample sample{};
    if(!api||api->sample(&sample))return nullptr;
    const jlong values[]={jlong(sample.installed),jlong(sample.stopping),jlong(sample.fault),
        jlong(sample.pulses),jlong(sample.generation),jlong(sample.owner_tid),jlong(sample.ready_mask)};
    jlongArray result=env->NewLongArray(7);
    if(result)env->SetLongArrayRegion(result,0,7,values);
    return result;
}

// Fast/Charged TM rows: [visible, x, y, count] x 3; read from the UI thread, no managed calls.
jfloatArray JNICALL jni_tm_state(JNIEnv *env,jclass){
    PgrTmRow rows[3];pgr_tm_state(rows);jfloat v[12];
    for(int i=0;i<3;i++){v[i*4]=float(rows[i].visible);v[i*4+1]=rows[i].x;v[i*4+2]=rows[i].y;v[i*4+3]=float(rows[i].count);}
    jfloatArray result=env->NewFloatArray(12);if(result)env->SetFloatArrayRegion(result,0,12,v);return result;
}
void JNICALL jni_tm_use(JNIEnv *,jclass,jint row){pgr_tm_request(row);}
void JNICALL jni_tag_catch_config(JNIEnv *env,jclass,jstring config){
    const char *text=config?env->GetStringUTFChars(config,nullptr):nullptr;
    pgr_tag_catch_config(text);if(text)env->ReleaseStringUTFChars(config,text);
}
jstring JNICALL jni_tag_catch_tags(JNIEnv *env,jclass){char names[16384];pgr_tag_catch_tags(names,sizeof(names));return env->NewStringUTF(names);}

jint JNICALL jni_tag_catch_manual_berries(JNIEnv *,jclass){return pgr_tag_catch_manual_berries();}

jstring JNICALL jni_rocket_counters(JNIEnv *env,jclass){std::string t=pgr_smart_battle_counters();return t.empty()?nullptr:env->NewStringUTF(t.c_str());}
jstring JNICALL jni_owned_pokemon_json(JNIEnv *env,jclass){std::string t=pgr_storage_json();return env->NewStringUTF(t.c_str());}
void JNICALL jni_rocket_lineups(JNIEnv *env,jclass,jstring text){
    const char *t=text?env->GetStringUTFChars(text,nullptr):nullptr;
    pgr_smart_battle_lineups(t);if(t)env->ReleaseStringUTFChars(text,t);
}

const JNINativeMethod kMethods[] = {
    {const_cast<char *>("apiVersion"), const_cast<char *>("()I"),
     reinterpret_cast<void *>(&jni_api_version)},
    {const_cast<char *>("policySelfcheck"), const_cast<char *>("()I"),
     reinterpret_cast<void *>(&jni_policy_selfcheck)},
    {const_cast<char *>("bootstrapProbe"),const_cast<char *>("()J"),reinterpret_cast<void *>(&jni_bootstrap_probe)},
    {const_cast<char *>("runtimeStart"),const_cast<char *>("(I)I"),reinterpret_cast<void *>(&jni_runtime_start)},
    {const_cast<char *>("runtimeHealth"),const_cast<char *>("()[J"),reinterpret_cast<void *>(&jni_runtime_health)},
    {const_cast<char *>("tmState"),const_cast<char *>("()[F"),reinterpret_cast<void *>(&jni_tm_state)},
    {const_cast<char *>("tmUse"),const_cast<char *>("(I)V"),reinterpret_cast<void *>(&jni_tm_use)},
    {const_cast<char *>("tagCatchConfig"),const_cast<char *>("(Ljava/lang/String;)V"),reinterpret_cast<void *>(&jni_tag_catch_config)},
    {const_cast<char *>("rocketCounters"),const_cast<char *>("()Ljava/lang/String;"),reinterpret_cast<void *>(&jni_rocket_counters)},
    {const_cast<char *>("ownedPokemonJson"),const_cast<char *>("()Ljava/lang/String;"),reinterpret_cast<void *>(&jni_owned_pokemon_json)},
    {const_cast<char *>("rocketLineups"),const_cast<char *>("(Ljava/lang/String;)V"),reinterpret_cast<void *>(&jni_rocket_lineups)},
    {const_cast<char *>("tagCatchTags"),const_cast<char *>("()Ljava/lang/String;"),reinterpret_cast<void *>(&jni_tag_catch_tags)},
    {const_cast<char *>("tagCatchManualBerries"),const_cast<char *>("()I"),reinterpret_cast<void *>(&jni_tag_catch_manual_berries)},
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
