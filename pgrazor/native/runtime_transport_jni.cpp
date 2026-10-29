#include "runtime_transport_jni.h"
#include "runtime_transport.h"
#include <atomic>
#include <cstdint>
#include <mutex>

namespace {
// The token identifies the single process-lifetime Java transport owner. It is
// NEVER an encoded native address or an authorization secret. Second install,
// failed valid install and stopped owner cannot create/revive another session.
constexpr jlong kOwnerToken=1;
std::mutex lifecycle;
bool attempted=false,active=false;
const PgrRuntimeTransportApi *transport=nullptr;
std::atomic<jmethodID> is_read_only{nullptr};

jlong JNICALL native_install(JNIEnv *,jclass,jint lease,jint events,jint string_bytes,jint budget){
 if(lease<=0||lease>PGR_TRANSPORT_MAX_LEASE_MS||events<=0||events>PGR_TRANSPORT_MAX_EVENTS||
    string_bytes<=0||string_bytes>PGR_TRANSPORT_MAX_STRING||budget<=0||budget>PGR_TRANSPORT_MAX_FRAME)return 0;
 PgrRuntimeTransportConfig config{};config.version=1;config.size=sizeof(config);
 config.lease_ms=static_cast<uint32_t>(lease);config.max_events=static_cast<uint32_t>(events);
 config.max_string_bytes=static_cast<uint32_t>(string_bytes);config.queue_byte_budget=static_cast<uint32_t>(budget);
 std::lock_guard<std::mutex> lock(lifecycle);
 if(attempted)return 0;
 attempted=true;
 const auto *api=static_cast<const PgrRuntimeTransportApi *>(pgrazor_runtime_transport_api(1));
 if(!api||api->version!=1||api->size!=sizeof(PgrRuntimeTransportApi)||!api->install||!api->exchange||!api->stop)return 0;
 if(api->install(&config)!=PGR_TRANSPORT_OK)return 0;
 transport=api;active=true;return kOwnerToken;
}

jint JNICALL native_exchange(JNIEnv *env,jclass,jlong handle,jobject snapshot,jint length,jobject events,jint capacity){
 if(handle!=kOwnerToken)return PGR_TRANSPORT_ERR_NOT_INSTALLED;
 if(!snapshot||!events)return PGR_TRANSPORT_ERR_NULL_ARG;
 if(length<0||capacity<0||length>PGR_TRANSPORT_MAX_FRAME||capacity>PGR_TRANSPORT_MAX_FRAME)return PGR_TRANSPORT_ERR_RANGE;
 if(capacity<PGR_TRANSPORT_EVENT_HEADER)return PGR_TRANSPORT_ERR_BAD_SIZE;
 // ALL JNI/Java interaction happens before lifecycle lock. In particular a
 // managed isReadOnly dispatch must never run under any native transport lock.
 jmethodID readonly=is_read_only.load(std::memory_order_acquire);
 if(!readonly)return PGR_TRANSPORT_ERR_NOT_INSTALLED;
 const jboolean immutable=env->CallBooleanMethod(events,readonly);
 if(env->ExceptionCheck())return PGR_TRANSPORT_ERR_BAD_FRAME; // preserve pending Java exception
 if(immutable)return PGR_TRANSPORT_ERR_BAD_BOUND;
 const jlong input_capacity=env->GetDirectBufferCapacity(snapshot);
 const jlong output_capacity=env->GetDirectBufferCapacity(events);
 if(env->ExceptionCheck())return PGR_TRANSPORT_ERR_BAD_FRAME;
 if(input_capacity<0||output_capacity<0||length>input_capacity||capacity>output_capacity)return PGR_TRANSPORT_ERR_RANGE;
 auto *input=static_cast<const uint8_t *>(env->GetDirectBufferAddress(snapshot));
 auto *output=static_cast<uint8_t *>(env->GetDirectBufferAddress(events));
 if(env->ExceptionCheck())return PGR_TRANSPORT_ERR_BAD_FRAME;
 if((length&&!input)||!output)return PGR_TRANSPORT_ERR_NULL_ARG;
 const uintptr_t in=reinterpret_cast<uintptr_t>(input),out=reinterpret_cast<uintptr_t>(output);
 const uintptr_t in_size=static_cast<uintptr_t>(length),out_size=static_cast<uintptr_t>(capacity);
 if(in_size>UINTPTR_MAX-in||out_size>UINTPTR_MAX-out)return PGR_TRANSPORT_ERR_RANGE;
 // Check the USED byte ranges, including aliases/slices of distinct Java
 // ByteBuffer objects. Zero snapshot length is the native drain-only contract.
 if(in_size&&in<out+out_size&&out<in+in_size)return PGR_TRANSPORT_ERR_BAD_BOUND;
 uint32_t written=0;
 int rc;
 {
  // Native table operations are documented callback-free. This short wrapper
  // lock linearizes exchange/stop without retaining any lock across JNI or
  // managed dispatch, and prevents a checked-active exchange racing terminal stop.
  std::lock_guard<std::mutex> lock(lifecycle);
  if(!active||!transport)return attempted?PGR_TRANSPORT_STOPPED:PGR_TRANSPORT_ERR_NOT_INSTALLED;
  rc=transport->exchange(input,static_cast<uint32_t>(length),output,static_cast<uint32_t>(capacity),&written);
 }
 if(rc!=PGR_TRANSPORT_OK)return rc<0?rc:PGR_TRANSPORT_ERR_BAD_FRAME;
 if(written<PGR_TRANSPORT_EVENT_HEADER||written>static_cast<uint32_t>(capacity)||written>PGR_TRANSPORT_MAX_FRAME)return PGR_TRANSPORT_ERR_BAD_FRAME;
 return static_cast<jint>(written);
}
void JNICALL native_stop(JNIEnv *,jclass,jlong handle){
 if(handle!=kOwnerToken)return;
 std::lock_guard<std::mutex> lock(lifecycle);
 if(!active||!transport)return;
 active=false; // terminal even if lower-level stop reports a fault
 transport->stop();
}
const JNINativeMethod methods[]={
 {const_cast<char *>("nativeInstall"),const_cast<char *>("(IIII)J"),reinterpret_cast<void *>(native_install)},
 {const_cast<char *>("nativeExchange"),const_cast<char *>("(JLjava/nio/ByteBuffer;ILjava/nio/ByteBuffer;I)I"),reinterpret_cast<void *>(native_exchange)},
 {const_cast<char *>("nativeStop"),const_cast<char *>("(J)V"),reinterpret_cast<void *>(native_stop)}
};
static_assert(sizeof(PgrRuntimeTransportConfig)==32,"transport JNI config ABI");
}
int pgrazor_register_runtime_transport(JNIEnv *env){
 if(!env)return -1;
 jclass klass=env->FindClass("dev/pgrazor/NativeRuntimeTransport");
 if(!klass)return -1;
 jclass buffer=env->FindClass("java/nio/ByteBuffer");
 if(!buffer){env->DeleteLocalRef(klass);return -1;}
 jmethodID readonly=env->GetMethodID(buffer,"isReadOnly","()Z");
 env->DeleteLocalRef(buffer);
 if(!readonly){env->DeleteLocalRef(klass);return -1;}
 is_read_only.store(readonly,std::memory_order_release);
 const jint rc=env->RegisterNatives(klass,methods,sizeof(methods)/sizeof(methods[0]));
 env->DeleteLocalRef(klass);
 return rc==JNI_OK?0:-1;
}
