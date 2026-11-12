// x86_64 fixture: actual installer bookkeeping with a deterministic hook seam.
// Run each scenario 0..4 in its own process; ARM veneer execution belongs to unity_scheduler_test.
#include "unity_scheduler.h"
#include "hook_engine.h"
#include "runtime_metadata.h"
#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <unistd.h>
namespace fixture {
int mode,calls,token,method;
int hook(void *target,void*,void **original){
    ++calls;
    if((mode==1&&calls==1)||(mode==3&&calls==2)){*original=nullptr;return -7;}
    if((mode==2&&calls==1)||(mode==4&&calls==2)){*original=nullptr;return 0;}
    *original=target;return 0;
}
void *attached(){return &token;}
int verify(const PgrUnitySchedulerConfig*){return 1;}
void *invoke(const void*,void*,void**,void**){return nullptr;}
void *convert(const void*,void*,void**,int32_t,void**){return nullptr;}
}
#define pgrazor_hook fixture::hook
#include "unity_scheduler.cpp"
int main(int argc,char **argv){
    assert(argc==2&&argv[1][0]>='0'&&argv[1][0]<='4'&&argv[1][1]=='\0');fixture::mode=std::atoi(argv[1]);
    const auto *api=static_cast<const PgrUnitySchedulerApi4*>(pgrazor_unity_scheduler_api(4));assert(api);
    PgrUnitySchedulerConfig c{};c.version=2;c.size=sizeof(c);c.runtime_invoke=reinterpret_cast<void*>(&fixture::invoke);
    c.runtime_invoke_convert_args=reinterpret_cast<void*>(&fixture::convert);c.execute_tasks=&fixture::method;
    c.thread_current=fixture::attached;c.verify_metadata=fixture::verify;c.expected_owner_tid=gettid();c.exclusive_owner=1;
    const int result=api->install(&c);PgrUnitySchedulerDiag d{};assert(api->diagnostics(&d)==0);
    if(fixture::mode==0){assert(result==0&&d.installed&&!d.poisoned&&d.failed_stage==PGR_US_STAGE_NONE);}
    else {
        assert(result==PGR_US_HOOK_FAILED&&!d.installed&&d.poisoned);
        assert(d.failed_stage==(fixture::mode<=2?PGR_US_STAGE_INVOKE_HOOK:PGR_US_STAGE_CONVERT_HOOK));
        // A successful installer result with a null trampoline must remain rc=0, not an invented error code.
        assert((fixture::mode<=2?d.invoke_hook_rc:d.convert_hook_rc)==(fixture::mode%2?-7:0));
        assert(api->install(&c)==PGR_US_POISONED);
        PgrUnitySchedulerDiag again{};assert(api->diagnostics(&again)==0&&again.failed_stage==d.failed_stage);
    }
    puts("PASS scheduler diagnostics: raw installer result, failed entrypoint, poison latch");
}
