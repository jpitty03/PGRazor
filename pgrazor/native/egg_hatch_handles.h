#pragma once
#include <stdint.h>
namespace pgr_egg_handles {
#ifdef PGR_EGG_HANDLES_WASM_TEST
using Handle=uint64_t;
#else
using Handle=uintptr_t;
static_assert(sizeof(Handle)==8,"Egg hatch requires the validated LP64 handle ABI");
#endif
using Create=Handle(*)(void*,int);
using Target=void*(*)(Handle);
using Free=void(*)(Handle);
inline void release(Handle &h,Free free){Handle old=h;h=0;if(old&&free)free(old);}
}
