#define PGR_EGG_HANDLES_WASM_TEST
#include "egg_hatch_handles.h"
using namespace pgr_egg_handles;
namespace {
constexpr Handle high=UINT64_C(0xfedcba9876543210);
Handle live=0;int frees=0,ok=1;
Handle create(void *p,int pinned){if(!p||pinned!=1)ok=0;return high;}
void *target(Handle h){if(h!=high)ok=0;return reinterpret_cast<void*>(1234);}
void free_handle(Handle h){if(h!=high||live!=0)ok=0;frees++;release(live,free_handle);}
}
extern "C" int run(){Create a=create;Target b=target;Free c=free_handle;live=a(reinterpret_cast<void*>(1234),1);if(live!=high||b(live)!=reinterpret_cast<void*>(1234))ok=0;release(live,c);release(live,c);return ok&&frees==1&&live==0;}
