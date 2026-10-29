#pragma once
#include <stdint.h>
// Feature57: compiled result/error callback dispatch for feature23. Exact game
// ABI: instance void zlc(ReleasePokemonOutProto), zld(System.String), followed
// by MethodInfo*. No JavaScript callback or event draining inside either hook.
// Caller validates metadata and relinquishes both Gum targets before install.
// Hooks/trampolines remain process lifetime, including partial install failure.
struct PgrTransferObserversConfig {
 uint32_t version,size;
 void *result_address,*error_address;
 void *thread_current,*gchandle_new,*gchandle_get_target,*gchandle_free;
};
struct PgrTransferObserversApi {
 uint32_t version,size;
 int (*install)(const PgrTransferObserversConfig *);
 int (*installed)();
};
static_assert(sizeof(PgrTransferObserversConfig)==56,"Transfer observers config");
static_assert(sizeof(PgrTransferObserversApi)==24,"Transfer observers API");
extern "C" const void *pgrazor_transfer_observers_api(uint32_t);
